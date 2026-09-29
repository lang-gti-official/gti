include_guard(GLOBAL)

# GTI's runtime and corpus tests invoke the GTI compiler many times. Building
# that compiler without optimization can turn a focused test into a minutes-
# long job, so a test- or fuzz-enabled first-party build must use an optimized
# host compiler configuration.
function(gti_require_optimized_test_compiler)
  if(NOT BUILD_TESTING AND NOT GTI_BUILD_FUZZERS)
    return()
  endif()

  set(gti_optimized_test_configurations
      Release RelWithDebInfo MinSizeRel)

  if(CMAKE_CONFIGURATION_TYPES)
    set(
      CMAKE_CONFIGURATION_TYPES
      "${gti_optimized_test_configurations}"
      CACHE STRING "Optimized configurations available to GTI test/fuzz builds"
      FORCE
    )
    set(CMAKE_CONFIGURATION_TYPES
        "${gti_optimized_test_configurations}"
        PARENT_SCOPE)
    return()
  endif()

  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type selected for a GTI test build" FORCE)
    message(STATUS
            "GTI testing or fuzzing is enabled; defaulting CMAKE_BUILD_TYPE to Release")
    return()
  endif()

  if(NOT CMAKE_BUILD_TYPE IN_LIST gti_optimized_test_configurations)
    string(JOIN ", " gti_allowed_test_configurations
           ${gti_optimized_test_configurations})
    message(
      FATAL_ERROR
        "GTI tests and fuzzers require an optimized compiler configuration "
        "(${gti_allowed_test_configurations}); got '${CMAKE_BUILD_TYPE}'. "
        "Use -DCMAKE_BUILD_TYPE=Release (or RelWithDebInfo for debuggable "
        "optimized tests). Configure a separate Debug tree with "
        "-DBUILD_TESTING=OFF -DGTI_BUILD_FUZZERS=OFF when an unoptimized "
        "compiler is specifically needed for debugger work."
    )
  endif()
endfunction()
