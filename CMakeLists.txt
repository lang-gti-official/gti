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

option(GTI_BUILD_LSP "Build the GTI language server" ON)
option(GTI_BUNDLE_JSON_C "Build a static json-c for a portable GTI LSP" OFF)
option(GTI_ENABLE_LLVM "Use the LLVM support libraries when available" ON)
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
  set(GTI_BUNDLE_JSON_C ON)
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

# Optional LLVM support libraries (ADR 006). Only LLVMSupport,
# LLVMTargetParser, and LLVMDemangle may ever be linked; llvm/IR, MC, Target,
# and CodeGen are out of scope by policy. LLVM types and llvm/* includes are
# confined to compiled sources: public headers must not require LLVM.
set(GTI_LLVM_FOUND OFF)
if(GTI_BUNDLE_LLVM)
  include(FetchContent)
  set(LLVM_TARGETS_TO_BUILD "" CACHE STRING "GTI links no LLVM backends" FORCE)
  set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Skip LLVM tests" FORCE)
  set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "Skip LLVM examples" FORCE)
  set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Skip LLVM benchmarks" FORCE)
  set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "Skip LLVM docs" FORCE)
  set(LLVM_ENABLE_ZLIB OFF CACHE STRING "No compression dependency" FORCE)
  set(LLVM_ENABLE_ZSTD OFF CACHE STRING "No compression dependency" FORCE)
  set(LLVM_ENABLE_LIBXML2 OFF CACHE STRING "No xml dependency" FORCE)
  set(LLVM_ENABLE_LIBEDIT OFF CACHE STRING "No libedit dependency" FORCE)
  set(LLVM_ENABLE_ASSERTIONS OFF CACHE BOOL "Release LLVM semantics" FORCE)
  FetchContent_Declare(
    llvm_project
    URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.0/llvm-project-20.1.0.src.tar.xz"
    URL_HASH
      SHA256=4579051e3c255fb4bb795d54324f5a7f3ef79bd9181e44293d7ee9a7f62aad9a
  )
  FetchContent_GetProperties(llvm_project)
  if(NOT llvm_project_POPULATED)
    FetchContent_Populate(llvm_project)
    # EXCLUDE_FROM_ALL keeps LLVM's own `all` out of GTI's; only the three
    # libraries GTI links are built, on demand.
    add_subdirectory("${llvm_project_SOURCE_DIR}/llvm"
                     "${llvm_project_BINARY_DIR}" EXCLUDE_FROM_ALL)
  endif()
  add_library(gti_llvm_support INTERFACE)
  target_link_libraries(gti_llvm_support INTERFACE LLVMSupport
                                                   LLVMTargetParser
                                                   LLVMDemangle)
  target_include_directories(
    gti_llvm_support SYSTEM
    INTERFACE "${llvm_project_SOURCE_DIR}/llvm/include"
              "${llvm_project_BINARY_DIR}/include")
  target_compile_definitions(gti_llvm_support INTERFACE GTI_HAS_LLVM=1)
  set(GTI_LLVM_FOUND ON)
  message(STATUS "GTI: bundled LLVM 20.1.0 support libraries enabled")
