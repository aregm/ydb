set_property(SOURCE ${CMAKE_BINARY_DIR}/contrib/libs/llvm12/lib/Frontend/OpenMP/OMP.cpp
  PROPERTY GENERATED On
)


# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(lib-Frontend-OpenMP)
target_include_directories(lib-Frontend-OpenMP PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Frontend/OpenMP
)
target_link_libraries(lib-Frontend-OpenMP PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  libs-llvm12-include
  llvm12-lib-IR
  llvm12-lib-Support
  lib-Transforms-Utils
)
target_sources(lib-Frontend-OpenMP PRIVATE
  ${CMAKE_BINARY_DIR}/contrib/libs/llvm12/lib/Frontend/OpenMP/OMP.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Frontend/OpenMP/OMPContext.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/Frontend/OpenMP/OMPIRBuilder.cpp
)

add_dependencies(lib-Frontend-OpenMP OpenMP-gen-srcs-stealing)

