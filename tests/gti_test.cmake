# gti-test: the directory-walking test runner. One `.gti` file under
# tests/ui is one test; adding a test touches no registration. See
# tests/ui/README.md and docs/plans/test-suite-redesign.md.
add_executable(gti-test
  "${PROJECT_SOURCE_DIR}/tests/runner/main.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/execute.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/snapshot.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/lsp.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/test_file.cpp")
target_link_libraries(gti-test PRIVATE gti_driver)
target_include_directories(gti-test SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
# --isolate runs the sibling `gti`.
add_dependencies(gti-test gti)
# LSP tests drive the sibling `gti_lsp`.
if(TARGET gti_lsp)
  add_dependencies(gti-test gti_lsp)
endif()

# The runner's own behaviour, then `--tidy` and every test under tests/ui.
add_test(NAME gti_test_ui
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/runner/check.py"
    $<TARGET_FILE:gti-test> --jobs 4)
# Per-test limits are CPU seconds measured on a fast development machine;
# hosted runners are about twice as slow, and sanitizer or other slow trees
# scale every bound by GTI_TEST_TIMEOUT_SCALE.
# (TestThroughput.cmake defines the cache entry after this file.)
set(gti_test_timeout_scale "${GTI_TEST_TIMEOUT_SCALE}")
if(NOT gti_test_timeout_scale MATCHES "^[1-9][0-9]*$")
  set(gti_test_timeout_scale 1)
endif()
math(EXPR gti_test_time_scale "2 * ${gti_test_timeout_scale}")
set_tests_properties(gti_test_ui PROPERTIES PROCESSORS 4
  ENVIRONMENT "GTI_TEST_TIME_SCALE=${gti_test_time_scale}")
