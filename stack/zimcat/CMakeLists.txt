# zimcat (M3-03a): look inside a ZIM archive with arczim. Build host only (ADR-0011, ADR-0022).
add_executable(zimcat main.c)
target_link_libraries(zimcat PRIVATE arczim)
