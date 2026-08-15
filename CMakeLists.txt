cmake_minimum_required(VERSION 3.20)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                       "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GTI_PROJECT_VERSION
     LIMIT_COUNT 1)
if(NOT GTI_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version such as 0.1.0")
endif()

project("gti" VERSION "${GTI_PROJECT_VERSION}" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(GTI_BUILD_LSP "Build the GTI language server" ON)
option(GTI_BUNDLE_LLVM "Download and build pinned LLVM support libraries" OFF)
option(GTI_RELEASE_BUILD "Require a self-contained, release-ready toolchain" OFF)

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

add_library(
  "gti_compiler" STATIC
  "src/compiler/ast_printer.cpp"
  "src/compiler/binary_float.cpp"
  "src/compiler/checked_integer.cpp"
  "src/compiler/constant_evaluator.cpp"
  "src/compiler/diagnostic.cpp"
  "src/compiler/executable_path.cpp"
  "src/compiler/failure_metadata.cpp"
  "src/compiler/format_config.cpp"
  "src/compiler/formatter.cpp"
  "src/compiler/frontend.cpp"
  "src/compiler/hir.cpp"
  "src/compiler/hir_model.cpp"
  "src/compiler/hir_lowering.cpp"
  "src/compiler/language_queries.cpp"
  "src/compiler/lexer.cpp"
  "src/compiler/mir.cpp"
  "src/compiler/mir_dominance.cpp"
  "src/compiler/mir_lowering.cpp"
  "src/compiler/mir_printer.cpp"
  "src/compiler/mir_backend.cpp"
  "src/compiler/native_header.cpp"
  "src/compiler/optimization/effects.cpp"
  "src/compiler/optimization/rewrite.cpp"
  "src/compiler/optimizer.cpp"
  "src/compiler/parser.cpp"
  "src/compiler/semantic_analyzer.cpp"
  "src/compiler/semantic_model.cpp"
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
  "src/compiler/cpp_mir_body_emitter.cpp"
  "src/compiler/cpp_mir_program_plan.cpp"
  "src/compiler/cpp_mir_representation_snapshot.cpp"
)
target_include_directories(
  "gti_cpp_backend"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features("gti_cpp_backend" PUBLIC cxx_std_20)
target_link_libraries("gti_cpp_backend" PUBLIC gti_compiler)

add_library(
  "gti_runtime" STATIC
  "runtime/src/failure.cpp"
  "runtime/src/io.cpp"
)
target_include_directories(
  "gti_runtime"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features("gti_runtime" PUBLIC cxx_std_20)

add_library(
  "gti_driver" STATIC
  "src/driver/artifact.cpp"
  "src/driver/build.cpp"
  "src/driver/compilation.cpp"
  "src/driver/manifest.cpp"
  "src/driver/native_toolchain.cpp"
  "src/driver/process.cpp"
  "src/driver/project.cpp"
  "src/driver/workspace.cpp"
)
target_include_directories(
  "gti_driver"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_include_directories(
  "gti_driver" SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tomlplusplus"
)
target_compile_features("gti_driver" PUBLIC cxx_std_20)
target_link_libraries("gti_driver" PUBLIC gti_compiler PRIVATE gti_cpp_backend)
add_dependencies("gti_driver" "gti_runtime")
target_compile_definitions(
  "gti_driver"
  PRIVATE
    GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
    GTI_BUILD_RUNTIME_INCLUDE_DIR="${GTI_BUILD_RUNTIME_INCLUDE_DIR}"
    GTI_BUILD_RUNTIME_LIBRARY_PATH="${GTI_BUILD_RUNTIME_LIBRARY_PATH}"
    GTI_RUNTIME_LIBRARY_NAME="$<TARGET_FILE_NAME:gti_runtime>"
    GTI_BUILD_VENDOR_INCLUDE_DIR="${GTI_BUILD_VENDOR_INCLUDE_DIR}"
)

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
target_link_libraries("gti" PRIVATE gti_driver)
target_compile_definitions(
  "gti"
  PRIVATE
    GTI_VERSION="${PROJECT_VERSION}"
)

if(GTI_BUILD_LSP)
  find_package(Threads REQUIRED)
  add_executable("gti_lsp" "src/lsp/main.cpp")
  target_include_directories("gti_lsp" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
  target_link_libraries("gti_lsp" PRIVATE gti_compiler Threads::Threads)
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
set_target_properties(gti_runtime PROPERTIES EXPORT_NAME runtime)
install(
  TARGETS gti_compiler gti_cpp_backend gti_driver gti_runtime
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
endif()
if(TARGET gti_lsp)
  install(TARGETS gti_lsp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
          COMPONENT gti_toolchain)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable("gti_c_abi_smoke" "tests/c_abi_smoke.c")
  target_include_directories(
    "gti_c_abi_smoke"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include"
  )
  target_compile_features("gti_c_abi_smoke" PRIVATE c_std_11)
  add_test(NAME c_abi_header_boundary COMMAND gti_c_abi_smoke)

  add_executable("gti_runtime_failure_tests"
                 "tests/runtime_failure_tests.cpp")
  target_include_directories(
    "gti_runtime_failure_tests"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src"
  )
  target_compile_features("gti_runtime_failure_tests" PRIVATE cxx_std_23)
  target_link_libraries("gti_runtime_failure_tests" PRIVATE gti_runtime)
  add_test(NAME runtime_failure_boundary COMMAND gti_runtime_failure_tests)

  add_executable("gti_runtime_failure_header_smoke"
                 "tests/runtime_failure_header_smoke.cpp")
  target_compile_features("gti_runtime_failure_header_smoke" PRIVATE cxx_std_20)
  target_link_libraries("gti_runtime_failure_header_smoke"
                        PRIVATE gti_compiler gti_runtime)
  add_test(NAME runtime_failure_header_coexistence
           COMMAND gti_runtime_failure_header_smoke)

  find_package(Threads REQUIRED)
  add_executable("gti_runtime_failure_helper"
                 "tests/runtime_failure_helper.cpp")
  target_link_libraries("gti_runtime_failure_helper"
                        PRIVATE gti_runtime Threads::Threads)

  add_executable("gti_compiler_library_smoke"
                 "tests/compiler_library_smoke.cpp")
  target_link_libraries("gti_compiler_library_smoke" PRIVATE gti_compiler)
  add_test(NAME compiler_library_boundary COMMAND gti_compiler_library_smoke)

  add_executable("gti_cpp_backend_library_smoke"
                 "tests/cpp_backend_library_smoke.cpp")
  target_link_libraries("gti_cpp_backend_library_smoke"
                        PRIVATE gti_cpp_backend)
  add_test(NAME cpp_backend_library_boundary
           COMMAND gti_cpp_backend_library_smoke)

  add_executable("gti_mir_differential_helper"
                 "tests/mir_differential_helper.cpp")
  target_link_libraries("gti_mir_differential_helper"
                        PRIVATE gti_cpp_backend gti_compiler)

  add_executable("gti_driver_library_smoke" "tests/driver_library_smoke.cpp")
  target_link_libraries("gti_driver_library_smoke" PRIVATE gti_driver)
  add_test(NAME driver_library_boundary COMMAND gti_driver_library_smoke)

  add_executable("gti_driver_tests" "tests/driver_tests.cpp")
  target_link_libraries("gti_driver_tests" PRIVATE gti_driver)
  add_test(NAME driver_pipeline COMMAND gti_driver_tests)

  add_executable("gti_project_tests" "tests/project_tests.cpp")
  target_link_libraries("gti_project_tests" PRIVATE gti_driver)
  add_test(NAME project_model COMMAND gti_project_tests)

  add_executable("gti_tests" "tests/compiler_tests.cpp")
  target_link_libraries("gti_tests" PRIVATE gti_cpp_backend)
  add_test(NAME compiler_pipeline COMMAND gti_tests)

  add_executable("gti_block_comment_tests" "tests/block_comment_tests.cpp")
  target_link_libraries("gti_block_comment_tests" PRIVATE gti_compiler)
  add_test(NAME block_comment_pipeline COMMAND gti_block_comment_tests)

  add_executable("gti_pack_fold_syntax_tests"
                 "tests/pack_fold_syntax_tests.cpp")
  target_link_libraries("gti_pack_fold_syntax_tests" PRIVATE gti_compiler)
  add_test(NAME pack_fold_syntax COMMAND gti_pack_fold_syntax_tests)

  add_executable("gti_optimizer_tests" "tests/optimizer_tests.cpp")
  target_link_libraries("gti_optimizer_tests" PRIVATE gti_compiler)
  add_test(NAME optimizer_foundation COMMAND gti_optimizer_tests)

  add_executable("gti_failure_metadata_tests"
                 "tests/failure_metadata_tests.cpp")
  target_link_libraries("gti_failure_metadata_tests" PRIVATE gti_compiler)
  add_test(NAME failure_metadata COMMAND gti_failure_metadata_tests)

  add_executable("gti_mir_program_initialization_tests"
                 "tests/mir_program_initialization_tests.cpp")
  target_link_libraries("gti_mir_program_initialization_tests"
                        PRIVATE gti_compiler)
  add_test(NAME mir_program_initialization
           COMMAND gti_mir_program_initialization_tests)

  add_executable("gti_mir_hosted_startup_tests"
                 "tests/mir_hosted_startup_tests.cpp")
  target_link_libraries("gti_mir_hosted_startup_tests" PRIVATE gti_compiler)
  add_test(NAME mir_hosted_startup COMMAND gti_mir_hosted_startup_tests)

  add_executable("gti_raw_pointer_tests" "tests/raw_pointer_tests.cpp")
  target_link_libraries("gti_raw_pointer_tests" PRIVATE gti_cpp_backend)
  add_test(NAME raw_pointer_pipeline COMMAND gti_raw_pointer_tests)

  add_executable("gti_layout_query_tests" "tests/layout_query_tests.cpp")
  target_link_libraries("gti_layout_query_tests" PRIVATE gti_cpp_backend)
  add_test(NAME layout_query_pipeline COMMAND gti_layout_query_tests)

  add_executable("gti_native_record_tests" "tests/native_record_tests.cpp")
  target_link_libraries("gti_native_record_tests" PRIVATE gti_cpp_backend)
  add_test(NAME native_record_pipeline COMMAND gti_native_record_tests)

  add_executable("gti_native_header_tests" "tests/native_header_tests.cpp")
  target_link_libraries("gti_native_header_tests" PRIVATE gti_cpp_backend)
  add_test(NAME native_header_pipeline COMMAND gti_native_header_tests)

  add_executable("gti_defined_integer_tests"
                 "tests/defined_integer_tests.cpp")
  target_link_libraries("gti_defined_integer_tests" PRIVATE gti_cpp_backend)
  add_test(NAME defined_integer_arithmetic COMMAND gti_defined_integer_tests)

  add_executable("gti_binary64_tests" "tests/binary64_tests.cpp")
  target_link_libraries("gti_binary64_tests" PRIVATE gti_cpp_backend)
  add_test(NAME binary64_pipeline COMMAND gti_binary64_tests)

  add_executable("gti_sum_type_tests" "tests/sum_type_tests.cpp")
  target_link_libraries("gti_sum_type_tests" PRIVATE gti_cpp_backend)
  add_test(NAME sum_type_pipeline COMMAND gti_sum_type_tests)

  add_executable("gti_mir_backend_tests" "tests/mir_backend_tests.cpp")
  target_link_libraries("gti_mir_backend_tests" PRIVATE gti_cpp_backend)
  add_test(NAME mir_backend_first_family COMMAND gti_mir_backend_tests)

  add_executable("gti_cpp_mir_program_plan_tests"
                 "tests/cpp_mir_program_plan_tests.cpp")
  target_link_libraries("gti_cpp_mir_program_plan_tests"
                        PRIVATE gti_cpp_backend)
  add_test(NAME cpp_mir_program_plan COMMAND gti_cpp_mir_program_plan_tests)

  add_executable("gti_cpp_mir_body_emitter_tests"
                 "tests/cpp_mir_body_emitter_tests.cpp")
  target_link_libraries("gti_cpp_mir_body_emitter_tests"
                        PRIVATE gti_cpp_backend)
  add_test(NAME cpp_mir_body_emitter COMMAND gti_cpp_mir_body_emitter_tests)

  add_executable(
    "gti_cpp_mir_representation_snapshot_tests"
    "tests/cpp_mir_representation_snapshot_tests.cpp"
  )
  target_link_libraries("gti_cpp_mir_representation_snapshot_tests"
                        PRIVATE gti_cpp_backend)
  add_test(NAME cpp_mir_representation_snapshot
           COMMAND gti_cpp_mir_representation_snapshot_tests)

  add_executable("gti_failure_descriptor_backend_tests"
                 "tests/failure_descriptor_backend_tests.cpp")
  target_link_libraries("gti_failure_descriptor_backend_tests"
                        PRIVATE gti_cpp_backend)
  add_test(
    NAME failure_descriptor_backend
    COMMAND gti_failure_descriptor_backend_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/failure_descriptor_backend.gti"
  )

  add_executable("gti_mir_backend_scalar_cfg_tests"
                 "tests/mir_backend_scalar_cfg_tests.cpp")
  target_link_libraries("gti_mir_backend_scalar_cfg_tests"
                        PRIVATE gti_cpp_backend)
  add_test(
    NAME mir_backend_scalar_cfg
    COMMAND gti_mir_backend_scalar_cfg_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_cfg.gti"
  )

  add_executable("gti_mir_backend_scalar_direct_call_tests"
                 "tests/mir_backend_scalar_direct_call_tests.cpp")
  target_link_libraries("gti_mir_backend_scalar_direct_call_tests"
                        PRIVATE gti_cpp_backend)
  add_test(
    NAME mir_backend_scalar_direct_call
    COMMAND gti_mir_backend_scalar_direct_call_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_direct_call.gti"
  )

  add_executable("gti_mir_backend_class_default_cleanup_tests"
                 "tests/mir_backend_class_default_cleanup_tests.cpp")
  target_link_libraries("gti_mir_backend_class_default_cleanup_tests"
                        PRIVATE gti_cpp_backend)
  add_test(
    NAME mir_backend_class_default_cleanup
    COMMAND gti_mir_backend_class_default_cleanup_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_class_default_cleanup.gti"
  )

  add_executable("gti_mir_backend_owned_lifecycle_tests"
                 "tests/mir_backend_owned_lifecycle_tests.cpp")
  target_link_libraries("gti_mir_backend_owned_lifecycle_tests"
                        PRIVATE gti_cpp_backend)
  add_test(
    NAME mir_backend_owned_lifecycle
    COMMAND gti_mir_backend_owned_lifecycle_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_owned_lifecycle.gti"
  )

  add_executable("gti_mir_backend_scalar_failure_callgraph_tests"
                 "tests/mir_backend_scalar_failure_callgraph_tests.cpp")
  target_link_libraries("gti_mir_backend_scalar_failure_callgraph_tests"
                        PRIVATE gti_cpp_backend)
  add_test(
    NAME mir_backend_scalar_failure_callgraph
    COMMAND gti_mir_backend_scalar_failure_callgraph_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_failure.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_reverse_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_recursive_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_cleanup_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_checked_ctor_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_checked_dtor_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_virtual_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_lambda_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_dynamic_global_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_failure_free_helper_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_c_linkage_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_class_field_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_lifecycle_user_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_array_user_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_generic_owner_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_no_detector_entry_near_miss.gti"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_nonowning_user.gti"
  )

  add_executable("gti_layout_query_native_smoke"
                 "tests/layout_query_native_smoke.cpp")
  target_link_libraries("gti_layout_query_native_smoke" PRIVATE gti_compiler)
  add_test(NAME layout_query_native_boundary
           COMMAND gti_layout_query_native_smoke)

  if(Python3_Interpreter_FOUND)
    add_test(
      NAME runtime_failure_subprocess
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_failure_test.py"
              "$<TARGET_FILE:gti_runtime_failure_helper>"
    )
    add_test(
      NAME release_version_policy
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_version_test.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_release_version.py"
    )
    add_test(
      NAME output_determinism
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/determinism_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/examples/07-generics.gti"
    )
    add_test(
      NAME defined_integer_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/defined_integer_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/examples/46-defined-integer-arithmetic.gti"
    )
    add_test(
      NAME binary64_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/binary64_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/examples/47-binary64.gti"
    )
    add_test(
      NAME sum_types_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/sum_types_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/examples/56-sum-types.gti"
    )
    file(GLOB GTI_DIFFERENTIAL_SOURCES
         "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.gti")
    add_test(
      NAME mir_differential_oracle
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/mir_differential_test.py"
              "$<TARGET_FILE:gti_mir_differential_helper>"
              "${CMAKE_CXX_COMPILER}"
              "${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
              "$<TARGET_FILE:gti_runtime>"
              ${GTI_DIFFERENTIAL_SOURCES}
    )
    add_test(
      NAME mir_backend_first_family_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/mir_backend_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_first_family.gti"
    )
    add_test(
      NAME failure_descriptor_backend_compile
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/failure_descriptor_backend_compile_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/failure_descriptor_backend.gti"
              "${CMAKE_CXX_COMPILER}"
              "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
      NAME mir_backend_scalar_cfg_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/mir_backend_scalar_cfg_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_cfg.gti"
    )
    add_test(
      NAME mir_backend_scalar_direct_call_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/mir_backend_scalar_direct_call_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_direct_call.gti"
    )
    add_test(
      NAME mir_backend_class_default_cleanup_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/mir_backend_class_default_cleanup_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_class_default_cleanup.gti"
    )
    set_tests_properties(mir_backend_class_default_cleanup_runtime
                         PROPERTIES TIMEOUT 180)
    add_test(
      NAME mir_backend_owned_lifecycle_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/mir_backend_owned_lifecycle_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_owned_lifecycle.gti"
    )
    set_tests_properties(mir_backend_owned_lifecycle_runtime
                         PROPERTIES TIMEOUT 180)
    add_test(
      NAME mir_backend_scalar_failure_callgraph_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/mir_backend_scalar_failure_callgraph_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_success.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_all_operations_success.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_failure.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_division_zero.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_division_overflow.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_negative_shift.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_wide_shift.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_subtraction_overflow.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_multiplication_overflow.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_remainder_zero.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_negative_right_shift.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_wide_right_shift.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_negation_overflow.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_conversion_wide.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_conversion_negative.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_unsigned_addition_overflow.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_unsigned_subtraction_overflow.gti"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_unsigned_multiplication_overflow.gti"
    )
    set_tests_properties(mir_backend_scalar_failure_callgraph_runtime
                         PROPERTIES TIMEOUT 300)
    add_test(
      NAME native_record_c_oracle
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_record_c_oracle_test.py"
              "$<TARGET_FILE:gti>" "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set_tests_properties(native_record_c_oracle PROPERTIES TIMEOUT 180)
    add_test(
      NAME native_header_c_cpp_oracle
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_header_c_cpp_oracle_test.py"
              "$<TARGET_FILE:gti>" "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set_tests_properties(native_header_c_cpp_oracle PROPERTIES TIMEOUT 180)
    add_test(
      NAME llvm_link_surface
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_llvm_link_surface.py"
              "${CMAKE_BINARY_DIR}"
    )
    add_test(
      NAME llvm_link_surface_policy
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/llvm_link_surface_test.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_llvm_link_surface.py"
    )
    add_test(
      NAME cli_workflow
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke_test.py"
              "$<TARGET_FILE:gti>"
    )
    add_test(
      NAME project_cli_workflow
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/project_cli_smoke_test.py"
              "$<TARGET_FILE:gti>"
    )
    add_test(
      NAME benchmark_harness
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_compare_test.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/benchmark_compare.py"
              "$<TARGET_FILE:gti>"
              "${CMAKE_CXX_COMPILER}"
    )
    set_tests_properties(
      benchmark_harness
      PROPERTIES TIMEOUT 300
                 ENVIRONMENT
                   "GTI_STDLIB_PATH=${CMAKE_CURRENT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/vendor/expected_lite/include"
    )
    if(GTI_RELEASE_BUILD)
      # Release binaries deliberately omit source-tree fallback paths so the
      # installed-toolchain checks exercise relocatable discovery. Build-tree
      # tests still run before installation and therefore need an explicit
      # test-only resource layout.
      set_tests_properties(
        driver_pipeline
        output_determinism defined_integer_runtime binary64_runtime sum_types_runtime
        native_record_c_oracle native_header_c_cpp_oracle
        cli_workflow project_cli_workflow
        mir_backend_first_family_runtime failure_descriptor_backend_compile
        mir_backend_scalar_cfg_runtime mir_backend_scalar_direct_call_runtime
        mir_backend_class_default_cleanup_runtime
        mir_backend_owned_lifecycle_runtime
        mir_backend_scalar_failure_callgraph_runtime
        PROPERTIES ENVIRONMENT
          "GTI_STDLIB_PATH=${CMAKE_CURRENT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/vendor/expected_lite/include"
      )
    endif()

    if(TARGET gti_lsp)
      add_test(
        NAME lsp_protocol
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_smoke_test.py"
                "$<TARGET_FILE:gti_lsp>"
      )
      add_test(
        NAME block_comment_lsp
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/block_comment_lsp_test.py"
                "$<TARGET_FILE:gti_lsp>"
      )
      if(GTI_RELEASE_BUILD)
        set_tests_properties(
          lsp_protocol block_comment_lsp
          PROPERTIES ENVIRONMENT
            "GTI_STDLIB_PATH=${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
        )
      endif()
    endif()
  endif()
endif()
