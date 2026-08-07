cmake_minimum_required(VERSION 3.20)

foreach(required_variable
        IN ITEMS GTI_SOURCE_DIR GTI_STAGE_DIR GTI_OUTPUT_DIR GTI_PLATFORM)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

file(STRINGS "${GTI_SOURCE_DIR}/VERSION" GTI_VERSION LIMIT_COUNT 1)
if(NOT GTI_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION does not contain a valid semantic version")
endif()

if(DEFINED GTI_EXPECTED_TAG AND
   NOT GTI_EXPECTED_TAG STREQUAL "v${GTI_VERSION}")
  message(FATAL_ERROR
          "Release tag ${GTI_EXPECTED_TAG} does not match VERSION ${GTI_VERSION}")
endif()

foreach(required_file
        IN ITEMS
          "bin/gti"
          "bin/gti_lsp"
          "lib/libgti_runtime.a"
          "share/gti/VERSION"
          "share/gti/parser/gti.so"
          "share/gti/stdlib/prelude.gti"
          "share/gti/stdlib/std/array.gti"
          "share/gti/stdlib/std/string.gti"
          "share/licenses/gti/GTI-LICENSE.txt"
          "share/licenses/gti/json-c-LICENSE.txt")
  if(NOT EXISTS "${GTI_STAGE_DIR}/${required_file}")
    message(FATAL_ERROR "Release staging is missing ${required_file}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${GTI_OUTPUT_DIR}")
set(GTI_ARCHIVE_NAME "gti-v${GTI_VERSION}-${GTI_PLATFORM}.tar.gz")
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
