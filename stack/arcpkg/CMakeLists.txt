# arcpkg (M5-02): install, remove, list and verify .arcpkg packages; switch model roles (ADR-0010, ADR-0012).
add_executable(arcpkg main.c install.c tar.c manifest.c db.c)
target_link_libraries(arcpkg PRIVATE arcjson arcutil port::zstd)
