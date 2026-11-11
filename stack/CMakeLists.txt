# arc_os user-space stack built with musl (lane L; conventions in docs/plans/lane-l.md).
#
#   tools/lane/build.sh                         configure and build into build-lane/stack
#   ctest --test-dir build-lane/stack -L lane   run the lane tests on Linux
#
# Static x86-64-v3 programs and libraries (toolchain-musl.cmake). Ports are found under
# build-lane/ports/<name>/ (ports/<name>/build.sh). userland/ is the libarc base system and
# is built by the top-level project, not here.
cmake_minimum_required(VERSION 3.20)
project(arc_stack C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)  # our code only: ports are built by their own build.sh

get_filename_component(ARC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(ARC_LANE_RUN "${ARC_ROOT}/tools/lane/run.sh")
# Lane tests write `ARC_TEST budget` lines here; lane.yml checks them against
# stack/budgets.txt with tools/budget-check.py --log-dir.
set(ARC_LANE_BUDGET_DIR "${CMAKE_BINARY_DIR}/budgets")
file(MAKE_DIRECTORY "${ARC_LANE_BUDGET_DIR}")

enable_testing()

# lane_test(NAME <name> COMMAND <program> [args...] [WORKING_DIRECTORY <dir>] [ENVIRONMENT <VAR=value>...])
# Registers ctest test lane.<name> (label `lane`). The program runs through
# tools/lane/run.sh, so it is traced when ARC_LANE_LOGS is set (log <name>.strace),
# and so is everything it execs: those must be arc_os-bound programs too.
# Use it only for arc_os-bound programs; drivers that start them use lane_driver_test().
function(lane_test)
    cmake_parse_arguments(T "" "NAME;WORKING_DIRECTORY" "COMMAND;ENVIRONMENT" ${ARGN})
    if(NOT T_WORKING_DIRECTORY)
        set(T_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    add_test(NAME lane.${T_NAME} COMMAND "${ARC_LANE_RUN}" ${T_NAME} ${T_COMMAND}
             WORKING_DIRECTORY "${T_WORKING_DIRECTORY}")
    set_tests_properties(lane.${T_NAME} PROPERTIES LABELS lane)
    if(T_ENVIRONMENT)
        set_tests_properties(lane.${T_NAME} PROPERTIES ENVIRONMENT "${T_ENVIRONMENT}")
    endif()
endfunction()

# lane_driver_test(NAME <name> COMMAND <driver> [args...])
# A test whose driver (shell or Python, untraced) starts arc_os-bound programs itself,
# each through tools/lane/run.sh. ARC_LANE_RUN and ARC_LANE_BUDGET_DIR are in its environment.
function(lane_driver_test)
    cmake_parse_arguments(T "" "NAME" "COMMAND" ${ARGN})
    add_test(NAME lane.${T_NAME} COMMAND ${T_COMMAND} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(lane.${T_NAME} PROPERTIES LABELS lane
        ENVIRONMENT "ARC_LANE_RUN=${ARC_LANE_RUN};ARC_LANE_BUDGET_DIR=${ARC_LANE_BUDGET_DIR}")
endfunction()

# Ports (ports/<name>/build.sh, installed by tools/lane/build.sh into build-lane/ports/<name>/,
# the toolchain's CMAKE_FIND_ROOT_PATH). Every program in stack/ links them through these
# imported targets, defined only here.
if(NOT CMAKE_FIND_ROOT_PATH)
    message(FATAL_ERROR "configure stack/ with -DCMAKE_TOOLCHAIN_FILE=toolchain-musl.cmake (tools/lane/build.sh)")
endif()
list(GET CMAKE_FIND_ROOT_PATH 0 ARC_PORTS_DIR)
# Test-only reference builds of ports (ports/<name>/build-ref.sh) sit beside them, in build-lane/ref/<name>/.
get_filename_component(ARC_PORTS_REF_DIR "${ARC_PORTS_DIR}/../ref" ABSOLUTE)

# arc_port_library(<target> <port> <library> <header>)
# Defines imported static library port::<target> from build-lane/ports/<port>/{lib,include}.
function(arc_port_library target port library header)
    find_library(ARC_PORT_${target}_LIBRARY NAMES ${library} PATHS /${port}/lib NO_DEFAULT_PATH)
    find_path(ARC_PORT_${target}_INCLUDE NAMES ${header} PATHS /${port}/include NO_DEFAULT_PATH)
    if(NOT ARC_PORT_${target}_LIBRARY OR NOT ARC_PORT_${target}_INCLUDE)
        message(FATAL_ERROR "port ${port} not built in ${ARC_PORTS_DIR}/${port} (run tools/lane/build.sh)")
    endif()
    add_library(port::${target} STATIC IMPORTED GLOBAL)
    set_target_properties(port::${target} PROPERTIES
        IMPORTED_LOCATION "${ARC_PORT_${target}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ARC_PORT_${target}_INCLUDE}")
endfunction()

# arc_port_program(<variable> <port> <program>): path of a port's program, in <variable>.
function(arc_port_program variable port program)
    find_program(${variable} NAMES ${program} PATHS "${ARC_PORTS_DIR}/${port}/bin" NO_DEFAULT_PATH)
    if(NOT ${variable})
        message(FATAL_ERROR "port ${port} has no ${program} in ${ARC_PORTS_DIR}/${port}/bin (run tools/lane/build.sh)")
    endif()
endfunction()

arc_port_library(zstd zstd zstd zstd.h)
arc_port_library(sqlite3 sqlite sqlite3 sqlite3.h)
# llama.cpp (M2-03): libllama needs libggml (backend registry), which needs the CPU backend and
# ggml-base; static archives, so the link order is fixed here once.
arc_port_library(llama llama.cpp llama llama.h)
arc_port_library(ggml llama.cpp ggml ggml.h)
arc_port_library(ggml_cpu llama.cpp ggml-cpu ggml-cpu.h)
arc_port_library(ggml_base llama.cpp ggml-base ggml.h)
set_target_properties(port::llama PROPERTIES INTERFACE_LINK_LIBRARIES "port::ggml;pthread")
set_target_properties(port::ggml PROPERTIES INTERFACE_LINK_LIBRARIES "port::ggml_cpu;port::ggml_base")
set_target_properties(port::ggml_cpu PROPERTIES INTERFACE_LINK_LIBRARIES "port::ggml_base;pthread")
arc_port_program(ARC_ZSTD_CLI zstd zstd)
arc_port_program(ARC_SQLITE_SHELL sqlite sqlite3)
arc_port_program(ARC_LLAMA_COMPLETION llama.cpp llama-completion)
arc_port_program(ARC_LLAMA_EMBEDDING llama.cpp llama-embedding)
arc_port_program(ARC_LLAMA_TOKENIZE llama.cpp llama-tokenize)

add_subdirectory(lib)
add_subdirectory(arc-llm)
add_subdirectory(search)
add_subdirectory(arcpkg)
add_subdirectory(tests)
