cmake_minimum_required(VERSION 3.15)
project(FourArithmetic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(Myapp main.cpp)

if (MSVC)
    target_compile_options(Myapp PRIVATE /W4 /utf-8)
else()
    target_compile_options(Myapp PRIVATE -Wall -Wextra -Wpedantic)
endif()
