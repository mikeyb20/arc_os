# packbuild (M3-03): article stream -> pack content/ and chunks/ (content-lane.md). The library holds the
# chunker and the build, so the lane tests call them directly; the program is the command line.
add_library(packbuild_core STATIC build.c chunk.c)
target_include_directories(packbuild_core PUBLIC .)
target_link_libraries(packbuild_core PUBLIC arcpack PRIVATE arcjson arcutil)
add_executable(packbuild main.c)
target_link_libraries(packbuild PRIVATE packbuild_core)
