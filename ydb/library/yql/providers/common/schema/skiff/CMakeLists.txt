
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(common-schema-skiff)
target_link_libraries(common-schema-skiff PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-yson-node
  yql-public-udf
  providers-common-codec
  common-schema-parser
)
target_sources(common-schema-skiff PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/common/schema/skiff/yql_skiff_schema.cpp
)
