
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(lib-ydb_cli-import)
target_link_libraries(lib-ydb_cli-import PUBLIC
  contrib-libs-cxxsupp
  yutil
  api-protos
  common
  cpp-client-ydb_proto
)
target_sources(lib-ydb_cli-import PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/ydb_cli/import/import.cpp
)
