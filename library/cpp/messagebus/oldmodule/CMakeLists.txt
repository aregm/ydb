
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-messagebus-oldmodule)
target_link_libraries(cpp-messagebus-oldmodule PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-messagebus
  messagebus_actor
)
target_sources(cpp-messagebus-oldmodule PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/messagebus/oldmodule/module.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/messagebus/oldmodule/startsession.cpp
)
