cmake_minimum_required(VERSION 3.20)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                       "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GTI_PROJECT_VERSION
     LIMIT_COUNT 1)
if(NOT GTI_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version such as 0.1.0")
endif()

project("gti" VERSION "${GTI_PROJECT_VERSION}" LANGUAGES C CXX)

option(GTI_BUILD_LSP "Build the GTI language server" ON)
option(GTI_BUNDLE_LLVM "Download and build pinned LLVM support libraries" OFF)
option(GTI_RELEASE_BUILD "Require a self-contained, release-ready toolchain" OFF)
option(GTI_BUILD_FUZZERS
       "Build opt-in libFuzzer harnesses (Clang only; never installed)" OFF)
option(GTI_ENABLE_THREAD_SANITIZER_TESTS
       "Run ThreadSanitizer test variants on a compatible host" ON)

# Enforce the host compiler configuration before dependency discovery.
include(CTest)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OptimizedTestCompiler.cmake")
gti_require_optimized_test_compiler()

find_program(GTI_GIT_EXECUTABLE git)
set(GTI_BUILD_IDENTITY_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/gti/build_identity_config.h")
add_custom_target(
  gti_build_identity_refresh
  COMMAND
    "${CMAKE_COMMAND}"
    "-DGTI_BUILD_IDENTITY_OUTPUT=${GTI_BUILD_IDENTITY_HEADER}"
    "-DGTI_SOURCE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DGTI_GIT_EXECUTABLE=${GTI_GIT_EXECUTABLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildIdentity.cmake"
  BYPRODUCTS "${GTI_BUILD_IDENTITY_HEADER}"
  COMMENT "Refreshing compiler source identity"
  VERBATIM
)

find_package(Threads REQUIRED)
# <std/http> loads the platform libcurl dynamically so the public runtime
# archive has no libcurl link dependency. Its stable C declarations and option
# constants still come from the build host's development headers.
find_package(CURL REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# An archive that `gti` links into generated executables is built with the
# host toolchain's view of the world, which knows nothing about this tree's
# compile flags. A sanitized tree (-fsanitize in CMAKE_CXX_FLAGS) therefore
# builds such archives uninstrumented, or every generated program fails to
# link against the __asan_*/__ubsan_* symbols their members expect. Sanitizing
# the runtime inside generated programs is the driver's instrumentation plan,
# which recompiles the runtime units with matching flags. The runtime archive
# is the toolchain product; tests that hand `gti` an archive of their own
# (a cleanup oracle component) declare it the same way. It is defined before
# the reused-toolchain branch because that graph registers the same tests.
function(gti_program_component target)
  if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=")
    target_compile_options("${target}" PRIVATE -fno-sanitize=all)
  endif()
endfunction()

set(GTI_REUSE_TOOLCHAIN "" CACHE PATH "Admitted production toolchain for source-only library CI")
option(GTI_REBUILD_LSP "Rebuild only the LSP against an admitted installed SDK" OFF)
if(GTI_REUSE_TOOLCHAIN)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReuseToolchain.cmake")
  return()
endif()
if(Python3_Interpreter_FOUND)
  add_custom_target(
    format
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/clang_format.py" --write
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Formatting first-party C and C++ sources"
    VERBATIM
  )
  add_custom_target(
    format-check
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/clang_format.py" --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking first-party C and C++ formatting"
    VERBATIM
  )
endif()

set(GTI_DIAGNOSTIC_REGISTRY_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/gti/diagnostic_registry_generated.h")
add_custom_command(
  OUTPUT "${GTI_DIAGNOSTIC_REGISTRY_HEADER}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_diagnostics.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/semantic.json"
    "${GTI_DIAGNOSTIC_REGISTRY_HEADER}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_diagnostics.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/semantic.json"
  COMMENT "Generating typed diagnostic registry"
  VERBATIM
)
add_custom_target(gti_diagnostic_registry
                  DEPENDS "${GTI_DIAGNOSTIC_REGISTRY_HEADER}")

if(GTI_RELEASE_BUILD)
  set(GTI_BUNDLE_LLVM ON)
endif()

set(GTI_BUILD_STDLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/stdlib")
set(GTI_BUILD_RUNTIME_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
set(GTI_BUILD_RUNTIME_LIBRARY_PATH "$<TARGET_FILE:gti_runtime>")
if(GTI_RELEASE_BUILD)
  set(GTI_BUILD_STDLIB_ROOT "")
  set(GTI_BUILD_RUNTIME_INCLUDE_DIR "")
  set(GTI_BUILD_RUNTIME_LIBRARY_PATH "")
endif()

# LLVM is a required compiler implementation dependency. GTI_BUNDLE_LLVM only
# controls how that dependency is acquired: ordinary builds use a supported
# installed LLVM, while release builds fetch the pinned version. The compiler
# core is confined to LLVMSupport, LLVMTargetParser, and LLVMDemangle. The
# independent native backend additionally owns the exact Core/native-codegen/Passes
# closure recorded below. LLVM headers stay private to compiled implementation
# targets and never become a public-header dependency.
#
# One LLVM for everyone: releases bundle exactly GTI_PINNED_LLVM_VERSION, and
# every other build (local development, CI's system-LLVM jobs) must use the
# same major version, so what is measured and tested is what ships.
# Distribution packages follow the release branch's point releases, so only
# the major is enforced there. GTI_ALLOW_UNPINNED_LLVM is an explicit,
# warned opt-out for trying a future LLVM; CI never sets it.
set(GTI_PINNED_LLVM_VERSION "23.1.4")
set(GTI_PINNED_LLVM_SHA256
    "819ecf7351f1a66f76b2d701d8942faaf5faeb324cb90c9d5b090de142434bb2")
string(REGEX MATCH "^[0-9]+" GTI_PINNED_LLVM_MAJOR "${GTI_PINNED_LLVM_VERSION}")
option(GTI_ALLOW_UNPINNED_LLVM
       "Allow an installed LLVM whose major version is not GTI's pinned one" OFF)
if(GTI_BUNDLE_LLVM)
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static bundled dependencies" FORCE)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" gti_llvm_host_processor)
  if(gti_llvm_host_processor MATCHES "^(arm64|aarch64)$")
    set(GTI_LLVM_NATIVE_TARGET "AArch64")
  elseif(gti_llvm_host_processor MATCHES "^(x86_64|amd64)$")
    set(GTI_LLVM_NATIVE_TARGET "X86")
  else()
    message(
      FATAL_ERROR
        "GTI's bundled LLVM backend supports only arm64/aarch64 and x86_64 hosts, not ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(LLVM_TARGETS_TO_BUILD "${GTI_LLVM_NATIVE_TARGET}" CACHE STRING
      "GTI native LLVM backend host target" FORCE)
  set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Skip LLVM tests" FORCE)
  set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "Skip LLVM examples" FORCE)
  set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Skip LLVM benchmarks" FORCE)
  set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "Skip LLVM docs" FORCE)
  set(LLVM_ENABLE_ZLIB OFF CACHE STRING "No compression dependency" FORCE)
  set(LLVM_ENABLE_ZSTD OFF CACHE STRING "No compression dependency" FORCE)
  set(LLVM_ENABLE_LIBXML2 OFF CACHE STRING "No xml dependency" FORCE)
  set(LLVM_ENABLE_LIBEDIT OFF CACHE STRING "No libedit dependency" FORCE)
  set(LLVM_ENABLE_TERMINFO OFF CACHE STRING "No terminfo dependency" FORCE)
  set(LLVM_ENABLE_Z3_SOLVER OFF CACHE BOOL "No Z3 dependency" FORCE)
  set(LLVM_ENABLE_FFI OFF CACHE BOOL "No libffi dependency" FORCE)
  set(LLVM_ENABLE_LIBPFM OFF CACHE BOOL "No libpfm dependency" FORCE)
  set(LLVM_ENABLE_CURL OFF CACHE BOOL "No curl dependency" FORCE)
  set(LLVM_ENABLE_HTTPLIB OFF CACHE BOOL "No httplib dependency" FORCE)
  set(LLVM_ENABLE_ASSERTIONS OFF CACHE BOOL "Release LLVM semantics" FORCE)
  # GTI compiles with RTTI enabled and instantiates LLVM templates that carry
  # typeinfo (llvm::Expected in src/compiler/binary_float.cpp needs
  # `typeinfo for llvm::ErrorInfoBase`). LLVM defaults to RTTI off, which link-
  # fails against an RTTI-enabled consumer. The distribution packages GTI is
  # normally built against (Debian llvm-*-dev, Homebrew llvm@*) enable RTTI, so
  # bundling must match them: GTI_BUNDLE_LLVM only changes how the dependency
  # is acquired, never what it provides.
  set(LLVM_ENABLE_RTTI ON CACHE BOOL "GTI links LLVM from RTTI-enabled code" FORCE)
  # The clang project provides exactly one additional GTI link surface:
  # libclang, the C API the bindgen reader uses (ADR 021). EXCLUDE_FROM_ALL
  # keeps the rest of clang's build graph out of GTI's `all`; only libclang
  # and its implementation dependencies build, on demand.
  set(LLVM_ENABLE_PROJECTS "clang" CACHE STRING
      "GTI bindgen links libclang" FORCE)
  set(gti_llvm_release_url
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${GTI_PINNED_LLVM_VERSION}/llvm-project-${GTI_PINNED_LLVM_VERSION}.src.tar.xz")
  # FetchContent downloads once and gives up, and GitHub's release CDN returns
  # transient 5xx responses (full validation run 41995172449 lost two Linux
  # shards and a package to one 504). Download with retries, verify the pinned
  # hash, and hand FetchContent the verified file; it re-verifies the hash.
  # GTI_LLVM_SOURCE_CACHE (environment) names a directory kept across build
  # trees, so local builds download the 179 MB archive once.
  set(gti_llvm_source_dir "$ENV{GTI_LLVM_SOURCE_CACHE}")
  if(NOT gti_llvm_source_dir)
    set(gti_llvm_source_dir "${CMAKE_BINARY_DIR}/_deps/llvm-source")
  endif()
  set(GTI_LLVM_SOURCE_URL
      "${gti_llvm_source_dir}/llvm-project-${GTI_PINNED_LLVM_VERSION}.src.tar.xz")
  set(gti_llvm_source_ready FALSE)
  if(EXISTS "${GTI_LLVM_SOURCE_URL}")
    file(SHA256 "${GTI_LLVM_SOURCE_URL}" gti_llvm_source_hash)
    if(gti_llvm_source_hash STREQUAL GTI_PINNED_LLVM_SHA256)
      set(gti_llvm_source_ready TRUE)
    endif()
  endif()
  set(gti_llvm_download_attempts 6)
  foreach(gti_llvm_attempt RANGE 1 ${gti_llvm_download_attempts})
    if(gti_llvm_source_ready)
      break()
    endif()
    file(MAKE_DIRECTORY "${gti_llvm_source_dir}")
    file(DOWNLOAD "${gti_llvm_release_url}" "${GTI_LLVM_SOURCE_URL}.part"
         STATUS gti_llvm_download_status TLS_VERIFY ON INACTIVITY_TIMEOUT 120)
    list(GET gti_llvm_download_status 0 gti_llvm_download_code)
    list(GET gti_llvm_download_status 1 gti_llvm_download_message)
    if(gti_llvm_download_code EQUAL 0)
      file(SHA256 "${GTI_LLVM_SOURCE_URL}.part" gti_llvm_source_hash)
      if(gti_llvm_source_hash STREQUAL GTI_PINNED_LLVM_SHA256)
        file(RENAME "${GTI_LLVM_SOURCE_URL}.part" "${GTI_LLVM_SOURCE_URL}")
        set(gti_llvm_source_ready TRUE)
        break()
      endif()
      set(gti_llvm_download_message "SHA-256 ${gti_llvm_source_hash} is not the pinned ${GTI_PINNED_LLVM_SHA256}")
    endif()
    file(REMOVE "${GTI_LLVM_SOURCE_URL}.part")
    if(gti_llvm_attempt LESS gti_llvm_download_attempts)
      math(EXPR gti_llvm_retry_delay "10 * ${gti_llvm_attempt}")
      message(STATUS "GTI: LLVM ${GTI_PINNED_LLVM_VERSION} source download attempt ${gti_llvm_attempt} failed (${gti_llvm_download_message}); retrying in ${gti_llvm_retry_delay} s")
      execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${gti_llvm_retry_delay})
    else()
      message(FATAL_ERROR "GTI: could not download LLVM ${GTI_PINNED_LLVM_VERSION} from ${gti_llvm_release_url} after ${gti_llvm_download_attempts} attempts (${gti_llvm_download_message})")
    endif()
  endforeach()
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    FetchContent_Declare(
      llvm_project
      URL "${GTI_LLVM_SOURCE_URL}"
      URL_HASH
        SHA256=${GTI_PINNED_LLVM_SHA256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SOURCE_SUBDIR llvm
      EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(llvm_project)
  else()
    FetchContent_Declare(
      llvm_project
      URL "${GTI_LLVM_SOURCE_URL}"
      URL_HASH
        SHA256=${GTI_PINNED_LLVM_SHA256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(llvm_project)
    if(NOT llvm_project_POPULATED)
      FetchContent_Populate(llvm_project)
      # EXCLUDE_FROM_ALL keeps LLVM's own `all` out of GTI's; only the three
      # libraries GTI links are built, on demand.
      add_subdirectory("${llvm_project_SOURCE_DIR}/llvm"
                       "${llvm_project_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
  endif()
  set(GTI_LLVM_VERSION "${GTI_PINNED_LLVM_VERSION}")
  set(GTI_LLVM_INCLUDE_DIRS "${llvm_project_SOURCE_DIR}/llvm/include"
                            "${llvm_project_BINARY_DIR}/include")
  set(GTI_CLANG_INCLUDE_DIRS
      "${llvm_project_SOURCE_DIR}/clang/include"
      "${llvm_project_BINARY_DIR}/tools/clang/include")
  message(STATUS "GTI: bundled LLVM ${GTI_LLVM_VERSION} support libraries enabled")
else()
  # Without LLVM_DIR, look first where the pinned major is normally installed.
  find_package(LLVM CONFIG QUIET
    HINTS "/opt/homebrew/opt/llvm@${GTI_PINNED_LLVM_MAJOR}/lib/cmake/llvm"
          "/usr/local/opt/llvm@${GTI_PINNED_LLVM_MAJOR}/lib/cmake/llvm"
          "/usr/lib/llvm-${GTI_PINNED_LLVM_MAJOR}/lib/cmake/llvm")
  set(gti_llvm_install_hint
      "Install LLVM ${GTI_PINNED_LLVM_MAJOR} (macOS: brew install llvm@${GTI_PINNED_LLVM_MAJOR}; Ubuntu: llvm-${GTI_PINNED_LLVM_MAJOR}-dev libclang-${GTI_PINNED_LLVM_MAJOR}-dev from apt.llvm.org) and set LLVM_DIR to its lib/cmake/llvm, or configure with -DGTI_BUNDLE_LLVM=ON to build the pinned LLVM ${GTI_PINNED_LLVM_VERSION}.")
  if(NOT LLVM_FOUND)
    message(FATAL_ERROR "GTI requires LLVM ${GTI_PINNED_LLVM_MAJOR}. ${gti_llvm_install_hint}")
  endif()
  if(NOT LLVM_VERSION_MAJOR EQUAL GTI_PINNED_LLVM_MAJOR)
    if(GTI_ALLOW_UNPINNED_LLVM)
      message(
        WARNING
          "GTI pins LLVM ${GTI_PINNED_LLVM_MAJOR}, but GTI_ALLOW_UNPINNED_LLVM selected LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}. Results from this build, including benchmarks, do not describe shipped GTI.")
    else()
      message(
        FATAL_ERROR
          "GTI pins LLVM ${GTI_PINNED_LLVM_MAJOR} (releases bundle ${GTI_PINNED_LLVM_VERSION}), but LLVM_DIR selects LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}. ${gti_llvm_install_hint}")
    endif()
  endif()
  # Without this check the mismatch surfaces as an undefined `typeinfo for
  # llvm::ErrorInfoBase` at link time, which does not name its own cause.
  if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
    message(
      FATAL_ERROR
        "GTI requires an LLVM built with RTTI, but LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR} was built with LLVM_ENABLE_RTTI=OFF. Use a distribution package (Debian llvm-*-dev, Homebrew llvm@*), rebuild LLVM with -DLLVM_ENABLE_RTTI=ON, or configure GTI with -DGTI_BUNDLE_LLVM=ON.")
  endif()
  set(GTI_LLVM_VERSION "${LLVM_PACKAGE_VERSION}")
  set(GTI_LLVM_INCLUDE_DIRS ${LLVM_INCLUDE_DIRS})
  message(STATUS "GTI: system LLVM ${GTI_LLVM_VERSION} support libraries enabled")
  # libclang ships in the Clang CMake package beside the LLVM one (Debian
  # libclang-*-dev, Homebrew llvm@*). The hint covers installations whose
  # clang package directory sits beside the LLVM package directory.
  find_package(Clang ${GTI_LLVM_VERSION} EXACT CONFIG QUIET
               HINTS "${LLVM_DIR}/../clang")
  if(NOT Clang_FOUND)
    message(
      FATAL_ERROR
        "GTI requires the Clang CMake package matching LLVM ${GTI_LLVM_VERSION} for libclang (Debian: libclang-XX-dev; Homebrew llvm@* includes it). Set Clang_DIR to its lib/cmake/clang directory or configure with -DGTI_BUNDLE_LLVM=ON.")
  endif()
  set(GTI_CLANG_INCLUDE_DIRS ${CLANG_INCLUDE_DIRS})
endif()

foreach(gti_llvm_target IN ITEMS LLVMSupport LLVMTargetParser LLVMDemangle)
  if(NOT TARGET "${gti_llvm_target}")
    message(FATAL_ERROR
            "LLVM ${GTI_LLVM_VERSION} does not provide ${gti_llvm_target}")
  endif()
  if(GTI_BUNDLE_LLVM)
    get_target_property(gti_llvm_target_type "${gti_llvm_target}" TYPE)
    if(NOT gti_llvm_target_type STREQUAL "STATIC_LIBRARY")
      message(
        FATAL_ERROR
          "Bundled ${gti_llvm_target} must be static for release packaging, but CMake created a ${gti_llvm_target_type} target")
    endif()
  endif()
endforeach()

# Resolve the smallest LLVM component roots needed by the backend foundation.
# Map canonical component targets before walking their auditable dependency
# closure; do not resolve a bare shared C API target such as Remarks.
if(NOT COMMAND llvm_map_components_to_libnames)
  include("${LLVM_CMAKE_DIR}/LLVM-Config.cmake")
endif()
if(GTI_BUNDLE_LLVM)
  # LLVM's native-architecture variable is local to its added subdirectory.
  set(LLVM_NATIVE_ARCH "${GTI_LLVM_NATIVE_TARGET}")
endif()
llvm_map_components_to_libnames(
  GTI_LLVM_BACKEND_DIRECT_LIBRARIES core nativecodegen passes)
if(NOT GTI_LLVM_BACKEND_DIRECT_LIBRARIES)
  message(FATAL_ERROR
          "LLVM ${GTI_LLVM_VERSION} did not resolve core/nativecodegen/passes")
endif()

function(gti_collect_llvm_library_closure output)
  set(queue ${ARGN})
  set(closure)
  while(queue)
    list(POP_FRONT queue current)
    if(current IN_LIST closure)
      continue()
    endif()
    if(NOT TARGET "${current}")
      message(FATAL_ERROR
              "LLVM backend dependency ${current} is not a CMake target")
    endif()
    list(APPEND closure "${current}")
    get_target_property(dependencies "${current}" INTERFACE_LINK_LIBRARIES)
    if(dependencies)
      foreach(dependency IN LISTS dependencies)
        string(REGEX MATCHALL "LLVM[A-Za-z0-9_]+" llvm_dependencies
                             "${dependency}")
        foreach(llvm_dependency IN LISTS llvm_dependencies)
          if(TARGET "${llvm_dependency}" AND
             NOT llvm_dependency IN_LIST closure)
            list(APPEND queue "${llvm_dependency}")
          endif()
        endforeach()
      endforeach()
    endif()
  endwhile()
  set(${output} "${closure}" PARENT_SCOPE)
endfunction()

gti_collect_llvm_library_closure(
  GTI_LLVM_BACKEND_LIBRARY_CLOSURE ${GTI_LLVM_BACKEND_DIRECT_LIBRARIES})
string(REPLACE ";" "\n" GTI_LLVM_BACKEND_COMPONENT_MANIFEST
               "${GTI_LLVM_BACKEND_LIBRARY_CLOSURE}")
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gti-llvm-backend-components.txt"
  CONTENT "${GTI_LLVM_BACKEND_COMPONENT_MANIFEST}\n")

function(gti_llvm_package_target source output)
  if(source STREQUAL "LLVMSupport")
    set(result "GTI_LLVM::Support")
  elseif(source STREQUAL "LLVMTargetParser")
    set(result "GTI_LLVM::TargetParser")
  elseif(source STREQUAL "LLVMDemangle")
    set(result "GTI_LLVM::Demangle")
  else()
    set(result "GTI_LLVM::${source}")
  endif()
  set(${output} "${result}" PARENT_SCOPE)
endfunction()

set(GTI_BUNDLED_LLVM_BACKEND_IMPORTS "")
set(GTI_LLVM_BACKEND_ARCHIVE_TARGETS "")
if(GTI_BUNDLE_LLVM)
  foreach(component IN LISTS GTI_LLVM_BACKEND_LIBRARY_CLOSURE)
    get_target_property(component_type "${component}" TYPE)
    if(NOT component_type STREQUAL "STATIC_LIBRARY")
      message(
        FATAL_ERROR
          "Bundled LLVM backend component ${component} must be static, but CMake created ${component_type}")
    endif()
    if(NOT component MATCHES "^(LLVMSupport|LLVMTargetParser|LLVMDemangle)$")
      list(APPEND GTI_LLVM_BACKEND_ARCHIVE_TARGETS "${component}")
      gti_llvm_package_target("${component}" imported_component)
      string(APPEND GTI_BUNDLED_LLVM_BACKEND_IMPORTS
             "  _gti_import_llvm_archive(${imported_component} \"${CMAKE_STATIC_LIBRARY_PREFIX}${component}${CMAKE_STATIC_LIBRARY_SUFFIX}\")\n")
    endif()
  endforeach()

  foreach(component IN LISTS GTI_LLVM_BACKEND_ARCHIVE_TARGETS)
    get_target_property(dependencies "${component}" INTERFACE_LINK_LIBRARIES)
    set(imported_dependencies)
    if(dependencies)
      foreach(dependency IN LISTS dependencies)
        string(REGEX MATCHALL "LLVM[A-Za-z0-9_]+" llvm_dependencies
                             "${dependency}")
        foreach(llvm_dependency IN LISTS llvm_dependencies)
          if(llvm_dependency IN_LIST GTI_LLVM_BACKEND_LIBRARY_CLOSURE)
            gti_llvm_package_target("${llvm_dependency}" imported_dependency)
            list(APPEND imported_dependencies "${imported_dependency}")
          endif()
        endforeach()
      endforeach()
    endif()
    list(REMOVE_DUPLICATES imported_dependencies)
    gti_llvm_package_target("${component}" imported_component)
    string(APPEND GTI_BUNDLED_LLVM_BACKEND_IMPORTS
           "  set_target_properties(${imported_component} PROPERTIES INTERFACE_LINK_LIBRARIES \"${imported_dependencies}\")\n")
  endforeach()

  set(imported_direct_libraries)
  foreach(component IN LISTS GTI_LLVM_BACKEND_DIRECT_LIBRARIES)
    gti_llvm_package_target("${component}" imported_component)
    list(APPEND imported_direct_libraries "${imported_component}")
  endforeach()
  string(APPEND GTI_BUNDLED_LLVM_BACKEND_IMPORTS
         "  add_library(GTI_LLVM::Backend INTERFACE IMPORTED)\n"
         "  set_target_properties(GTI_LLVM::Backend PROPERTIES INTERFACE_LINK_LIBRARIES \"${imported_direct_libraries}\")\n")
endif()

# The bindgen reader links exactly one clang-project library: libclang, the
# stable C API (ADR 021). Component libraries (clangAST, clang-cpp, ...) stay
# out of scope; scripts/check_llvm_link_surface.py enforces this in every
# build.
if(NOT TARGET libclang)
  message(
    FATAL_ERROR
      "LLVM ${GTI_LLVM_VERSION} does not provide the libclang target GTI bindgen requires")
endif()

set(GTI_BUNDLED_LLVM_SUPPORT_SYSTEM_LIBRARIES "")
if(GTI_BUNDLE_LLVM)
  get_target_property(
    GTI_BUNDLED_LLVM_SUPPORT_SYSTEM_LIBRARIES
    LLVMSupport LLVM_SYSTEM_LIBS
  )
  if(NOT GTI_BUNDLED_LLVM_SUPPORT_SYSTEM_LIBRARIES)
    set(GTI_BUNDLED_LLVM_SUPPORT_SYSTEM_LIBRARIES "")
  endif()
endif()

# First-party code builds warning-clean under -Werror on both clang and GCC so
# the macOS/Linux toolchain divergence class (e.g. C++20 designator order,
# which clang only warns about) breaks locally instead of in CI.
#
# The position of this block is load-bearing. `add_compile_options` applies to
# every target defined after it in this directory and below, which includes the
# LLVM subproject that GTI_BUNDLE_LLVM adds through FetchContent. Vendored LLVM
# is not warning-clean under these flags and GTI cannot fix it, so the options
# are set here — after the LLVM block, before the first GTI target. Any new
# first-party target must be defined below this point to be covered.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # Both compilers extend -Wmissing-field-initializers to designated
  # initializers whose omitted members are value-initialized — a C++20 idiom
  # this codebase uses deliberately (effect tables, request structs). Neither
  # loses coverage of positional omissions. Everything else stays -Werror.
  add_compile_options(-Wall -Wextra -Werror -Wno-missing-field-initializers)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GNU-only: under optimization -Wmaybe-uninitialized fires inside libstdc++'s
  # own <variant> machinery, a long-standing GCC false-positive family. The
  # definite -Wuninitialized analysis stays active. Clang has no such flag and
  # rejects it.
  add_compile_options(-Wno-maybe-uninitialized)
endif()

if(GTI_BUILD_FUZZERS)
  # Fuzzing instruments the whole tree (position load-bearing, same rule as
  # the warning flags above): the compiler library must carry coverage and
  # sanitizer instrumentation or libFuzzer explores blind. Harness executables
  # in fuzz/ additionally link -fsanitize=fuzzer for the driver.
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "GTI_BUILD_FUZZERS requires a Clang toolchain with "
                        "libFuzzer; the current compiler is "
                        "${CMAKE_CXX_COMPILER_ID}.")
  endif()
  # "Is Clang" is not enough: Apple's CommandLineTools clang ships without
  # libclang_rt.fuzzer and only fails at link. Probe the runtime at configure
  # time so the failure is immediate and names the fix.
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
  check_cxx_source_compiles("
#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t *, size_t) { return 0; }
" GTI_TOOLCHAIN_HAS_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  if(NOT GTI_TOOLCHAIN_HAS_LIBFUZZER)
    message(FATAL_ERROR "This Clang cannot link -fsanitize=fuzzer (Apple "
                        "CommandLineTools omits libclang_rt.fuzzer). Use a "
                        "full LLVM toolchain, e.g. "
                        "-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@23/bin/clang++.")
  endif()
  if(GTI_RELEASE_BUILD)
    message(FATAL_ERROR "GTI_BUILD_FUZZERS and GTI_RELEASE_BUILD are mutually "
                        "exclusive; fuzzers never enter release artifacts.")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
                      -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(
  "gti_semantic_analysis" OBJECT
  "src/compiler/semantic_analyzer.cpp"
  "src/compiler/semantic_body_analysis.cpp"
  "src/compiler/semantic_macro.cpp"
  "src/compiler/semantic_constexpr.cpp"
  "src/compiler/semantic_call_resolution.cpp"
  "src/compiler/semantic_requirement_predicates.cpp"
  "src/compiler/semantic_requirement_expressions.cpp"
  "src/compiler/semantic_protocol_concepts.cpp"
  "src/compiler/semantic_tryable_propagation.cpp"
  "src/compiler/semantic_declaration_analysis.cpp"
  "src/compiler/semantic_embedding.cpp"
  "src/compiler/semantic_flow_analysis.cpp"
  "src/compiler/semantic_ownership_analysis.cpp"
  "src/compiler/semantic_callable_erasure.cpp"
  "src/compiler/semantic_type_resolution.cpp"
)
target_include_directories(
  "gti_semantic_analysis"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
          "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_include_directories(
  "gti_semantic_analysis" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS}
)
target_compile_features("gti_semantic_analysis" PRIVATE cxx_std_20)
target_precompile_headers(
  "gti_semantic_analysis"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/semantic_analyzer_impl.h"
)
add_dependencies("gti_semantic_analysis" gti_diagnostic_registry)

add_library(
  "gti_compiler" STATIC
  "src/compiler/backend.cpp"
  $<TARGET_OBJECTS:gti_semantic_analysis>
  "src/compiler/ast_clone.cpp"
  "src/compiler/ast_printer.cpp"
  "src/compiler/binary_float.cpp"
  "src/compiler/checked_integer.cpp"
  "src/compiler/compile_limits.cpp"
  "src/compiler/configuration_directive_processor.cpp"
  "src/compiler/constant_evaluator.cpp"
  "src/compiler/mir_constant_evaluator.cpp"
  "src/compiler/diagnostic.cpp"
  "src/compiler/documentation.cpp"
  "src/compiler/executable_path.cpp"
  "src/compiler/failure_metadata.cpp"
  "src/compiler/format_config.cpp"
  "src/compiler/formatter.cpp"
  "src/compiler/frontend.cpp"
  "src/compiler/include_suggestions.cpp"
  "src/compiler/frontend_cache.cpp"
  "src/compiler/instance_analysis_cache.cpp"
  "src/compiler/hir.cpp"
  "src/compiler/hir_model.cpp"
  "src/compiler/hir_lowering.cpp"
  "src/compiler/language_queries.cpp"
  "src/compiler/language_snippets.cpp"
  "src/compiler/lexer.cpp"
  "src/compiler/loaded_lexical_unit.cpp"
  "src/compiler/lowered_program.cpp"
  "src/compiler/lowered_program_builder.cpp"
  "src/compiler/lowered_program_closure.cpp"
  "src/compiler/lowered_program_verifier.cpp"
  "src/compiler/lowered_program_printer.cpp"
  "src/compiler/mir.cpp"
  "src/compiler/mir_body_lowering.cpp"
  "src/compiler/mir_dominance.cpp"
  "src/compiler/mir_failure_effects.cpp"
  "src/compiler/mir_hosted_startup_builder.cpp"
  "src/compiler/mir_lowering.cpp"
  "src/compiler/mir_optimization_verifier.cpp"
  "src/compiler/mir_printer.cpp"
  "src/compiler/mir_program_verifier.cpp"
  "src/compiler/mir_thread_local.cpp"
  "src/compiler/mir_storage_effects.cpp"
  "src/compiler/mir_text_contracts.cpp"
  "src/compiler/mir_verification_queries.cpp"
  "src/compiler/mir_backend.cpp"
  "src/compiler/native_header.cpp"
  "src/compiler/optimization/alias.cpp"
  "src/compiler/optimization/coverage.cpp"
  "src/compiler/optimization/closed_world.cpp"
  "src/compiler/optimization/devirtualization.cpp"
  "src/compiler/mir_effects.cpp"
  "src/compiler/optimization/loops.cpp"
  "src/compiler/optimization/pass.cpp"
  "src/compiler/optimization/report.cpp"
  "src/compiler/mir_editor.cpp"
  "src/compiler/optimization/safety.cpp"
  "src/compiler/optimization/sccp.cpp"
  "src/compiler/optimizer.cpp"
  "src/compiler/parser.cpp"
  "src/compiler/syntax_capture.cpp"
  "src/compiler/procedural_syntax.cpp"
  "src/compiler/process_memory_runtime.cpp"
  "src/compiler/semantic_model.cpp"
  "src/compiler/persistent_declaration.cpp"
  "src/compiler/compiled_scalar_unit.cpp"
  "src/compiler/compiled_interface_types.cpp"
  "src/compiler/semantic_instance_reuse.cpp"
  "src/compiler/semantic_preamble.cpp"
  "src/compiler/semantic_diagnostics.cpp"
  "src/compiler/semantic_spelling.cpp"
  "src/compiler/semantic_type_printer.cpp"
  "src/compiler/source_graph.cpp"
  "src/compiler/source_loader.cpp"
  "src/compiler/standard_library.cpp"
  "src/compiler/support.cpp"
  "src/compiler/target.cpp"
  "src/compiler/token.cpp"
)
target_include_directories("gti_compiler" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_include_directories("gti_compiler" PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies("gti_compiler" gti_diagnostic_registry)
target_link_libraries(
  "gti_compiler"
  PRIVATE
    "$<BUILD_INTERFACE:LLVMSupport>"
    "$<BUILD_INTERFACE:LLVMTargetParser>"
    "$<BUILD_INTERFACE:LLVMDemangle>"
    "$<INSTALL_INTERFACE:GTI_LLVM::Support>"
    "$<INSTALL_INTERFACE:GTI_LLVM::TargetParser>"
    "$<INSTALL_INTERFACE:GTI_LLVM::Demangle>"
)
target_include_directories(
  "gti_compiler"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features("gti_compiler" PUBLIC cxx_std_20)

add_library(
  "gti_cpp_backend" STATIC
  "src/compiler/cpp_backend.cpp"
  "src/compiler/cpp_emitter.cpp"
  "src/compiler/cpp_failure_support_writer.cpp"
  "src/compiler/cpp_mir_control_flow.cpp"
  "src/compiler/cpp_representation.cpp"
  "src/compiler/cpp_mir_body_emitter.cpp"
  "src/compiler/cpp_mir_body_emission_plan.cpp"
  "src/compiler/cpp_mir_body_text_writer.cpp"
  "src/compiler/cpp_mir_program_plan.cpp"
  "src/compiler/cpp_mir_representation_snapshot.cpp"
  "src/compiler/cpp_program_emission_plan.cpp"
  "src/compiler/cpp_program_emission_planner.cpp"
  "src/compiler/cpp_program_text_writer.cpp"
  "src/compiler/cpp_support_preamble.cpp"
)
target_include_directories(
  "gti_cpp_backend"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories("gti_cpp_backend" SYSTEM PRIVATE
                           ${GTI_LLVM_INCLUDE_DIRS})
target_compile_features("gti_cpp_backend" PUBLIC cxx_std_20)
target_link_libraries("gti_cpp_backend" PUBLIC gti_compiler)

add_library(
  "gti_llvm_backend" STATIC
  "src/compiler/llvm_backend.cpp"
  "src/compiler/llvm_embedding.cpp"
  "src/compiler/llvm/admission.cpp"
  "src/compiler/llvm/coverage.cpp"
  "src/compiler/llvm/session.cpp"
  "src/compiler/llvm/loop_invariants.cpp"
  "src/compiler/llvm/checked_reductions.cpp"
  "src/compiler/llvm/checked_ranges.cpp"
  "src/compiler/llvm/checked_offsets.cpp"
  "src/compiler/llvm/private_atomics.cpp"
  "src/compiler/llvm/debug_info.cpp"
  "src/compiler/llvm/instrumentation.cpp"
  "src/compiler/llvm/layout.cpp"
  "src/compiler/llvm/abi.cpp"
  "src/compiler/llvm/dispatch.cpp"
  "src/compiler/llvm/native_c_abi.cpp"
  "src/compiler/llvm/embedding.cpp"
  "src/compiler/llvm/runtime_abi.cpp"
  "src/compiler/llvm/hosted.cpp"
  "src/compiler/llvm/reachability.cpp"
  "src/compiler/llvm/failure.cpp"
  "src/compiler/llvm/scalar.cpp"
  "src/compiler/llvm/atomics.cpp"
  "src/compiler/llvm/mutexes.cpp"
  "src/compiler/llvm/simd.cpp"
  "src/compiler/llvm/closure_environment.cpp"
  "src/compiler/llvm/constructor_field_publication.cpp"
  "src/compiler/llvm/callables.cpp"
  "src/compiler/llvm/parameter_packs.cpp"
  "src/compiler/llvm/body.cpp"
  "src/compiler/llvm/places.cpp"
  "src/compiler/llvm/placement.cpp"
  "src/compiler/llvm/lifetimes.cpp"
  "src/compiler/llvm/polymorphic_storage.cpp"
  "src/compiler/llvm/memory.cpp"
  "src/compiler/llvm/thread_locals.cpp"
  "src/compiler/llvm/thread_lifecycle.cpp"
  "src/compiler/llvm/threads.cpp"
  "src/compiler/llvm/text.cpp"
)
file(
  GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/gti_llvm_backend_sources.txt"
  CONTENT "$<JOIN:$<TARGET_PROPERTY:gti_llvm_backend,SOURCES>,\n>\n"
)
target_include_directories(
  "gti_llvm_backend"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories("gti_llvm_backend" SYSTEM PRIVATE
                           ${GTI_LLVM_INCLUDE_DIRS})
target_compile_features("gti_llvm_backend" PUBLIC cxx_std_20)
target_link_libraries(
  "gti_llvm_backend"
  PUBLIC gti_compiler
  PRIVATE "$<BUILD_INTERFACE:${GTI_LLVM_BACKEND_DIRECT_LIBRARIES}>"
          "$<INSTALL_INTERFACE:GTI_LLVM::Backend>"
)

# System libclang can depend on a shared libLLVM while GTI deliberately links
# only its owned static components. ELF executable symbols otherwise preempt
# the shared copy's ManagedStatic/command-line registries, corrupting startup
# and shutdown. Isolate LLVM implementation symbols only, not GTI/native API
# exports. INTERFACE propagation protects installed static-library consumers.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(gti_llvm_support_archives
      "${CMAKE_STATIC_LIBRARY_PREFIX}LLVMSupport${CMAKE_STATIC_LIBRARY_SUFFIX}"
      "${CMAKE_STATIC_LIBRARY_PREFIX}LLVMTargetParser${CMAKE_STATIC_LIBRARY_SUFFIX}"
      "${CMAKE_STATIC_LIBRARY_PREFIX}LLVMDemangle${CMAKE_STATIC_LIBRARY_SUFFIX}")
  list(JOIN gti_llvm_support_archives ":" gti_llvm_support_exclusions)
  target_link_options(gti_compiler INTERFACE
                      "LINKER:--exclude-libs,${gti_llvm_support_exclusions}")
  set(gti_llvm_backend_archives)
  foreach(component IN LISTS GTI_LLVM_BACKEND_LIBRARY_CLOSURE)
    list(APPEND gti_llvm_backend_archives
         "${CMAKE_STATIC_LIBRARY_PREFIX}${component}${CMAKE_STATIC_LIBRARY_SUFFIX}")
  endforeach()
  list(JOIN gti_llvm_backend_archives ":" gti_llvm_backend_exclusions)
  target_link_options(gti_llvm_backend INTERFACE
                      "LINKER:--exclude-libs,${gti_llvm_backend_exclusions}")
endif()

add_library(
  "gti_runtime" STATIC
  "runtime/src/execution.cpp"
  "runtime/src/embedding.cpp"
  "runtime/src/charconv.cpp"
  "runtime/src/chrono.cpp"
  "runtime/src/cthread.cpp"
  "runtime/src/cthread_sleep.cpp"
  "runtime/src/cthread_mutex.cpp"
  "runtime/src/cthread_state.cpp"
  "runtime/src/environment.cpp"
  "runtime/src/failure.cpp"
  "runtime/src/filesystem.cpp"
  "runtime/src/http.cpp"
  "runtime/src/io.cpp"
  "runtime/src/math.cpp"
  "runtime/src/memory.cpp"
  "runtime/src/owner_memory.cpp"
  "runtime/src/c_string.cpp"
  "runtime/src/process.cpp"
  "runtime/src/random.cpp"
  "runtime/src/thread.cpp"
)
if((APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$") OR
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"))
  target_sources(gti_runtime PRIVATE runtime/src/atomic_wait.cpp runtime/src/mutex.cpp)
  set_source_files_properties(runtime/src/atomic_wait.cpp
                              PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
  set(GTI_NATIVE_SYNCHRONIZATION_SERVICES ON)
endif()
target_include_directories(
  "gti_runtime"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories("gti_runtime" SYSTEM PRIVATE ${CURL_INCLUDE_DIRS})
target_compile_features("gti_runtime" PUBLIC cxx_std_20)
set_target_properties(gti_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
gti_program_component(gti_runtime)
set_source_files_properties(runtime/src/embedding.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fvisibility=hidden>")
target_link_libraries("gti_runtime" PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RuntimeCharconv.cmake")
gti_configure_runtime_charconv()
if(WIN32)
  # gti_rt_random_bytes calls BCryptGenRandom.
  target_link_libraries("gti_runtime" PRIVATE "bcrypt")
endif()

# Read-only project and frontend services are shared by CLI and LSP without
# introducing code-generation or bindgen dependencies. Registry resolution
# retains the small shared runtime transport used by the existing driver.
add_library(gti_project_services STATIC
  "src/driver/analysis_configuration.cpp"
  "src/driver/native_toolchain.cpp"
  "src/driver/artifact.cpp"
  "src/driver/dependencies.cpp"
  "src/driver/package_archive.cpp"
  "src/driver/registry_metadata.cpp"
  "src/driver/registry_gitea.cpp"
  "src/driver/registry_credentials.cpp"
  "src/driver/registry_cache.cpp"
  "src/driver/registry_resolver.cpp"
  "src/driver/semver.cpp"
  "src/driver/manifest.cpp"
  "src/driver/module_discovery.cpp"
  "src/driver/process.cpp"
  "src/driver/project_facts.cpp"
  "src/driver/workspace.cpp"
)
target_include_directories(gti_project_services PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(gti_project_services PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
target_include_directories(gti_project_services SYSTEM PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tomlplusplus" ${GTI_LLVM_INCLUDE_DIRS})
target_compile_features(gti_project_services PUBLIC cxx_std_20)
target_link_libraries(gti_project_services PUBLIC gti_compiler Threads::Threads PRIVATE gti_runtime)
target_compile_definitions(gti_project_services PRIVATE
  GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
  GTI_BUILD_RUNTIME_INCLUDE_DIR="${GTI_BUILD_RUNTIME_INCLUDE_DIR}"
  GTI_BUILD_RUNTIME_LIBRARY_PATH="${GTI_BUILD_RUNTIME_LIBRARY_PATH}"
  GTI_RUNTIME_LIBRARY_NAME="$<TARGET_FILE_NAME:gti_runtime>")
if(APPLE)
  enable_language(OBJCXX)
  set_target_properties(gti_project_services PROPERTIES OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
  target_sources(gti_project_services PRIVATE "src/driver/registry_credentials_macos.mm")
  set_source_files_properties("src/driver/registry_credentials_macos.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  # System framework names remain relocatable in the installed static SDK.
  target_link_libraries(gti_project_services PRIVATE
    "-framework Security" "-framework Foundation" "-framework LocalAuthentication")
endif()

add_library(
  "gti_driver" STATIC
  "src/driver/library_bundle.cpp"
  "src/driver/compiled_scalar_library.cpp"
  "src/driver/embedding_library.cpp"
  "src/driver/bindgen_import.cpp"
  "src/driver/build_script.cpp"
  "src/driver/build.cpp"
  "src/driver/build_report.cpp"
  "src/driver/compilation.cpp"
  "src/driver/install.cpp"
  "src/driver/registry_commands.cpp"
  "src/driver/test_runner.cpp"
  "src/driver/test_report.cpp"
  "src/driver/project.cpp"
)
target_include_directories(
  "gti_driver"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories(
  "gti_driver" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_include_directories(
  "gti_driver" SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tomlplusplus"
)
target_include_directories("gti_driver" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_include_directories("gti_driver" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
target_compile_features("gti_driver" PUBLIC cxx_std_20)
target_link_libraries(
  "gti_driver"
  PUBLIC gti_project_services
  PRIVATE gti_cpp_backend gti_llvm_backend gti_bindgen gti_runtime
)
add_dependencies("gti_driver" "gti_runtime" "gti_build_identity_refresh")
target_compile_definitions(
  "gti_driver"
  PRIVATE
    GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
    GTI_BUILD_RUNTIME_INCLUDE_DIR="${GTI_BUILD_RUNTIME_INCLUDE_DIR}"
    GTI_BUILD_RUNTIME_LIBRARY_PATH="${GTI_BUILD_RUNTIME_LIBRARY_PATH}"
    GTI_RUNTIME_LIBRARY_NAME="$<TARGET_FILE_NAME:gti_runtime>"
)

add_library(
  "gti_bindgen" STATIC
  "src/bindgen/bindgen.cpp"
  "src/bindgen/binding_planner.cpp"
  "src/bindgen/clang_reader.cpp"
  "src/bindgen/gti_writer.cpp"
  "src/bindgen/native_model.cpp"
)
target_include_directories(
  "gti_bindgen"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
# clang-c headers stay private to the reader translation unit; installed GTI
# headers never require them (ADR 021 keeps ADR 006's header posture).
target_include_directories(
  "gti_bindgen" SYSTEM PRIVATE ${GTI_CLANG_INCLUDE_DIRS}
)
# The installed archive keeps the same single clang-project link surface
# through the imported GTI_LLVM::libclang defined by GTIConfig.cmake.
target_link_libraries(
  "gti_bindgen"
  PRIVATE "$<BUILD_INTERFACE:libclang>" "$<INSTALL_INTERFACE:GTI_LLVM::libclang>"
)
target_compile_features("gti_bindgen" PUBLIC cxx_std_20)

add_library("gti_tree_sitter" MODULE "tree-sitter-gti/src/parser.c")
target_include_directories(
  "gti_tree_sitter" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tree-sitter-gti/src"
)
set_target_properties(
  "gti_tree_sitter"
  PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    PREFIX ""
    OUTPUT_NAME "gti"
    SUFFIX ".so"
)

add_executable("gti" "src/cli/main.cpp" "src/cli/registry_input.cpp" "src/cli/project_presentation.cpp"
                     "src/cli/terminal_style.cpp" "src/cli/build_progress.cpp")
target_link_libraries("gti" PRIVATE gti_driver gti_bindgen Threads::Threads)
target_include_directories("gti" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_compile_definitions(
  "gti"
  PRIVATE
    GTI_VERSION="${PROJECT_VERSION}"
)
# Building the compiler yields the complete `gti_toolchain` install component:
# the editor grammar module ships beside every `gti`, so a tree that built only
# the compiler can still stage the toolchain (`cmake --install --component
# gti_toolchain`), as the installed-toolchain tests do.
add_dependencies("gti" "gti_tree_sitter")

# Build-time producer for the reference site's compiler-owned API manifest.
# It is intentionally not installed with the user-facing toolchain.
add_executable("gti_docgen" EXCLUDE_FROM_ALL
               "src/docgen/main.cpp")
target_link_libraries("gti_docgen" PRIVATE gti_compiler)
target_include_directories("gti_docgen" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_compile_definitions(
  "gti_docgen"
  PRIVATE
    GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
)

if(GTI_BUILD_LSP)
  find_package(Threads REQUIRED)
  add_library("gti_lsp_state" STATIC "src/lsp/frontend_snapshot_state.cpp")
  target_include_directories(
    "gti_lsp_state"
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lsp>"
  )
  target_link_libraries("gti_lsp_state" PUBLIC gti_compiler)
  target_compile_features("gti_lsp_state" PUBLIC cxx_std_20)

  add_executable("gti_lsp" "src/lsp/main.cpp")
  target_include_directories("gti_lsp" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
  target_link_libraries(
    "gti_lsp" PRIVATE gti_project_services gti_lsp_state Threads::Threads
  )
  target_compile_definitions(
    "gti_lsp"
    PRIVATE
      GTI_VERSION="${PROJECT_VERSION}"
      GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
  )
endif()

# Compatibility describes shared source inputs, not the independently replaceable
# protocol adapter. Release CI requires a clean Git source and verifies this ID.
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/lsp_compatibility.py"
  --source "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE GTI_LSP_ID_RESULT OUTPUT_VARIABLE GTI_LSP_COMPATIBILITY
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GTI_LSP_ID_RESULT EQUAL 0)
  set(GTI_LSP_COMPATIBILITY "unavailable")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LSP_COMPATIBILITY.json"
  "{\"schema\":1,\"compatibility\":\"${GTI_LSP_COMPATIBILITY}\"}\n")
execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD
  OUTPUT_VARIABLE GTI_LSP_SOURCE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LSP_SOURCE" "${GTI_LSP_SOURCE}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LSP_COMPATIBILITY.json"
  "${CMAKE_CURRENT_BINARY_DIR}/LSP_SOURCE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/gti" COMPONENT gti_toolchain)
# The LSP uses LLVM's JSON and thread support. Ship their pinned headers with
# the SDK so rebuilding the adapter never fetches or rebuilds LLVM/Clang.
foreach(llvm_include IN LISTS GTI_LLVM_INCLUDE_DIRS)
  foreach(llvm_header_root IN ITEMS llvm llvm-c)
    if(EXISTS "${llvm_include}/${llvm_header_root}")
      install(DIRECTORY "${llvm_include}/${llvm_header_root}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gti-llvm"
        COMPONENT gti_toolchain FILES_MATCHING PATTERN "*.h" PATTERN "*.inc"
        PATTERN "CMakeFiles" EXCLUDE)
    endif()
  endforeach()
endforeach()

if(GTI_RELEASE_BUILD AND NOT TARGET gti_lsp)
  message(FATAL_ERROR "A release build requires the gti_lsp target")
endif()

install(TARGETS gti RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT gti_toolchain)
set_target_properties(gti_compiler PROPERTIES EXPORT_NAME compiler)
set_target_properties(gti_cpp_backend PROPERTIES EXPORT_NAME cpp_backend)
set_target_properties(gti_llvm_backend PROPERTIES EXPORT_NAME llvm_backend)
set_target_properties(gti_driver PROPERTIES EXPORT_NAME driver)
set_target_properties(gti_project_services PROPERTIES EXPORT_NAME project_services)
set_target_properties(gti_bindgen PROPERTIES EXPORT_NAME bindgen)
set_target_properties(gti_runtime PROPERTIES EXPORT_NAME runtime)
install(
  TARGETS gti_compiler gti_cpp_backend gti_llvm_backend gti_driver gti_project_services gti_bindgen
          gti_runtime
  EXPORT GTITargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  COMPONENT gti_toolchain
)
set(GTI_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/GTI")
install(
  EXPORT GTITargets
  FILE GTITargets.cmake
  NAMESPACE GTI::
  DESTINATION "${GTI_INSTALL_CMAKEDIR}"
  COMPONENT gti_toolchain
)

set(GTI_PACKAGE_BUNDLED_LLVM "${GTI_BUNDLE_LLVM}")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GTIConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/GTIConfig.cmake"
  INSTALL_DESTINATION "${GTI_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/GTIConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY ExactVersion
)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/GTIConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/GTIConfigVersion.cmake"
  DESTINATION "${GTI_INSTALL_CMAKEDIR}"
  COMPONENT gti_toolchain
)

if(GTI_BUNDLE_LLVM)
  set(GTI_LLVM_ARCHIVE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/gti/llvm")
  install(
    FILES "$<TARGET_FILE:LLVMSupport>"
          "$<TARGET_FILE:LLVMTargetParser>"
          "$<TARGET_FILE:LLVMDemangle>"
    DESTINATION "${GTI_LLVM_ARCHIVE_INSTALL_DIR}"
    COMPONENT gti_toolchain
  )
  foreach(component IN LISTS GTI_LLVM_BACKEND_ARCHIVE_TARGETS)
    install(
      FILES "$<TARGET_FILE:${component}>"
      DESTINATION "${GTI_LLVM_ARCHIVE_INSTALL_DIR}"
      COMPONENT gti_toolchain
    )
  endforeach()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/llvm-backend-archives.txt" "")
  foreach(component IN LISTS GTI_LLVM_BACKEND_ARCHIVE_TARGETS)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/llvm-backend-archives.txt"
         "${CMAKE_STATIC_LIBRARY_PREFIX}${component}${CMAKE_STATIC_LIBRARY_SUFFIX}\n")
  endforeach()
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/llvm-backend-archives.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/gti"
    COMPONENT gti_toolchain
  )
  # libclang is deliberately shared — the C API's supported distribution form
  # — and ships beside the LLVM archives. Every installed executable that
  # reaches the bindgen reader locates it by rpath relative to its own
  # location. The independent LSP has no bindgen dependency.
  install(
    TARGETS libclang
    LIBRARY DESTINATION "${GTI_LLVM_ARCHIVE_INSTALL_DIR}"
            COMPONENT gti_toolchain
            NAMELINK_COMPONENT gti_toolchain
  )
  if(APPLE)
    set(GTI_LLVM_ARCHIVE_INSTALL_RPATH
        "@loader_path/../${GTI_LLVM_ARCHIVE_INSTALL_DIR}")
  else()
    set(GTI_LLVM_ARCHIVE_INSTALL_RPATH
        "$ORIGIN/../${GTI_LLVM_ARCHIVE_INSTALL_DIR}")
  endif()
  foreach(installed_executable IN ITEMS gti gti_lsp)
    if(TARGET "${installed_executable}")
      set_target_properties(
        "${installed_executable}"
        PROPERTIES INSTALL_RPATH "${GTI_LLVM_ARCHIVE_INSTALL_RPATH}")
    endif()
  endforeach()
endif()
install(TARGETS gti_tree_sitter
        LIBRARY DESTINATION "${CMAKE_INSTALL_DATADIR}/gti/parser"
        COMPONENT gti_toolchain)
install(DIRECTORY "include/gti" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT gti_toolchain)
install(DIRECTORY "runtime/include/gti" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT gti_toolchain)
install(FILES "stdlib/prelude.gti" "stdlib/std.gti" "stdlib/module-map.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/gti/stdlib"
        COMPONENT gti_toolchain)
install(DIRECTORY "stdlib/std"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/gti/stdlib"
        COMPONENT gti_toolchain
        FILES_MATCHING PATTERN "*.gti")
install(FILES "VERSION" DESTINATION "${CMAKE_INSTALL_DATADIR}/gti"
        COMPONENT gti_toolchain)
install(FILES "vendor/tomlplusplus/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/gti"
        RENAME "tomlplusplus-LICENSE.txt"
        COMPONENT gti_toolchain)
install(FILES "LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/gti"
        RENAME "GTI-LICENSE.txt"
        COMPONENT gti_toolchain)
if(GTI_BUNDLE_LLVM)
  install(FILES "${llvm_project_SOURCE_DIR}/llvm/LICENSE.TXT"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/gti"
          RENAME "llvm-LICENSE.txt"
          COMPONENT gti_toolchain)
  # Same Apache-2.0-with-LLVM-exception terms; the clang project carries its
  # own license file, and the bundled libclang ships with it (ADR 021).
  install(FILES "${llvm_project_SOURCE_DIR}/clang/LICENSE.TXT"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/gti"
          RENAME "clang-LICENSE.txt"
          COMPONENT gti_toolchain)
endif()
if(TARGET gti_lsp)
  install(TARGETS gti_lsp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
          COMPONENT gti_toolchain)
endif()

if(APPLE)
  foreach(installed_executable IN ITEMS gti gti_lsp)
    if(TARGET "${installed_executable}")
      target_link_options("${installed_executable}" PRIVATE
                          "LINKER:-headerpad_max_install_names")
    endif()
  endforeach()
  if(GTI_BUNDLE_LLVM)
    target_link_options(libclang PRIVATE "LINKER:-headerpad_max_install_names")
  endif()
  # Homebrew opt paths can move to a different ABI between build and install.
  # Use the selected libclang target, resolve its transitive runtime closure,
  # rewrite only staged copies, and sign them before archiving/activation.
  install(CODE "
    execute_process(
      COMMAND \"${Python3_EXECUTABLE}\"
              \"${CMAKE_CURRENT_SOURCE_DIR}/scripts/bundle_macos_runtime.py\"
              --stage \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\"
              --library \"$<TARGET_FILE:libclang>\"
      RESULT_VARIABLE gti_bundle_result)
    if(NOT gti_bundle_result EQUAL 0)
      message(FATAL_ERROR \"Failed to bundle macOS toolchain runtime dependencies\")
    endif()
  " COMPONENT gti_toolchain)
endif()

# BUILD_TESTING and the optimized compiler policy are established above.
if(GTI_BUILD_FUZZERS)
  add_subdirectory("fuzz")
endif()

if(BUILD_TESTING)
  add_subdirectory("tests")
endif()
