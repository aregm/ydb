
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-getopt-small)
target_link_libraries(cpp-getopt-small PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-colorizer
)
target_sources(cpp-getopt-small PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/completer.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/completer_command.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/completion_generator.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/formatted_output.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/last_getopt.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/last_getopt_easy_setup.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/last_getopt_opt.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/last_getopt_opts.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/last_getopt_parser.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/last_getopt_parse_result.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/modchooser.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/opt.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/opt2.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/posix_getopt.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/wrap.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/getopt/small/ygetopt.cpp
)
