cmake_minimum_required(VERSION 3.20)
project(GtiLlvmCacheChecks LANGUAGES C CXX)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." gti)
add_executable(gti_llvm_cache_tests ../llvm_cache_tests.cpp)
target_link_libraries(gti_llvm_cache_tests PRIVATE gti_driver)
target_compile_features(gti_llvm_cache_tests PRIVATE cxx_std_20)
target_compile_options(gti_llvm_cache_tests PRIVATE -Wall -Wextra -Werror
                       -Wno-missing-field-initializers)
target_compile_definitions(gti_llvm_cache_tests PRIVATE
  GTI_LLVM_CACHE_FIXTURE="${CMAKE_CURRENT_LIST_DIR}/../fixtures/llvm_cache")
