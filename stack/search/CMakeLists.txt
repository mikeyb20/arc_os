# search (M3-06a): keyword search over the installed packs, no arc-llm needed (ADR-0015).
add_executable(search search.c)
target_link_libraries(search PRIVATE arcsearch)
