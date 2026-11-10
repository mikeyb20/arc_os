# Lane tests for arc-llm (M2-04): the service in a temporary root with the archived tiny models,
# checked against the M2-03 llama.cpp tools (arc_llm_lane.py says what each test does).
foreach(test chat embed roles fit errors clients)
    lane_driver_test(NAME arc_llm_${test} COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/arc_llm_lane.py" ${test}
        --arc-llm $<TARGET_FILE:arc-llm> --tools "${ARC_LLAMA_OURS}" --chat "${ARC_TINY_CHAT}"
        --embed "${ARC_TINY_EMBED}")
endforeach()
