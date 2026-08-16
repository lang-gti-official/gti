cmake_minimum_required(VERSION 3.20)

foreach(required_variable
        IN ITEMS GTI_SOURCE_DIR GTI_STAGE_DIR GTI_OUTPUT_DIR GTI_PLATFORM)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

if(NOT DEFINED GTI_CHANNEL OR "${GTI_CHANNEL}" STREQUAL "")
  set(GTI_CHANNEL "stable")
endif()
if(NOT GTI_CHANNEL STREQUAL "stable" AND NOT GTI_CHANNEL STREQUAL "nightly")
  message(FATAL_ERROR "GTI_CHANNEL must be stable or nightly")
endif()

file(STRINGS "${GTI_SOURCE_DIR}/VERSION" GTI_VERSION LIMIT_COUNT 1)
if(NOT GTI_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION does not contain a valid semantic version")
endif()

# A nightly archive is named for its channel rather than its version: the
# channel republishes one release per commit, so its tag never matches VERSION.
# Consumers tell nightly builds apart by BUILD_ID, not by VERSION.
if(GTI_CHANNEL STREQUAL "nightly")
  if(NOT DEFINED GTI_BUILD_ID OR "${GTI_BUILD_ID}" STREQUAL "")
    message(FATAL_ERROR "GTI_BUILD_ID is required for the nightly channel")
  endif()
  if(NOT GTI_BUILD_ID MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "GTI_BUILD_ID must be a commit identity")
  endif()
  if(DEFINED GTI_EXPECTED_TAG AND NOT GTI_EXPECTED_TAG STREQUAL "nightly")
    message(FATAL_ERROR
            "Nightly tag ${GTI_EXPECTED_TAG} must be nightly")
  endif()
elseif(DEFINED GTI_EXPECTED_TAG AND
       NOT GTI_EXPECTED_TAG STREQUAL "v${GTI_VERSION}")
  message(FATAL_ERROR
          "Release tag ${GTI_EXPECTED_TAG} does not match VERSION ${GTI_VERSION}")
endif()

foreach(required_file
        IN ITEMS
          "bin/gti"
          "bin/gti_lsp"
          "lib/libgti_compiler.a"
          "lib/libgti_cpp_backend.a"
          "lib/libgti_driver.a"
          "lib/libgti_runtime.a"
          "lib/gti/llvm/libLLVMDemangle.a"
          "lib/gti/llvm/libLLVMSupport.a"
          "lib/gti/llvm/libLLVMTargetParser.a"
          "lib/cmake/GTI/GTIConfig.cmake"
          "lib/cmake/GTI/GTIConfigVersion.cmake"
          "lib/cmake/GTI/GTITargets.cmake"
          "include/gti/c_abi.h"
          "include/gti/failure.h"
          "include/gti/runtime_failure.h"
          "include/gti/runtime.h"
          "include/gti/runtime.hpp"
          "share/gti/VERSION"
          "share/gti/parser/gti.so"
          "share/gti/stdlib/prelude.gti"
          "share/gti/stdlib/std/array.gti"
          "share/gti/stdlib/std/string.gti"
          "share/gti/stdlib/std/tcp.gti"
          "share/gti/stdlib/std/vector.gti"
          "share/licenses/gti/GTI-LICENSE.txt"
          "share/licenses/gti/llvm-LICENSE.txt"
          "share/licenses/gti/tomlplusplus-LICENSE.txt")
  if(NOT EXISTS "${GTI_STAGE_DIR}/${required_file}")
    message(FATAL_ERROR "Release staging is missing ${required_file}")
  endif()
endforeach()

# Stamp the build identity into the staged toolchain before archiving. On the
# nightly channel VERSION does not change between builds, so this is the only
# fact an installed toolchain can compare to decide whether it is current.
if(GTI_CHANNEL STREQUAL "nightly")
  file(WRITE "${GTI_STAGE_DIR}/share/gti/BUILD_ID" "${GTI_BUILD_ID}\n")
endif()

file(MAKE_DIRECTORY "${GTI_OUTPUT_DIR}")
if(GTI_CHANNEL STREQUAL "nightly")
  set(GTI_ARCHIVE_NAME "gti-nightly-${GTI_PLATFORM}.tar.gz")
else()
  set(GTI_ARCHIVE_NAME "gti-v${GTI_VERSION}-${GTI_PLATFORM}.tar.gz")
endif()
set(GTI_ARCHIVE_PATH "${GTI_OUTPUT_DIR}/${GTI_ARCHIVE_NAME}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar czf "${GTI_ARCHIVE_PATH}" .
  WORKING_DIRECTORY "${GTI_STAGE_DIR}"
  RESULT_VARIABLE archive_result
  OUTPUT_VARIABLE archive_output
  ERROR_VARIABLE archive_error
)
if(NOT archive_result EQUAL 0)
  message(FATAL_ERROR
          "Failed to create ${GTI_ARCHIVE_NAME}: ${archive_output}${archive_error}")
endif()

file(SHA256 "${GTI_ARCHIVE_PATH}" GTI_ARCHIVE_SHA256)
file(WRITE "${GTI_ARCHIVE_PATH}.sha256"
     "${GTI_ARCHIVE_SHA256}  ${GTI_ARCHIVE_NAME}\n")

message(STATUS "Created ${GTI_ARCHIVE_PATH}")
