
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-monlib-dynamic_counters)
target_link_libraries(cpp-monlib-dynamic_counters PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-containers-stack_vector
  monlib-encode-json
  monlib-encode-spack
  monlib-encode-prometheus
  monlib-service-pages
  cpp-string_utils-quote
  cpp-threading-light_rw_lock
)
target_sources(cpp-monlib-dynamic_counters PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/monlib/dynamic_counters/counters.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/monlib/dynamic_counters/encode.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/monlib/dynamic_counters/golovan_page.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/monlib/dynamic_counters/page.cpp
)
