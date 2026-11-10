# Lane tests for the ports (M2-02): zstd and SQLite as arc_os programs use them.

add_executable(zstd_lib zstd_lib.c)
target_link_libraries(zstd_lib PRIVATE port::zstd)
lane_test(NAME zstd_lib COMMAND $<TARGET_FILE:zstd_lib>)

add_executable(sqlite_fts5 sqlite_fts5.c)
target_link_libraries(sqlite_fts5 PRIVATE port::sqlite3)
lane_test(NAME sqlite_fts5 COMMAND $<TARGET_FILE:sqlite_fts5>)

lane_driver_test(NAME zstd_cli COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/zstd_cli.sh" "${ARC_ZSTD_CLI}")
lane_driver_test(NAME sqlite_shell COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/sqlite_shell.sh"
    "${ARC_SQLITE_SHELL}" "${ARC_ROOT}/tests/target/fts.sql")
