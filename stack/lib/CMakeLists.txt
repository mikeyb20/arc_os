# Stack libraries (stack/README.md lists them; one home per helper, ADR-0020).

# arcjson (M2-04): the stack's only JSON parser and writer.
add_library(arcjson STATIC arcjson/arcjson.c)
target_include_directories(arcjson PUBLIC arcjson)
target_link_libraries(arcjson PUBLIC m)

# arcutil (M3-06a, M5-02): SHA-256, durable files, recursive removal, RAM size. No other copies in stack/.
add_library(arcutil STATIC arcutil/arcutil.c arcutil/sha256.c)
target_include_directories(arcutil PUBLIC arcutil)

# arcpack (M3-02): the pack reader and writer, docs/formats/pack-v1.md. The only code that runs
# zstd on pack content.
add_library(arcpack STATIC arcpack/arcpack.c arcpack/arcpack_writer.c)
target_include_directories(arcpack PUBLIC arcpack)
target_link_libraries(arcpack PUBLIC arcjson PRIVATE arcutil port::zstd)

# arcsearch (M3-06a): keyword search over the packs' FTS5 indexes, and the index builder.
add_library(arcsearch STATIC arcsearch/arcsearch.c arcsearch/arcsearch_build.c)
target_include_directories(arcsearch PUBLIC arcsearch)
target_link_libraries(arcsearch PUBLIC arcpack PRIVATE arcutil arcjson port::sqlite3)
