# arc_os — ARC_MUSL_PREFIX: path prefix of the pinned x86_64-linux-musl tools (M2-01).
#
# Cache variable, else environment variable, else where `tools/dev-setup.sh musl` installs
# them. Included by toolchain-musl.cmake (stack/) and userland/testprogs (musl test programs).
if(NOT DEFINED ARC_MUSL_PREFIX)
    if(DEFINED ENV{ARC_MUSL_PREFIX})
        set(ARC_MUSL_PREFIX "$ENV{ARC_MUSL_PREFIX}")
    else()
        set(ARC_MUSL_PREFIX "$ENV{HOME}/opt/cross-musl/bin/x86_64-linux-musl-")
    endif()
endif()
set(ARC_MUSL_PREFIX "${ARC_MUSL_PREFIX}" CACHE STRING "Path prefix of the x86_64-linux-musl tools")
