# Test consumers of an admitted production compiler without rebuilding it.
# Admission and archive verification belong to scripts/source_stdlib_ci.py.
get_filename_component(GTI_REUSE_TOOLCHAIN "${GTI_REUSE_TOOLCHAIN}" ABSOLUTE)
find_package(GTI CONFIG REQUIRED PATHS "${GTI_REUSE_TOOLCHAIN}/lib/cmake/GTI" NO_DEFAULT_PATH)
foreach(component compiler cpp_backend llvm_backend driver bindgen runtime)
  add_library(gti_${component} ALIAS GTI::${component})
endforeach()
set(reused_tools gti)
if(NOT GTI_REBUILD_LSP)
  list(APPEND reused_tools gti_lsp)
endif()
foreach(tool IN LISTS reused_tools)
  add_executable(${tool} IMPORTED GLOBAL)
  set_target_properties(${tool} PROPERTIES IMPORTED_LOCATION "${GTI_REUSE_TOOLCHAIN}/bin/${tool}")
endforeach()
if(GTI_REBUILD_LSP)
  if(NOT TARGET GTI::project_services)
    message(FATAL_ERROR "The admitted SDK predates independent LSP builds")
  endif()
  file(READ "${GTI_REUSE_TOOLCHAIN}/share/gti/LSP_COMPATIBILITY.json" lsp_metadata)
  string(JSON sdk_compatibility GET "${lsp_metadata}" compatibility)
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/scripts/lsp_compatibility.py" --source "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE identity_result OUTPUT_VARIABLE source_compatibility
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT identity_result EQUAL 0 OR NOT sdk_compatibility STREQUAL source_compatibility)
    message(FATAL_ERROR "LSP source is incompatible with the admitted SDK")
  endif()
  add_library(gti_lsp_state STATIC "${PROJECT_SOURCE_DIR}/src/lsp/frontend_snapshot_state.cpp")
  target_include_directories(gti_lsp_state PUBLIC "${PROJECT_SOURCE_DIR}/src/lsp")
  target_link_libraries(gti_lsp_state PUBLIC GTI::compiler)
  target_compile_features(gti_lsp_state PUBLIC cxx_std_20)
  add_executable(gti_lsp "${PROJECT_SOURCE_DIR}/src/lsp/main.cpp")
  target_include_directories(gti_lsp SYSTEM PRIVATE "${GTI_REUSE_TOOLCHAIN}/include/gti-llvm")
  target_link_libraries(gti_lsp PRIVATE GTI::project_services gti_lsp_state Threads::Threads)
  target_compile_definitions(gti_lsp PRIVATE GTI_VERSION="${PROJECT_VERSION}"
    GTI_BUILD_STDLIB_ROOT="${GTI_REUSE_TOOLCHAIN}/share/gti/stdlib")
endif()
add_library(gti_tree_sitter MODULE IMPORTED GLOBAL)
set_target_properties(gti_tree_sitter PROPERTIES IMPORTED_LOCATION "${GTI_REUSE_TOOLCHAIN}/share/gti/parser/gti.so")
set(GTI_BUILD_STDLIB_ROOT "${PROJECT_SOURCE_DIR}/stdlib")
set(GTI_RELEASE_BUILD ON)
# Do not retain every compiler test case's allocations in a single process.
# The existing aggregate validates the complete census in either mode.
set(GTI_COMPILER_TEST_ISOLATE_CASES ON CACHE BOOL "Bound source-lane compiler test memory" FORCE)
# Existing test registrations remain authoritative. Only selected test harness
# targets are built; no source compiler/runtime targets exist in this graph.
add_subdirectory(tests)
