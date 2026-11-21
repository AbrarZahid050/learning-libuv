
cmake_minimum_required(VERSION 3.20)

project(cppuv
    VERSION 0.1.0
    DESCRIPTION "A Windows asynchronous runtime written in C++"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# add_library(cppuv
#     src/loop.cpp
# )

add_library(cppuv
    src/loop.cpp
    src/poller.cpp
    src/winsock.cpp
    src/tcp.cpp
)

target_include_directories(cppuv
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(cppuv PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(cppuv
    PUBLIC
        ws2_32
        Threads::Threads
)

add_executable(cppuv_example
    examples/main.cpp
)

target_link_libraries(cppuv_example
    PRIVATE cppuv
)
