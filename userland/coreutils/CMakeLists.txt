# arc_os — Coreutils (linked against libarc)

# Source name == binary name; the target gets a _bin suffix so it can't clash with other targets.
foreach(prog cat wc head tail touch cp mv ls grep ps uname rm)
    arc_user_program(${prog}_bin ${prog}.c OUTPUT ${prog})
endforeach()

# Sources with a _cmd suffix (their binary names are taken as CMake targets elsewhere).
foreach(prog echo mkdir stat chmod chown kill free)
    arc_user_program(${prog}_cmd ${prog}_cmd.c OUTPUT ${prog})
endforeach()
