# One timeout scale for every registered test. Timeouts are sized for a
# developer host and separate a hang from a slow compile; a slower host states
# its factor here instead of loosening those values. The same factor reaches
# the Python harnesses as GTI_TEST_TIMEOUT_SCALE in every test's environment
# (tests/harness_timeouts.py). This is scheduling metadata only: no case,
# assertion or observation changes.
set(GTI_TEST_TIMEOUT_SCALE "1" CACHE STRING
  "Multiply every CTest and harness timeout for a host slower than the developer machine")
if(NOT GTI_TEST_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "GTI_TEST_TIMEOUT_SCALE must be a positive integer")
endif()

# Apply the scale to every test registered in the calling directory; call it
# after the directory's last add_test. At scale 1 a test without its own
# TIMEOUT keeps CTest's stored default (DART_TESTING_TIMEOUT, else 1500
# seconds), so a command-line --timeout still governs it. A larger scale fixes
# that default explicitly, because CTest cannot scale a default it never
# stored per test.
function(gti_scale_test_timeouts)
  get_property(gti_scaled_tests DIRECTORY PROPERTY TESTS)
  set(gti_default_timeout 1500)
  if(DEFINED DART_TESTING_TIMEOUT AND DART_TESTING_TIMEOUT MATCHES "^[1-9][0-9]*$")
    set(gti_default_timeout "${DART_TESTING_TIMEOUT}")
  endif()
  foreach(test IN LISTS gti_scaled_tests)
    get_test_property("${test}" TIMEOUT gti_timeout)
    if(gti_timeout MATCHES "^[1-9][0-9]*$")
      math(EXPR gti_timeout "${gti_timeout} * ${GTI_TEST_TIMEOUT_SCALE}")
      set_tests_properties("${test}" PROPERTIES TIMEOUT "${gti_timeout}")
    elseif(GTI_TEST_TIMEOUT_SCALE GREATER 1)
      math(EXPR gti_timeout "${gti_default_timeout} * ${GTI_TEST_TIMEOUT_SCALE}")
      set_tests_properties("${test}" PROPERTIES TIMEOUT "${gti_timeout}")
    endif()
    set_property(TEST "${test}" APPEND PROPERTY ENVIRONMENT
      "GTI_TEST_TIMEOUT_SCALE=${GTI_TEST_TIMEOUT_SCALE}")
  endforeach()
  if(GTI_TEST_TIMEOUT_SCALE GREATER 1)
    list(LENGTH gti_scaled_tests gti_scaled_count)
    message(STATUS "Test timeouts scaled by ${GTI_TEST_TIMEOUT_SCALE} for "
      "${gti_scaled_count} tests in ${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endfunction()
