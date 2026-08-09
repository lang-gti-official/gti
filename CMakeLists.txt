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
option(GTI_RELEASE_BUILD "Require a self-contained, release-ready toolchain" OFF)

if(GTI_RELEASE_BUILD)
  set(GTI_BUNDLE_JSON_C ON)
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

add_library(
  "gti_compiler" STATIC
  "src/compiler/format_config.cpp"
  "src/compiler/lexer.cpp"
  "src/compiler/mir.cpp"
  "src/compiler/mir_printer.cpp"
  "src/compiler/optimization/effects.cpp"
  "src/compiler/optimizer.cpp"
)
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

add_executable("gti" "src/cli/main.cpp")
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
if(TARGET gti_lsp)
  install(TARGETS gti_lsp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
          COMPONENT gti_toolchain)
endif()

include(CTest)
if(BUILD_TESTING)
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

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME release_version_policy
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_version_test.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_release_version.py"
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
