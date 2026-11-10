# Stack libraries (stack/README.md lists them; one home per helper, ADR-0020).

# arcjson (M2-04): the stack's only JSON parser and writer.
add_library(arcjson STATIC arcjson/arcjson.c)
target_include_directories(arcjson PUBLIC arcjson)
target_link_libraries(arcjson PUBLIC m)