elseif(GTI_ENABLE_LLVM)
  find_package(LLVM CONFIG QUIET)
  if(LLVM_FOUND
     AND LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL 17
     AND LLVM_PACKAGE_VERSION VERSION_LESS 21)
    add_library(gti_llvm_support INTERFACE)
    # LLVM's exported flags (for example -fno-rtti) are deliberately not
    # imported; only include paths, the three libraries, and GTI's own macro.
    target_link_libraries(gti_llvm_support INTERFACE LLVMSupport
                                                     LLVMTargetParser
                                                     LLVMDemangle)
    target_include_directories(gti_llvm_support SYSTEM
                               INTERFACE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(gti_llvm_support INTERFACE GTI_HAS_LLVM=1)
    set(GTI_LLVM_FOUND ON)
    message(
      STATUS "GTI: LLVM ${LLVM_PACKAGE_VERSION} support libraries enabled")
  else()
    message(
      STATUS
        "GTI: LLVM support libraries not found; LLVM-backed features disabled")
  endif()
endif()

add_library(
  "gti_compiler" STATIC
  "src/compiler/checked_integer.cpp"
  "src/compiler/format_config.cpp"
  "src/compiler/lexer.cpp"
  "src/compiler/mir.cpp"
  "src/compiler/mir_printer.cpp"
  "src/compiler/optimization/effects.cpp"
  "src/compiler/optimizer.cpp"
  "src/compiler/support.cpp"
  "src/compiler/target.cpp"
)
if(GTI_LLVM_FOUND)
  target_link_libraries("gti_compiler" PRIVATE gti_llvm_support)
endif()
target_include_directories(
  "gti_compiler"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features("gti_compiler" PUBLIC cxx_std_20)

add_library("gti_runtime" STATIC "runtime/src/io.cpp")
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
target_link_libraries("gti_driver" PUBLIC gti_compiler)
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
  if(GTI_BUNDLE_JSON_C)
    enable_language(C)
    include(FetchContent)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static release dependencies" FORCE)
    set(BUILD_STATIC_LIBS OFF CACHE BOOL "Avoid duplicate json-c libraries" FORCE)
    set(BUILD_APPS OFF CACHE BOOL "Do not build json-c applications" FORCE)
    FetchContent_Declare(
      json_c
      URL "https://s3.amazonaws.com/json-c_releases/releases/json-c-0.19.tar.gz"
      URL_HASH
        "SHA256=37ad0249902e301bd9052bf712e511fcc6acff4ecaad4b5900aad9ce564e26de"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(json_c)
    set(GTI_JSON_C_TARGET json-c)
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(JSON_C QUIET IMPORTED_TARGET json-c)
    endif()
    if(TARGET PkgConfig::JSON_C)
      set(GTI_JSON_C_TARGET PkgConfig::JSON_C)
    endif()
  endif()

  if(GTI_JSON_C_TARGET)
    find_package(Threads REQUIRED)
    add_executable("gti_lsp" "src/lsp/main.cpp")
    target_link_libraries("gti_lsp" PRIVATE gti_compiler ${GTI_JSON_C_TARGET}
                                            Threads::Threads)
    if(GTI_BUNDLE_JSON_C)
      target_compile_definitions("gti_lsp" PRIVATE GTI_BUNDLED_JSON_C=1)
    endif()
    target_compile_definitions(
      "gti_lsp"
      PRIVATE
        GTI_VERSION="${PROJECT_VERSION}"
        GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}"
    )
  else()
    message(WARNING "json-c was not found; the gti_lsp target will not be built")
  endif()
endif()

if(GTI_RELEASE_BUILD AND NOT TARGET gti_lsp)
  message(FATAL_ERROR "A release build requires the gti_lsp target")
endif()

install(TARGETS gti RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT gti_toolchain)
install(TARGETS gti_compiler ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT gti_toolchain)
install(TARGETS gti_driver ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT gti_toolchain)
install(TARGETS gti_runtime ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT gti_toolchain)
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
if(GTI_BUNDLE_JSON_C)
  install(FILES "${json_c_SOURCE_DIR}/COPYING"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/gti"
          RENAME "json-c-LICENSE.txt"
          COMPONENT gti_toolchain)
endif()
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

  add_executable("gti_compiler_library_smoke"
                 "tests/compiler_library_smoke.cpp")
  target_link_libraries("gti_compiler_library_smoke" PRIVATE gti_compiler)
  add_test(NAME compiler_library_boundary COMMAND gti_compiler_library_smoke)

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
  target_link_libraries("gti_tests" PRIVATE gti_compiler)
  add_test(NAME compiler_pipeline COMMAND gti_tests)

  add_executable("gti_optimizer_tests" "tests/optimizer_tests.cpp")
  target_link_libraries("gti_optimizer_tests" PRIVATE gti_compiler)
  add_test(NAME optimizer_foundation COMMAND gti_optimizer_tests)

  add_executable("gti_raw_pointer_tests" "tests/raw_pointer_tests.cpp")
  target_link_libraries("gti_raw_pointer_tests" PRIVATE gti_compiler)
  add_test(NAME raw_pointer_pipeline COMMAND gti_raw_pointer_tests)

  if(Python3_Interpreter_FOUND)
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
    if(GTI_LLVM_FOUND)
      add_test(
        NAME llvm_link_surface
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_llvm_link_surface.py"
                "${CMAKE_BINARY_DIR}"
      )
    endif()
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
    if(GTI_RELEASE_BUILD)
      set_tests_properties(
        cli_workflow project_cli_workflow
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
      if(GTI_RELEASE_BUILD)
        set_tests_properties(
          lsp_protocol
          PROPERTIES ENVIRONMENT
            "GTI_STDLIB_PATH=${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
        )
      endif()
    endif()
  endif()
endif()
