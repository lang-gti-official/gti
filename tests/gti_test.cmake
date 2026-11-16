# gti-test: the directory-walking test runner. One `.gti` file under
# tests/ui is one test; adding a test touches no registration. See
# tests/ui/README.md and docs/plans/test-suite-redesign.md.
add_executable(gti-test
  "${PROJECT_SOURCE_DIR}/tests/runner/main.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/execute.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/test_file.cpp")
target_link_libraries(gti-test PRIVATE gti_driver)
target_include_directories(gti-test SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
# --isolate runs the sibling `gti`.
add_dependencies(gti-test gti)

# The runner's own behaviour, then `--tidy` and every test under tests/ui.
add_test(NAME gti_test_ui
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/runner/check.py"
    $<TARGET_FILE:gti-test> --jobs 4)
set_tests_properties(gti_test_ui PROPERTIES PROCESSORS 4)
