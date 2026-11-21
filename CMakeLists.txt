cmake_minimum_required(VERSION 3.20)

project(cppuv
    VERSION 0.1.0
    DESCRIPTION "A reusable asynchronous I/O library for C++"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CPPUV_BUILD_EXAMPLES "Build CppUV example applications" ON)

# The current event-loop and TCP implementation uses Windows IOCP and Winsock.
# Other platform backends will be added behind the same public API.
if(NOT WIN32)
    message(FATAL_ERROR
        "CppUV currently supports Windows only. Linux and macOS backends are planned.")
endif()

find_package(Threads REQUIRED)

# Build the library independently from its example applications.
add_library(cppuv STATIC
    src/loop.cpp
    src/poller.cpp
    src/winsock.cpp
    src/tcp.cpp
)

add_library(cppuv::cppuv ALIAS cppuv)

target_compile_features(cppuv PUBLIC cxx_std_20)

target_include_directories(cppuv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(cppuv
    PUBLIC
        ws2_32
        Threads::Threads
)

if(MSVC)
    target_compile_options(cppuv PRIVATE /W4 /permissive-)
else()
    target_compile_options(cppuv PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CPPUV_BUILD_EXAMPLES)
    add_executable(cppuv_example examples/main.cpp)
    target_link_libraries(cppuv_example PRIVATE cppuv::cppuv)
endif()

# Install the library and public headers.
install(TARGETS cppuv
    EXPORT cppuvTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Export a namespaced target so consumers can use cppuv::cppuv.
install(EXPORT cppuvTargets
    FILE cppuvTargets.cmake
    NAMESPACE cppuv::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppuv
)

configure_package_config_file(
    cmake/cppuvConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppuv
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppuv
)
