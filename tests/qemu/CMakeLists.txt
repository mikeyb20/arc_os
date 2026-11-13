# arc_os — QEMU boot tests (M0-02). Run with `ctest -L qemu`.
#
# Each test boots a test ISO in kernel test mode (arc.test=<suite>) through
# tools/qemu-test.py. They need the cross build first:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake && cmake --build build
# Label `slow` is reserved for long tests (none yet).

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "python3 not found: QEMU tests disabled")
    return()
endif()

set(_qemu_test ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/qemu-test.py
    --build-dir ${ARC_CROSS_BUILD_DIR})

# Boot, run hello, then the shell non-interactively on smoke.sh: an external
# command, the ls/stat/cd/pwd/run/id builtins (getdents64, stat, wait4 by pid)
# Boots with -m 4G: the boot_free_bytes budget is measured here (M0-08).
add_test(NAME qemu.smoke COMMAND ${_qemu_test} --suite smoke --memory 4G
    --module tests/qemu/suites/smoke.sh
    --expect "arc_os shell"
    --expect "(?s)\\]\\$ .*Hello from exec'd binary"
    --expect "-rw-rw-rw- +[0-9]+ shell"
    --expect "Type: directory"
    --expect "\\[/boot\\]\\$ /boot"
    --expect "exited with status 0"
    --expect "uid=0 gid=0")

# M1-02: the buddy allocator hands out an aligned 2 MiB block after boot (-m 4G default)
add_test(NAME qemu.mem.buddy COMMAND ${_qemu_test} --suite smoke --name mem-buddy
    --module tests/qemu/suites/smoke.sh
    --expect "\\[PMM\\] Free blocks per order: 0:"
    --expect "\\[PMM\\] Self-test passed: 2 MiB block")

# M1-02: arc.mem=4G on a 6 GiB guest uses exactly 4 GiB (low-tier floor on a bigger machine)
add_test(NAME qemu.mem.cap COMMAND ${_qemu_test} --suite smoke --name mem-cap
    --module tests/qemu/suites/smoke.sh --memory 6G --cmdline arc.mem=4G
    --expect "\\[PMM\\] arc.mem: using 4096 MiB of RAM"
    --expect "\\[PMM\\] Self-test passed")

# Every existing user program, run once
add_test(NAME qemu.userland COMMAND ${_qemu_test} --suite userland)

# Linux syscall ABI (M0-05): getdents64, stat, wait4, rt_sigaction, ioctl, brk, -ENOSYS;
# process lifecycle (M0-10): 200 children forked and reaped, orphans adopted by init
add_test(NAME qemu.abi COMMAND ${_qemu_test} --suite abi --module abi_check --module forkreap
    --expect "forkreap: 200 children reaped, 0 failures")

# Linux process start (M0-06): stock musl static and static-PIE programs and
# the same test built with libarc: argv, envp, auxv, TLS, stdio, files, tty
add_test(NAME qemu.musl COMMAND ${_qemu_test} --suite musl
    --module start_musl --module start_musl_pie --module start_arc
    --expect "start_basic\\(musl\\): PASS"
    --expect "start_basic\\(libarc\\): PASS"
    --expect "writev ok")

# FPU state and signal frames (M1-01, ADR-0003): AVX2 registers survive
# preemption and signals; Linux rt_sigframe with musl's sigaction
add_test(NAME qemu.fpu COMMAND ${_qemu_test} --suite fpu --module sig_fpu --module fpu_avx
    --expect "sig_fpu: 0 failures"
    --expect "fpu_avx: 0 failures"
    --expect "\\[FPU\\] XSAVE on")

# CPU floor (M1-01): on a CPU without AVX2 the kernel says why and stops
add_test(NAME qemu.cpufloor COMMAND ${_qemu_test} --suite cpufloor --cpu Nehalem
    --expect-exit 0x7e --timeout 60
    --expect "CPU floor: this CPU lacks: .*AVX2")

