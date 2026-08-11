cmake_minimum_required(VERSION 3.20)

project(gti_installed_library_smoke LANGUAGES CXX)

include(CTest)
find_package(GTI CONFIG REQUIRED)

add_executable(gti_installed_compiler_smoke ../compiler_library_smoke.cpp)
target_link_libraries(gti_installed_compiler_smoke PRIVATE GTI::compiler)
add_test(NAME installed_compiler_library
         COMMAND gti_installed_compiler_smoke)

add_executable(gti_installed_driver_smoke ../driver_library_smoke.cpp)
target_link_libraries(gti_installed_driver_smoke PRIVATE GTI::driver)
add_test(NAME installed_driver_library COMMAND gti_installed_driver_smoke)
