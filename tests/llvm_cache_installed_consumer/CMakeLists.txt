cmake_minimum_required(VERSION 3.20)
project(GtiInstalledLlvmCache LANGUAGES C CXX)

find_package(GTI CONFIG REQUIRED)
add_executable(gti_installed_llvm_cache ../llvm_cache_tests.cpp)
target_link_libraries(gti_installed_llvm_cache PRIVATE GTI::driver)
target_compile_definitions(gti_installed_llvm_cache PRIVATE
  GTI_LLVM_CACHE_FIXTURE="${CMAKE_CURRENT_LIST_DIR}/../fixtures/llvm_cache")
