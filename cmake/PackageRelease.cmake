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
if(NOT GTI_CHANNEL STREQUAL "stable" AND NOT GTI_CHANNEL STREQUAL "nightly" AND NOT GTI_CHANNEL STREQUAL "preview")
  message(FATAL_ERROR "GTI_CHANNEL must be stable, nightly or preview")
endif()

file(STRINGS "${GTI_SOURCE_DIR}/VERSION" GTI_VERSION LIMIT_COUNT 1)
if(NOT GTI_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION does not contain a valid semantic version")
endif()

# A nightly archive is named for its channel rather than its version: the
# channel republishes one release per commit, so its tag never matches VERSION.
# Consumers tell nightly builds apart by BUILD_ID, not by VERSION.
if(GTI_CHANNEL STREQUAL "nightly" OR GTI_CHANNEL STREQUAL "preview")
  if(NOT DEFINED GTI_BUILD_ID OR "${GTI_BUILD_ID}" STREQUAL "")
    message(FATAL_ERROR "GTI_BUILD_ID is required for the ${GTI_CHANNEL} channel")
  endif()
  if(NOT GTI_BUILD_ID MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "GTI_BUILD_ID must be a commit identity")
  endif()
  if(DEFINED GTI_EXPECTED_TAG AND NOT GTI_EXPECTED_TAG STREQUAL GTI_CHANNEL)
    message(FATAL_ERROR
            "Rolling tag ${GTI_EXPECTED_TAG} must be ${GTI_CHANNEL}")
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
          "lib/libgti_llvm_backend.a"
          "lib/libgti_driver.a"
          "lib/libgti_runtime.a"
          "lib/gti/llvm/libLLVMDemangle.a"
          "lib/gti/llvm/libLLVMSupport.a"
          "lib/gti/llvm/libLLVMTargetParser.a"
          "lib/cmake/GTI/GTIConfig.cmake"
          "lib/cmake/GTI/GTIConfigVersion.cmake"
          "lib/cmake/GTI/GTITargets.cmake"
          "include/gti/c_abi.h"
          "include/gti/environment.h"
          "include/gti/failure.h"
          "include/gti/random.h"
          "include/gti/runtime_failure.h"
          "include/gti/runtime_thread.h"
          "include/gti/runtime_thread.hpp"
          "include/gti/runtime.h"
          "include/gti/runtime.hpp"
          "share/gti/VERSION"
          "share/gti/llvm-backend-archives.txt"
          "share/gti/parser/gti.so"
          "share/gti/stdlib/prelude.gti"
          "share/gti/stdlib/module-map.json"
          "share/gti/stdlib/std/array.gti"
          "share/gti/stdlib/std/environment.gti"
          "share/gti/stdlib/std/filesystem.gti"
          "share/gti/stdlib/std/random.gti"
          "share/gti/stdlib/std/string.gti"
          "share/gti/stdlib/std/tcp.gti"
          "share/gti/stdlib/std/thread.gti"
          "share/gti/stdlib/std/vector.gti"
          "share/licenses/gti/GTI-LICENSE.txt"
          "share/licenses/gti/clang-LICENSE.txt"
          "share/licenses/gti/llvm-LICENSE.txt"
          "share/licenses/gti/tomlplusplus-LICENSE.txt")
  if(NOT EXISTS "${GTI_STAGE_DIR}/${required_file}")
    message(FATAL_ERROR "Release staging is missing ${required_file}")
  endif()
endforeach()

file(STRINGS "${GTI_STAGE_DIR}/share/gti/llvm-backend-archives.txt"
     GTI_LLVM_BACKEND_ARCHIVES)
if(GTI_LLVM_BACKEND_ARCHIVES STREQUAL "")
  message(FATAL_ERROR "Release staging has an empty LLVM backend archive manifest")
endif()
foreach(archive IN LISTS GTI_LLVM_BACKEND_ARCHIVES)
  if(NOT EXISTS "${GTI_STAGE_DIR}/lib/gti/llvm/${archive}")
    message(FATAL_ERROR
            "Release staging is missing LLVM backend archive ${archive}")
  endif()
endforeach()

# libclang's file name is platform-shaped (libclang.so.MAJOR / libclang.dylib),
# so the presence check is a glob rather than one literal path.
file(GLOB GTI_STAGED_LIBCLANG "${GTI_STAGE_DIR}/lib/gti/llvm/libclang.*")
if(GTI_STAGED_LIBCLANG STREQUAL "")
  message(FATAL_ERROR
          "Release staging is missing lib/gti/llvm/libclang (the bindgen C reader's runtime library)")
endif()
if(GTI_PLATFORM MATCHES "^linux-")
  set(GTI_STAGED_LIBCLANG_SONAME_FOUND FALSE)
  foreach(candidate IN LISTS GTI_STAGED_LIBCLANG)
    get_filename_component(candidate_name "${candidate}" NAME)
    if(candidate_name MATCHES "^libclang\\.so\\.[0-9]" AND
       IS_SYMLINK "${candidate}")
      set(GTI_STAGED_LIBCLANG_SONAME_FOUND TRUE)
      break()
    endif()
  endforeach()
  if(NOT GTI_STAGED_LIBCLANG_SONAME_FOUND)
    message(
      FATAL_ERROR
        "Release staging is missing libclang's versioned SONAME symlink; the installed gti executable cannot load the bundled C reader")
  endif()
endif()

# macOS archives must come from the runtime-closure install step, never from
# an older system-LLVM stage with absolute Homebrew dependency paths.
if(GTI_PLATFORM MATCHES "^darwin-" AND
   NOT EXISTS "${GTI_STAGE_DIR}/share/gti/host-runtime.json")
  message(FATAL_ERROR "Release staging is missing the verified macOS runtime manifest")
endif()

# Stamp the build identity into the staged toolchain before archiving. On the
# nightly channel VERSION does not change between builds, so this is the only
# fact an installed toolchain can compare to decide whether it is current.
# A stable archive is channel-neutral: the release train publishes it as a
# beta and republishes the same bytes as stable, so a stage that a nightly
# packaging already stamped is refused rather than archived with that stamp.
if(GTI_CHANNEL STREQUAL "nightly" OR GTI_CHANNEL STREQUAL "preview")
  file(WRITE "${GTI_STAGE_DIR}/share/gti/BUILD_ID" "${GTI_BUILD_ID}\n")
elseif(EXISTS "${GTI_STAGE_DIR}/share/gti/BUILD_ID")
  message(FATAL_ERROR
          "Release staging carries a nightly share/gti/BUILD_ID; a stable archive is channel-neutral")
endif()

file(MAKE_DIRECTORY "${GTI_OUTPUT_DIR}")
if(GTI_CHANNEL STREQUAL "nightly" OR GTI_CHANNEL STREQUAL "preview")
  set(GTI_ARCHIVE_NAME "gti-${GTI_CHANNEL}-${GTI_PLATFORM}.tar.gz")
else()
  set(GTI_ARCHIVE_NAME "gti-v${GTI_VERSION}-${GTI_PLATFORM}.tar.gz")
endif()
set(GTI_ARCHIVE_PATH "${GTI_OUTPUT_DIR}/${GTI_ARCHIVE_NAME}")

# Do not materialize macOS metadata as new AppleDouble files on extraction:
# those files would change the installed runtime-header tree's cache identity.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar czf "${GTI_ARCHIVE_PATH}" --format=gnutar .
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
