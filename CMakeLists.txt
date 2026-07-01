cmake_minimum_required(VERSION 3.20)
project("cpplang" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROJECT_SOURCES
  "main.cpp"
)

if(NOT PROJECT_SOURCES)
  message(FATAL_ERROR "No source files found for this target")
endif()

add_executable("cpplang" ${PROJECT_SOURCES})
