# arc-llm (M2-04): the model service, ADR-0012. C, linked with the C++ driver because
# libllama and libggml are C++ (static libstdc++).
add_executable(arc-llm main.c http.c roles.c engine.c api.c)
target_link_libraries(arc-llm PRIVATE arcjson port::llama)
set_target_properties(arc-llm PROPERTIES LINKER_LANGUAGE CXX)
