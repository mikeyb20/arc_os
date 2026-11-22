# lane.packbuild (M3-03): packbuild_test.c checks the build and the chunker in-process on generated
# streams; packbuild_lane.py runs the program (usage, exit codes, nothing left behind, peak RSS on a
# 10 000-document stream through lane_rss).
add_executable(packbuild_test packbuild_test.c)
target_link_libraries(packbuild_test PRIVATE packbuild_core arcjson arcutil)
lane_test(NAME packbuild COMMAND $<TARGET_FILE:packbuild_test> "${CMAKE_CURRENT_BINARY_DIR}/scratch")
lane_driver_test(NAME packbuild_cli COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/packbuild_lane.py"
    --packbuild $<TARGET_FILE:packbuild> --lane-rss $<TARGET_FILE:lane_rss>)
