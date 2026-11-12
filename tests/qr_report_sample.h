/* arc_os — A real boot report for tests/test_qr.c: the [REPORT] lines (prefix
 * removed) of the qemu.diag boot (arc.report=1 arc.selftest=1 arc.bench=1,
 * QEMU 8.2 TCG, q35, 4 CPUs, 4 GiB) on 2026-11-12. Test data only. */

#ifndef ARCHOS_TESTS_QR_REPORT_SAMPLE_H
#define ARCHOS_TESTS_QR_REPORT_SAMPLE_H

static const char qr_report_sample[] =
    "arc_os boot report (M1-04 bring-up)\n"
    "Machine    QEMU Standard PC (Q35 + ICH9, 2009) (pc-q35-8.2)\n"
    "Firmware   legacy BIOS; BIOS SeaBIOS 1.16.3-debian-1.16.3-2 04/01/2014; SMBIOS 3.0\n"
    "CPU        QEMU TCG CPU version 2.5+\n"
    "CPU id     AuthenticAMD family 0x6 model 0x6 stepping 3\n"
    "Features   AVX2 FMA BMI2 XSAVE MWAIT SMEP SMAP NX 1G-PAGES RDRAND\n"
    "CPUs       4 online; MADT lists 4, bootloader 4; cores: 0 P, 0 E, 4 untyped\n"
    "APIC       xAPIC mode\n"
    "TSC        2099116 kHz from pit (CPUID 0x15 0 kHz, PIT 2099116 kHz, base 0 MHz), NOT invariant\n"
    "Memory     4095 MB RAM (top of RAM 0x180000000), ACPI 0 KB, reserved 12544 MB, other 3 MB, 47 map entries\n"
    "Memory     4037 MB free now\n"
    "Display    1280x800, 32 bpp, pitch 5120; R 8@16 G 8@8 B 8@0; text 160x50\n"
    "Cmdline    arc.test=diag arc.report=1 arc.selftest=1 arc.bench=1\n"
    "ACPI       revision 0, OEM 'BOCHS ', 5 tables\n"
    "           FACP:244 APIC:144 HPET:56 MCFG:60 WAET:40\n"
    "PCI        6 devices\n"
    "  00:00.0 8086:29c0 class 0600 host bridge  no driver\n"
    "  00:01.0 1234:1111 class 0300 display      no driver\n"
    "  00:02.0 8086:10d3 class 0200 ethernet     no driver\n"
    "  00:1f.0 8086:2918 class 0601 ISA bridge   no driver\n"
    "  00:1f.2 8086:2922 class 0106 SATA         no driver\n"
    "  00:1f.3 8086:2930 class 0c05 SMBus        no driver\n"
    "Self-test\n"
    "SELFTEST PASS cpus     4 of 4 MADT CPUs online\n"
    "SELFTEST PASS avx2     4 CPUs (4 answered the IPI), sum f35be000, XCR0 7\n"
    "SELFTEST PASS memory   256 MB, 2 patterns, no errors\n"
    "SELFTEST PASS tsc      2099116 kHz from the PIT (the CPU gives no frequency to compare)\n"
    "SELFTEST PASS timer    20 PIT interrupts in 200 ms by the TSC (want 20)\n"
    "Self-test: 5 PASS, 0 FAIL\n"
    "Benchmark (TSC 2099116 kHz; per operation, best of 3 runs)\n"
    "BENCH page alloc+free 4 KiB        1998 cycles      951 ns  (10000 runs)\n"
    "BENCH page alloc+free 2 MiB        2035 cycles      969 ns  (1000 runs)\n"
    "BENCH kmalloc+kfree 64 B           6745 cycles     3213 ns  (10000 runs)\n"
    "BENCH context switch               3461 cycles     1649 ns  (10000 runs)\n"
    "End of report.\n";

#endif /* ARCHOS_TESTS_QR_REPORT_SAMPLE_H */
