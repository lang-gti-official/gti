cmake_minimum_required(VERSION 3.20)
project(gti_native_c_abi_oracle LANGUAGES C CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 20)
get_filename_component(GTI_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT LLVM_DIR)
  find_program(LLVM_CONFIG NAMES llvm-config llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18 REQUIRED)
  execute_process(COMMAND "${LLVM_CONFIG}" --cmakedir OUTPUT_VARIABLE LLVM_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
find_package(LLVM REQUIRED CONFIG)
add_subdirectory("${GTI_SOURCE_ROOT}" gti)
if(TARGET gti_llvm_coverage_evidence)
  target_include_directories(gti_llvm_coverage_evidence PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/gti/generated")
endif()
# Temporary standalone consumer until coordinator-owned registration lands.
get_target_property(backend_sources gti_llvm_backend SOURCES)
if(NOT "src/compiler/llvm/native_c_abi.cpp" IN_LIST backend_sources)
  target_sources(gti_llvm_backend PRIVATE
    "${GTI_SOURCE_ROOT}/src/compiler/llvm/native_c_abi.cpp")
endif()
if(NOT TARGET gti_llvm_native_c_abi_tests)
  add_executable(gti_llvm_native_c_abi_tests
    "${GTI_SOURCE_ROOT}/tests/llvm_native_c_abi_tests.cpp")
  target_include_directories(gti_llvm_native_c_abi_tests SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(gti_llvm_native_c_abi_tests PRIVATE gti_llvm_backend)
endif()
