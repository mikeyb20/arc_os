# lane.arcjson (M2-04): the JSON library against RFC 8259 edge cases.
add_executable(arcjson_test arcjson_test.c)
target_link_libraries(arcjson_test PRIVATE arcjson)
lane_test(NAME arcjson COMMAND $<TARGET_FILE:arcjson_test>)