# Per-CPU entry and APIC modes (M1-03): every suite boots with -smp 4; these two
# pin the APIC mode, and each AP reads its own CPU index back through GS.
# x2APIC needs KVM (QEMU before 9.0 has none under TCG), so CI runs it and TCG
# sessions skip it; CI's default `-cpu host` runs every other suite in x2APIC too.
set(_smp_expect
    --expect "\\[SMP\\] CPU 1 \\(APIC [0-9]+\\) online, GS index 1$"
    --expect "\\[SMP\\] CPU 2 \\(APIC [0-9]+\\) online, GS index 2$"
    --expect "\\[SMP\\] CPU 3 \\(APIC [0-9]+\\) online, GS index 3$")
add_test(NAME qemu.smp.xapic COMMAND ${_qemu_test} --suite userland --name smp-xapic --cpu-flags=-x2apic
    ${_smp_expect}
    --expect "\\[APIC\\] xAPIC mode"
    --expect "\\[SMP\\] All 4 CPUs online \\(xAPIC\\)")
add_test(NAME qemu.smp.x2apic COMMAND ${_qemu_test} --suite userland --name smp-x2apic --cpu-flags=+x2apic
    --require-kvm
    ${_smp_expect}
    --expect "\\[APIC\\] x2APIC mode"
    --expect "\\[SMP\\] All 4 CPUs online \\(x2APIC\\)")
set_tests_properties(qemu.smp.x2apic PROPERTIES SKIP_RETURN_CODE 77)

# SMP scheduler (M1-05): 64 CPU-bound processes over all 4 CPUs, 10 000
# fork/exec/wait rounds with no leak (the suite's budgets: 0 pages, 0 heap
# bytes), a pipe ping-pong across CPUs; the debug build's lock checker stops a
# deliberately inverted pair of spinlocks
add_test(NAME qemu.sched COMMAND ${_qemu_test} --suite sched --module schedtest --timeout 400
    --expect "\\[SCHED\\] 4 CPUs scheduling"
    --expect "schedtest: spread 64 processes over 4 CPUs"
    --expect "schedtest: forkexec 10000 rounds"
    --expect "schedtest: pingpong 10000 round trips"
    --expect "ARC_TEST budget pages_leaked 0$"
    --expect "ARC_TEST budget heap_leaked_bytes 0$")
# About 2 minutes under TCG (10 000 fork/exec rounds), seconds under KVM
set_tests_properties(qemu.sched PROPERTIES TIMEOUT 420)
add_test(NAME qemu.lockorder COMMAND ${_qemu_test} --suite lockorder --expect-panic --timeout 30
    --expect "LOCK ORDER VIOLATION: taking test outer \\(page tables\\) \\(level 50\\)"
    --expect "held: test inner \\(wait queue\\) \\(level 80\\)")

