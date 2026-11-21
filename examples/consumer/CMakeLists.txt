cmake_minimum_required(VERSION 3.20) # Require the CMake version used by the CppUV package.
project(cppuv_consumer_example LANGUAGES CXX) # Treat this folder as an independent application, not part of the library build.

find_package(cppuv 0.1 CONFIG REQUIRED) # Locate the installed CppUV package and import cppuv::cppuv.

add_executable(cppuv_consumer main.cpp) # Build a tiny application that uses only the public package.
target_compile_features(cppuv_consumer PRIVATE cxx_std_20) # Compile the consumer using the library's required C++ standard.
target_link_libraries(cppuv_consumer PRIVATE cppuv::cppuv) # Link against the installed imported target, not CppUV source files.
