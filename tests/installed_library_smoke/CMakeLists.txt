cmake_minimum_required(VERSION 3.20)

# System LLVM packages may run C feature probes while loading LLVMConfig.cmake
# (Homebrew LLVM 20 checks libedit this way), even though these smoke programs
# are C++. Enable both languages so this project exercises the real downstream
# package contract on every supported system LLVM distribution.
project(gti_installed_library_smoke LANGUAGES C CXX)

include(CTest)
find_package(GTI CONFIG REQUIRED)

add_executable(gti_installed_charconv_smoke ../runtime_charconv_tests.cpp)
target_link_libraries(gti_installed_charconv_smoke PRIVATE GTI::runtime)
target_compile_features(gti_installed_charconv_smoke PRIVATE cxx_std_20)
add_test(NAME installed_charconv_boundary COMMAND gti_installed_charconv_smoke)

add_executable(gti_installed_memory_smoke ../resource_memory_runtime_tests.cpp)
target_link_libraries(gti_installed_memory_smoke PRIVATE GTI::runtime)
target_compile_features(gti_installed_memory_smoke PRIVATE cxx_std_20)
add_test(NAME installed_memory_boundary COMMAND gti_installed_memory_smoke)

add_executable(gti_installed_filesystem_smoke ../runtime_filesystem_tests.cpp)
target_link_libraries(gti_installed_filesystem_smoke PRIVATE GTI::runtime)
target_compile_features(gti_installed_filesystem_smoke PRIVATE cxx_std_20)
add_test(NAME installed_filesystem_boundary COMMAND gti_installed_filesystem_smoke)

add_executable(gti_installed_cthread_smoke ../cthread_library_smoke.c)
target_compile_features(gti_installed_cthread_smoke PRIVATE c_std_11)
target_link_libraries(gti_installed_cthread_smoke PRIVATE GTI::runtime)
add_test(NAME installed_cthread_library COMMAND gti_installed_cthread_smoke)

add_executable(gti_installed_compiler_smoke ../compiler_library_smoke.cpp)
target_link_libraries(gti_installed_compiler_smoke PRIVATE GTI::compiler)
add_test(NAME installed_compiler_library
         COMMAND gti_installed_compiler_smoke)

add_executable(gti_installed_embedding_request_smoke embedding_request_smoke.cpp)
target_link_libraries(gti_installed_embedding_request_smoke PRIVATE GTI::compiler)
add_test(NAME installed_embedding_request COMMAND gti_installed_embedding_request_smoke)

add_executable(gti_installed_runtime_failure_header_smoke
               ../runtime_failure_header_smoke.cpp)
target_link_libraries(gti_installed_runtime_failure_header_smoke
                      PRIVATE GTI::compiler GTI::runtime)
add_test(NAME installed_runtime_failure_header_coexistence
         COMMAND gti_installed_runtime_failure_header_smoke)

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

add_executable(gti_installed_unsafe_function_smoke ../unsafe_function_tests.cpp)
target_link_libraries(gti_installed_unsafe_function_smoke PRIVATE GTI::cpp_backend)
add_test(NAME installed_unsafe_function_contract
         COMMAND gti_installed_unsafe_function_smoke)
