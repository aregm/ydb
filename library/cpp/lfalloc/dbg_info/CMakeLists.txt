
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-lfalloc-dbg_info)
target_link_libraries(cpp-lfalloc-dbg_info PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-malloc-api
)
target_sources(cpp-lfalloc-dbg_info PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/lfalloc/dbg_info/dbg_info.cpp
)
