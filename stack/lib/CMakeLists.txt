# Stack libraries (stack/README.md lists them; one home per helper, ADR-0020).

# arcjson (M2-04): the stack's only JSON parser and writer.
add_library(arcjson STATIC arcjson/arcjson.c)
target_include_directories(arcjson PUBLIC arcjson)
target_link_libraries(arcjson PUBLIC m)

# arcpack (M3-02): the pack reader and writer, docs/formats/pack-v1.md. The only code that runs
# zstd on pack content.
add_library(arcpack STATIC arcpack/arcpack.c arcpack/arcpack_writer.c)
target_include_directories(arcpack PUBLIC arcpack)
target_link_libraries(arcpack PUBLIC arcjson PRIVATE port::zstd)
