
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(lib-TextAPI-MachO)
target_include_directories(lib-TextAPI-MachO PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI
)
target_link_libraries(lib-TextAPI-MachO PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  llvm12-lib-BinaryFormat
  llvm12-lib-Support
)
target_sources(lib-TextAPI-MachO PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/Architecture.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/ArchitectureSet.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/InterfaceFile.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/PackedVersion.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/Platform.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/Symbol.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/Target.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/TextStub.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/TextAPI/MachO/TextStubCommon.cpp
)
