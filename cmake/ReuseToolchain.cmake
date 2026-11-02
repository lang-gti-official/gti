# Test consumers of an admitted production compiler without rebuilding it.
# Admission and archive verification belong to scripts/source_stdlib_ci.py.
get_filename_component(GTI_REUSE_TOOLCHAIN "${GTI_REUSE_TOOLCHAIN}" ABSOLUTE)
find_package(GTI CONFIG REQUIRED PATHS "${GTI_REUSE_TOOLCHAIN}/lib/cmake/GTI" NO_DEFAULT_PATH)
foreach(component compiler cpp_backend llvm_backend driver bindgen runtime)
  add_library(gti_${component} ALIAS GTI::${component})
endforeach()
foreach(tool gti gti_lsp)
  add_executable(${tool} IMPORTED GLOBAL)
  set_target_properties(${tool} PROPERTIES IMPORTED_LOCATION "${GTI_REUSE_TOOLCHAIN}/bin/${tool}")
endforeach()
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
