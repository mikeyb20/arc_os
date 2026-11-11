# lane.search (M3-06a): keyword search over two small packs; search_lane.py says what it checks.
add_executable(search_test search_test.c)
target_link_libraries(search_test PRIVATE arcsearch)
lane_driver_test(NAME search COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/search_lane.py"
    --program $<TARGET_FILE:search_test> --search $<TARGET_FILE:search> --lane-rss $<TARGET_FILE:lane_rss>)