# Harness self-checks: a failing program is reported by name and reason,
# and a kernel panic ends the run within seconds instead of at the timeout.
add_test(NAME qemu.selftest.fail COMMAND ${_qemu_test} --suite selftest --module testfail)
set_tests_properties(qemu.selftest.fail PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL testfail exit=3")
add_test(NAME qemu.selftest.panic COMMAND ${_qemu_test} --suite kpanic --expect-panic --timeout 30)

# Panic screen (M1-04): a real page fault (arc.panic=fault, test mode only) prints
# the reason, registers, CR2 and a backtrace with symbols from the build's table
add_test(NAME qemu.panic.fault COMMAND ${_qemu_test} --suite kpanic --name panic-fault --cmdline arc.panic=fault
    --expect-panic --timeout 30
    --expect "PANIC: Page Fault on CPU 0"
    --expect "CR2 +0000000000000010"
    --expect "Backtrace:"
    --expect "#0 +ffffffff[0-9a-f]+ panic_test_fault\\+0x"
    --expect "kmain\\+0x")

# Bring-up diagnostics (M1-04): boot report, self-test and benchmark run in the
# kernel instead of init and end the run themselves (ARC_TEST DONE)
add_test(NAME qemu.diag COMMAND ${_qemu_test} --suite diag
    --cmdline arc.report=1 --cmdline arc.selftest=1 --cmdline arc.bench=1
    --expect "\\[REPORT\\] Machine +QEMU"
    --expect "\\[REPORT\\] CPUs +4 online; MADT lists 4"
    --expect "\\[REPORT\\] TSC +[0-9]+ kHz"
    --expect "\\[REPORT\\] Memory +[0-9]+ MB RAM"
    --expect "\\[REPORT\\] ACPI +revision"
    --expect "\\[REPORT\\] PCI +[0-9]+ devices"
    --expect "\\[REPORT\\] SELFTEST PASS avx2 +4 CPUs"
    --expect "\\[REPORT\\] SELFTEST PASS sched +4 of 4 CPUs ran a pinned thread"
    --expect "\\[REPORT\\] BENCH context switch"
    --expect "ARC_TEST DONE pass=6 fail=0")

# QR screens (M1-04c): arc.qr=1 draws the report as ARC1 QR pages. The harness
# takes a QEMU screendump on each "ARC_QR shown" line and presses a key for the
# next page; tools/qr-check.py decodes every shot with zbarimg and compares it
# with the [REPORT] lines on serial, CRCs included. arc.qr=log does the same
# for the log tail. The panic code: arc.qrhold keeps the panic screen up
# (test mode only) until the harness has its screendump, then it ends QEMU.
set(_qr_check "${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/qr-check.py")
add_test(NAME qemu.qr.report COMMAND ${_qemu_test} --suite diag --name qr-report
    --cmdline arc.qr=1 --cmdline arc.selftest=1
    --screendump "^ARC_QR shown report "
    --check "${_qr_check} --kind report"
    --expect "\\[REPORT\\] arc_os boot report, build [0-9a-f]{12}"
    --expect "\\[REPORT\\] SELFTEST PASS"
    --expect "ARC_TEST DONE pass=[0-9]+ fail=0")
add_test(NAME qemu.qr.log COMMAND ${_qemu_test} --suite diag --name qr-log
    --cmdline arc.qr=log
    --screendump "^ARC_QR shown log "
    --check "${_qr_check} --kind log"
    --expect "ARC_TEST DONE pass=[0-9]+ fail=0")
add_test(NAME qemu.qr.panic COMMAND ${_qemu_test} --suite kpanic --name qr-panic
    --cmdline arc.panic=fault --cmdline arc.qrhold=1
    --expect-panic --timeout 90
    --screendump "^ARC_QR shown panic " --after-shot quit
    --check "${_qr_check} --kind panic"
    --expect "PANIC: Page Fault on CPU 0")

set_tests_properties(qemu.smoke qemu.mem.buddy qemu.mem.cap qemu.userland qemu.abi qemu.musl qemu.fpu qemu.cpufloor
    qemu.smp.xapic qemu.smp.x2apic qemu.sched qemu.lockorder qemu.selftest.fail qemu.selftest.panic
    qemu.panic.fault qemu.diag qemu.qr.report qemu.qr.log qemu.qr.panic
    PROPERTIES LABELS qemu TIMEOUT 180 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    FIXTURES_SETUP qemu_logs)

# Budgets (ADR-0019, M0-08): sizes and counts from the build and from the
# suites' `ARC_TEST budget` lines, against tests/budgets.txt. Runs after the
# suites above (fixture); no extra boot.
set(_budget_check ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/budget-check.py
    --build-dir ${ARC_CROSS_BUILD_DIR})
# Hygiene counts are checked by the host test `hygiene` (no QEMU needed)
add_test(NAME qemu.budgets COMMAND ${_budget_check} --skip-source hygiene)
# Self-check: deliberately wrong limits fail, each by name
add_test(NAME qemu.budgets.selftest COMMAND ${_budget_check}
    --budgets tests/budgets-selftest.txt
    --expect-fail kernel_image_bytes --expect-fail slab_steps_max --expect-fail boot_free_bytes)
set_tests_properties(qemu.budgets qemu.budgets.selftest
    PROPERTIES LABELS qemu TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    FIXTURES_REQUIRED qemu_logs)

# kernel.no_simd (M1-01, ADR-0003): no instruction in kernel.elf names an
# XMM/YMM/ZMM or mask register. Needs only the cross build, no boot.
add_test(NAME kernel.no_simd COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/no-simd-check.py
    ${ARC_CROSS_BUILD_DIR}/kernel.elf)
set_tests_properties(kernel.no_simd PROPERTIES LABELS qemu TIMEOUT 60)
