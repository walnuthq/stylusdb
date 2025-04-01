cmake_minimum_required(VERSION 3.16)

project(walnut-dbg)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include(AddLLVM)
include(TableGen)

link_directories(${LLVM_BUILD_ROOT}/)
find_library(LLDB_LIBRARY REQUIRED
    NAMES lldb LLDB
    HINTS "${LLVM_BUILD_ROOT}/lib"
    PATHS "${LLVM_BUILD_ROOT}/lib" /usr/lib/llvm-19/lib/
)

include_directories(/usr/lib/llvm-19/include)
include_directories(/opt/homebrew/opt/llvm@19/include)
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CMAKE_BINARY_DIR})
include_directories(${LLVM_SRC}/lldb/include/)
include_directories(${LLVM_BUILD_ROOT}/tools/lldb/include/)

llvm_map_components_to_libnames(llvm_libs
  Support
  Option
)

if (APPLE)
  set(CMAKE_MACOSX_RPATH ON)
endif()

add_subdirectory(lldb-plugins)

add_executable(walnut-dbg walnut-dbg.cpp Platform.cpp)

target_link_libraries(walnut-dbg ${llvm_libs} FunctionCallTrace)

set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(LLDBOptionsTableGen)

add_dependencies(walnut-dbg
  LLDBOptionsTableGen
  ${tablegen_deps}
)

if (APPLE)
set_target_properties(walnut-dbg PROPERTIES
    INSTALL_RPATH "@executable_path/../lib;@loader_path/../lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)
endif()

message("-- LLDB LIBRARY IS ${LLDB_LIBRARY}")

set_target_properties(walnut-dbg PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS walnut-dbg
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

if (APPLE)
    add_custom_command(
        TARGET walnut-dbg
        POST_BUILD

        # 1) Copy liblldb.19.1.7.dylib
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${LLDB_LIBRARY}"
                "/usr/local/lib/liblldb.19.1.7.dylib"

        # 2) Copy libLLVM.dylib
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${LLVM_LIB_PATH}"
                "/usr/local/lib/libLLVM.dylib"

        # 3) Copy libclang-cpp.dylib
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${LLVM_BUILD_ROOT}/lib/libclang-cpp.dylib"
                "/usr/local/lib/libclang-cpp.dylib"

        # 4) Copy LLDB_ARG_DUMPER_EXE
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${LLVM_BUILD_ROOT}/bin/lldb-argdumper"
                "/usr/local/bin/"

        COMMENT "Copying LLDB, LLVM, Clang libraries, and LLDB_ARG_DUMPER_EXE"
        VERBATIM
    )
endif()
