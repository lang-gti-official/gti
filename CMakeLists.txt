cmake_minimum_required(VERSION 3.20)
project("gti" VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

add_library("gti_compiler" INTERFACE)
target_include_directories(
  "gti_compiler"
  INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features("gti_compiler" INTERFACE cxx_std_20)

add_library("gti_runtime" STATIC "runtime/src/io.cpp")
target_include_directories(
  "gti_runtime"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features("gti_runtime" PUBLIC cxx_std_20)

add_executable("gti" "src/cli/main.cpp")
target_link_libraries("gti" PRIVATE gti_compiler)
add_dependencies("gti" "gti_runtime")
target_compile_definitions(
  "gti"
  PRIVATE
    GTI_VERSION="${PROJECT_VERSION}"
    GTI_BUILD_STDLIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/stdlib/prelude.gti"
    GTI_BUILD_RUNTIME_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime/include"
    GTI_BUILD_RUNTIME_LIBRARY_PATH="$<TARGET_FILE:gti_runtime>"
    GTI_RUNTIME_LIBRARY_NAME="$<TARGET_FILE_NAME:gti_runtime>"
)

option(GTI_BUILD_LSP "Build the GTI language server" ON)
if(GTI_BUILD_LSP)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(JSON_C QUIET IMPORTED_TARGET json-c)
  endif()

  if(TARGET PkgConfig::JSON_C)
    add_executable("gti_lsp" "src/lsp/main.cpp")
    target_link_libraries("gti_lsp" PRIVATE gti_compiler PkgConfig::JSON_C)
    target_compile_definitions(
      "gti_lsp"
      PRIVATE GTI_BUILD_STDLIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/stdlib/prelude.gti"
    )
  else()
    message(WARNING "json-c was not found; the gti_lsp target will not be built")
  endif()
endif()

install(
  TARGETS gti gti_runtime
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(DIRECTORY "include/gti" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "runtime/include/gti" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "stdlib/prelude.gti"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/gti/stdlib")
if(TARGET gti_lsp)
  install(TARGETS gti_lsp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable("gti_tests" "tests/compiler_tests.cpp")
  target_link_libraries("gti_tests" PRIVATE gti_compiler)
  add_test(NAME compiler_pipeline COMMAND gti_tests)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME cli_workflow
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke_test.py"
              "$<TARGET_FILE:gti>"
    )

    if(TARGET gti_lsp)
      add_test(
        NAME lsp_protocol
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_smoke_test.py"
                "$<TARGET_FILE:gti_lsp>"
      )
    endif()
  endif()
endif()
