# Lane tests for the ZIM reader (M3-03a, ADR-0022). zim_fixture writes test ZIMs (lane tests never
# download one); arczim_test checks the library on good and damaged files; zimcat_lane.py runs zimcat.
add_library(zim_fixture STATIC zim_fixture.c)
target_include_directories(zim_fixture PUBLIC . ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/arczim)
target_link_libraries(zim_fixture PRIVATE port::zstd)
add_executable(zim_fixture_tool zim_fixture_main.c)
set_target_properties(zim_fixture_tool PROPERTIES OUTPUT_NAME zim_fixture)
target_link_libraries(zim_fixture_tool PRIVATE zim_fixture arcjson)

add_executable(arczim_test arczim_test.c)
target_link_libraries(arczim_test PRIVATE arczim zim_fixture)
lane_test(NAME arczim COMMAND $<TARGET_FILE:arczim_test>)
lane_driver_test(NAME zimcat COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/zimcat_lane.py"
    --zimcat $<TARGET_FILE:zimcat> --fixture $<TARGET_FILE:zim_fixture_tool> --lane-rss $<TARGET_FILE:lane_rss>)
# M3-03: a fixture ZIM through zimcat stream, tools/content/zim_extract.py and packbuild.
lane_driver_test(NAME zim_extract COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/zim_extract_lane.py"
    --zimcat $<TARGET_FILE:zimcat> --fixture $<TARGET_FILE:zim_fixture_tool> --packbuild $<TARGET_FILE:packbuild>
    --extract "${ARC_ROOT}/tools/content/zim_extract.py")
