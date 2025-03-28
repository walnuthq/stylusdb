add_library(FunctionCallTrace STATIC
    FunctionCallTrace.cpp
)

target_link_libraries(FunctionCallTrace PRIVATE ${llvm_libs} ${LLDB_LIBRARY})

set_target_properties(FunctionCallTrace PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS FunctionCallTrace
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

add_library(FunctionCallTracePlugin SHARED Plugin.cpp)
target_link_libraries(FunctionCallTracePlugin
    PRIVATE
        FunctionCallTrace
)

set_target_properties(FunctionCallTracePlugin PROPERTIES
    INSTALL_RPATH "@loader_path"
    MACOSX_RPATH ON
    LINK_FLAGS "-Wl,-export_dynamic"
)

set_target_properties(FunctionCallTracePlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS FunctionCallTracePlugin
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
