# Lane tests for arcpkg (M5-02): install, refusals, model versions and roles, the 200 MiB install.
# arcpkg_lane.py says what each test checks; it builds the fixture packages itself (tarfile + zstd CLI).
foreach(test install refuse models big)
    lane_driver_test(NAME arcpkg_${test} COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/arcpkg_lane.py" ${test}
        --arcpkg $<TARGET_FILE:arcpkg> --zstd ${ARC_ZSTD_CLI} --lane-rss $<TARGET_FILE:lane_rss>
        --validate "${ARC_ROOT}/tests/schemas/validate.py")
endforeach()
