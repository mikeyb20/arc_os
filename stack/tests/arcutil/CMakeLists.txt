# lane.arcutil (M5-02): SHA-256 FIPS vectors, durable write, directory creation and removal.
add_executable(arcutil_test arcutil_test.c)
target_link_libraries(arcutil_test PRIVATE arcutil)
lane_test(NAME arcutil COMMAND $<TARGET_FILE:arcutil_test>)
