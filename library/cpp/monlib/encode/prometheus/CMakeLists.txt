
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(monlib-encode-prometheus)
target_link_libraries(monlib-encode-prometheus PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-monlib-encode
)
target_sources(monlib-encode-prometheus PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/monlib/encode/prometheus/prometheus_decoder.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/monlib/encode/prometheus/prometheus_encoder.cpp
)
