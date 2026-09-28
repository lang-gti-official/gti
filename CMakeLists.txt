cmake_minimum_required(VERSION 3.20)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                       "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GTI_PROJECT_VERSION
     LIMIT_COUNT 1)
if(NOT GTI_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version such as 0.1.0")
endif()

project("gti" VERSION "${GTI_PROJECT_VERSION}" LANGUAGES C CXX)

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

option(GTI_BUILD_LSP "Build the GTI language server" ON)
option(GTI_BUNDLE_LLVM "Download and build pinned LLVM support libraries" OFF)
option(GTI_RELEASE_BUILD "Require a self-contained, release-ready toolchain" OFF)
option(GTI_BUILD_FUZZERS
       "Build opt-in libFuzzer harnesses (Clang only; never installed)" OFF)

find_package(Python3 COMPONENTS Interpreter QUIET)
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

if(GTI_RELEASE_BUILD)
  set(GTI_BUNDLE_LLVM ON)
endif()

set(GTI_BUILD_STDLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/stdlib")
set(GTI_BUILD_RUNTIME_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
set(GTI_BUILD_RUNTIME_LIBRARY_PATH "$<TARGET_FILE:gti_runtime>")
set(GTI_BUILD_VENDOR_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/expected_lite/include")
if(GTI_RELEASE_BUILD)
  set(GTI_BUILD_STDLIB_ROOT "")
  set(GTI_BUILD_RUNTIME_INCLUDE_DIR "")
  set(GTI_BUILD_RUNTIME_LIBRARY_PATH "")
  set(GTI_BUILD_VENDOR_INCLUDE_DIR "")
endif()

# LLVM is a required compiler implementation dependency. GTI_BUNDLE_LLVM only
# controls how that dependency is acquired: ordinary builds use a supported
# installed LLVM, while release builds fetch the pinned version. Only
# LLVMSupport, LLVMTargetParser, and LLVMDemangle may be linked; llvm/IR, MC,
# Target, and CodeGen remain out of scope. LLVM headers stay private to
# compiled implementation targets and never become a public-header dependency.
set(GTI_PINNED_LLVM_VERSION "20.1.0")
if(GTI_BUNDLE_LLVM)
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static bundled dependencies" FORCE)
  set(LLVM_TARGETS_TO_BUILD "" CACHE STRING "GTI links no LLVM backends" FORCE)
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
  set(GTI_LLVM_SOURCE_URL
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${GTI_PINNED_LLVM_VERSION}/llvm-project-${GTI_PINNED_LLVM_VERSION}.src.tar.xz")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    FetchContent_Declare(
      llvm_project
      URL "${GTI_LLVM_SOURCE_URL}"
      URL_HASH
        SHA256=4579051e3c255fb4bb795d54324f5a7f3ef79bd9181e44293d7ee9a7f62aad9a
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
        SHA256=4579051e3c255fb4bb795d54324f5a7f3ef79bd9181e44293d7ee9a7f62aad9a
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
  find_package(LLVM CONFIG QUIET)
  if(NOT LLVM_FOUND)
    message(
      FATAL_ERROR
        "GTI requires LLVM 18 through 22. Set LLVM_DIR to a supported LLVM CMake package or configure with -DGTI_BUNDLE_LLVM=ON.")
  endif()
  if(LLVM_PACKAGE_VERSION VERSION_LESS 18 OR
     LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL 23)
    message(
      FATAL_ERROR
        "GTI requires LLVM 18 through 22, but found LLVM ${LLVM_PACKAGE_VERSION}. Set LLVM_DIR to a supported installation or configure with -DGTI_BUNDLE_LLVM=ON.")
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
  find_package(Clang CONFIG QUIET HINTS "${LLVM_DIR}/../clang")
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
                        "-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++.")
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
  "src/compiler/semantic_call_resolution.cpp"
  "src/compiler/semantic_declaration_analysis.cpp"
  "src/compiler/semantic_flow_analysis.cpp"
  "src/compiler/semantic_ownership_analysis.cpp"
  "src/compiler/semantic_type_resolution.cpp"
)
target_include_directories(
  "gti_semantic_analysis"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(
  "gti_semantic_analysis" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS}
)
target_compile_features("gti_semantic_analysis" PRIVATE cxx_std_20)
target_precompile_headers(
  "gti_semantic_analysis"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/semantic_analyzer_impl.h"
)

add_library(
  "gti_compiler" STATIC
  $<TARGET_OBJECTS:gti_semantic_analysis>
  "src/compiler/ast_printer.cpp"
  "src/compiler/binary_float.cpp"
  "src/compiler/checked_integer.cpp"
  "src/compiler/configuration_directive_processor.cpp"
  "src/compiler/constant_evaluator.cpp"
  "src/compiler/diagnostic.cpp"
  "src/compiler/documentation.cpp"
  "src/compiler/executable_path.cpp"
  "src/compiler/failure_metadata.cpp"
  "src/compiler/format_config.cpp"
  "src/compiler/formatter.cpp"
  "src/compiler/frontend.cpp"
  "src/compiler/frontend_cache.cpp"
  "src/compiler/hir.cpp"
  "src/compiler/hir_model.cpp"
  "src/compiler/hir_lowering.cpp"
  "src/compiler/language_queries.cpp"
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
  "src/compiler/mir_verification_queries.cpp"
  "src/compiler/mir_backend.cpp"
  "src/compiler/native_header.cpp"
  "src/compiler/optimization/alias.cpp"
  "src/compiler/mir_effects.cpp"
  "src/compiler/optimization/loops.cpp"
  "src/compiler/optimization/pass.cpp"
  "src/compiler/optimization/report.cpp"
  "src/compiler/mir_editor.cpp"
  "src/compiler/optimization/safety.cpp"
  "src/compiler/optimization/sccp.cpp"
  "src/compiler/optimizer.cpp"
  "src/compiler/parser.cpp"
  "src/compiler/semantic_model.cpp"
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
  "gti_runtime" STATIC
  "runtime/src/chrono.cpp"
  "runtime/src/cthread.cpp"
  "runtime/src/cthread_sleep.cpp"
  "runtime/src/cthread_mutex.cpp"
  "runtime/src/cthread_state.cpp"
  "runtime/src/failure.cpp"
  "runtime/src/http.cpp"
  "runtime/src/io.cpp"
  "runtime/src/math.cpp"
  "runtime/src/process.cpp"
  "runtime/src/random.cpp"
  "runtime/src/thread.cpp"
)
target_include_directories(
  "gti_runtime"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories("gti_runtime" SYSTEM PRIVATE ${CURL_INCLUDE_DIRS})
target_compile_features("gti_runtime" PUBLIC cxx_std_20)
target_link_libraries("gti_runtime" PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  # gti_rt_random_bytes calls BCryptGenRandom.
  target_link_libraries("gti_runtime" PRIVATE "bcrypt")
endif()

add_library(
  "gti_driver" STATIC
  "src/driver/artifact.cpp"
  "src/driver/bindgen_import.cpp"
  "src/driver/build_script.cpp"
  "src/driver/build.cpp"
  "src/driver/build_report.cpp"
  "src/driver/compilation.cpp"
  "src/driver/dependencies.cpp"
  "src/driver/manifest.cpp"
  "src/driver/native_toolchain.cpp"
  "src/driver/process.cpp"
  "src/driver/project.cpp"
  "src/driver/project_facts.cpp"
  "src/driver/workspace.cpp"
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
target_compile_features("gti_driver" PUBLIC cxx_std_20)
target_link_libraries("gti_driver" PUBLIC gti_compiler PRIVATE gti_cpp_backend gti_bindgen)
add_dependencies("gti_driver" "gti_runtime" "gti_build_identity_refresh")
target_compile_definitions(
  "gti_driver"
  PRIVATE
    GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
    GTI_BUILD_RUNTIME_INCLUDE_DIR="${GTI_BUILD_RUNTIME_INCLUDE_DIR}"
    GTI_BUILD_RUNTIME_LIBRARY_PATH="${GTI_BUILD_RUNTIME_LIBRARY_PATH}"
    GTI_RUNTIME_LIBRARY_NAME="$<TARGET_FILE_NAME:gti_runtime>"
    GTI_BUILD_VENDOR_INCLUDE_DIR="${GTI_BUILD_VENDOR_INCLUDE_DIR}"
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

add_executable("gti" "src/cli/main.cpp" "src/cli/project_presentation.cpp")
target_link_libraries("gti" PRIVATE gti_driver gti_bindgen)
target_compile_definitions(
  "gti"
  PRIVATE
    GTI_VERSION="${PROJECT_VERSION}"
)

# Build-time producer for the reference site's compiler-owned API manifest.
# It is intentionally not installed with the user-facing toolchain.
add_executable("gti_docgen" EXCLUDE_FROM_ALL
               "reference/tools/docgen_main.cpp")
target_link_libraries("gti_docgen" PRIVATE gti_compiler)
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
    "gti_lsp" PRIVATE gti_driver gti_lsp_state Threads::Threads
  )
  target_compile_definitions(
    "gti_lsp"
    PRIVATE
      GTI_VERSION="${PROJECT_VERSION}"
      GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
  )
endif()

if(GTI_RELEASE_BUILD AND NOT TARGET gti_lsp)
  message(FATAL_ERROR "A release build requires the gti_lsp target")
endif()

install(TARGETS gti RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT gti_toolchain)
set_target_properties(gti_compiler PROPERTIES EXPORT_NAME compiler)
set_target_properties(gti_cpp_backend PROPERTIES EXPORT_NAME cpp_backend)
set_target_properties(gti_driver PROPERTIES EXPORT_NAME driver)
set_target_properties(gti_bindgen PROPERTIES EXPORT_NAME bindgen)
set_target_properties(gti_runtime PROPERTIES EXPORT_NAME runtime)
install(
  TARGETS gti_compiler gti_cpp_backend gti_driver gti_bindgen gti_runtime
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
  # libclang is deliberately shared — the C API's supported distribution form
  # — and ships beside the LLVM archives. Every installed executable that
  # reaches the bindgen reader locates it by rpath relative to its own
  # location, so each one needs the rpath: gti_lsp links the same driver and
  # fails to load at startup without it.
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
install(DIRECTORY "vendor/expected_lite/include/nonstd"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT gti_toolchain)
install(FILES "stdlib/prelude.gti"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/gti/stdlib"
        COMPONENT gti_toolchain)
install(DIRECTORY "stdlib/std"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/gti/stdlib"
        COMPONENT gti_toolchain
        FILES_MATCHING PATTERN "*.gti")
install(FILES "VERSION" DESTINATION "${CMAKE_INSTALL_DATADIR}/gti"
        COMPONENT gti_toolchain)
install(FILES "vendor/expected_lite/LICENSE.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/gti"
        RENAME "expected-lite-LICENSE.txt"
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

include(CTest)
# After include(CTest) so fuzz/CMakeLists.txt sees BUILD_TESTING and can
# register its fixed-corpus smoke test.
if(GTI_BUILD_FUZZERS)
  add_subdirectory("fuzz")
endif()

if(BUILD_TESTING)
  add_subdirectory("tests")
endif()
