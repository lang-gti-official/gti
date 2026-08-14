cmake_minimum_required(VERSION 3.20)

# System LLVM packages may run C feature probes while loading LLVMConfig.cmake
# (Homebrew LLVM 20 checks libedit this way), even though these smoke programs
# are C++. Enable both languages so this project exercises the real downstream
# package contract on every supported system LLVM distribution.
project(gti_installed_library_smoke LANGUAGES C CXX)

include(CTest)
find_package(GTI CONFIG REQUIRED)

add_executable(gti_installed_compiler_smoke ../compiler_library_smoke.cpp)
target_link_libraries(gti_installed_compiler_smoke PRIVATE GTI::compiler)
add_test(NAME installed_compiler_library
         COMMAND gti_installed_compiler_smoke)

add_executable(gti_installed_cpp_backend_smoke
               ../cpp_backend_library_smoke.cpp)
target_link_libraries(gti_installed_cpp_backend_smoke
                      PRIVATE GTI::cpp_backend)
add_test(NAME installed_cpp_backend_library
         COMMAND gti_installed_cpp_backend_smoke)

add_executable(gti_installed_driver_smoke ../driver_library_smoke.cpp)
target_link_libraries(gti_installed_driver_smoke PRIVATE GTI::driver)
add_test(NAME installed_driver_library COMMAND gti_installed_driver_smoke)

add_executable(gti_installed_layout_query_smoke
               ../layout_query_native_smoke.cpp)
target_link_libraries(gti_installed_layout_query_smoke PRIVATE GTI::compiler)
add_test(NAME installed_layout_query_native_boundary
         COMMAND gti_installed_layout_query_smoke)

add_executable(gti_installed_native_record_smoke ../native_record_tests.cpp)
target_link_libraries(gti_installed_native_record_smoke
                      PRIVATE GTI::cpp_backend)
add_test(NAME installed_native_record_pipeline
         COMMAND gti_installed_native_record_smoke)

add_executable(gti_installed_native_header_smoke ../native_header_tests.cpp)
target_link_libraries(gti_installed_native_header_smoke
                      PRIVATE GTI::cpp_backend)
add_test(NAME installed_native_header_pipeline
         COMMAND gti_installed_native_header_smoke)
