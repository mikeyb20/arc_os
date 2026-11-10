# Lane tests for the llama.cpp port (M2-03): our static musl tools against the test-only
# reference build (ports/llama.cpp/build-ref.sh), on the archived tiny models.

set(ARC_LLAMA_REF "${ARC_PORTS_REF_DIR}/llama.cpp/bin")
if(NOT EXISTS "${ARC_LLAMA_REF}/llama-completion")
    message(FATAL_ERROR "no llama.cpp reference build in ${ARC_LLAMA_REF} (run tools/lane/build.sh)")
endif()

foreach(test greedy nommap embed tokenize)
    lane_driver_test(NAME llama_${test} COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/llama_lane.py" ${test}
        --ours "${ARC_LLAMA_OURS}" --ref "${ARC_LLAMA_REF}" --chat "${ARC_TINY_CHAT}" --embed "${ARC_TINY_EMBED}")
endforeach()
