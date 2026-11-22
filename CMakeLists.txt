cmake_minimum_required(VERSION 3.20) # Require modern CMake features used by package installation.

project(cppuv # Define the library project and its package identity.
    VERSION 0.1.0 # Keep the package version in one place.
    DESCRIPTION "A reusable Windows asynchronous I/O library for C++" # Describe the supported target platform.
    LANGUAGES CXX # This project currently contains C++ code only.
)

include(GNUInstallDirs) # Use platform-aware standard installation directories.
include(CTest) # Enable CTest so library behavior can be verified automatically.
include(CMakePackageConfigHelpers) # Generate find_package-compatible package metadata.

option(CPPUV_BUILD_EXAMPLES "Build CppUV example applications" ON) # Let consumers build only the library when desired.

# CppUV intentionally targets Windows; its event loop is built around IOCP.
if(NOT WIN32) # Reject unsupported operating systems during configuration.
    message(FATAL_ERROR "CppUV is a Windows-only library.") # Avoid implying that other platforms are part of the current roadmap.
endif()

find_package(Threads REQUIRED) # Provide the standard CMake target for thread support.

# Compile the implementation once as a reusable static library.
add_library(cppuv STATIC
    src/loop.cpp # Implement the event loop and timer scheduler.
    src/poller.cpp # Implement the Windows I/O Completion Port wrapper.
    src/winsock.cpp # Manage the process-level Winsock initialization.
    src/tcp.cpp # Implement asynchronous TCP connections and accepting.
)

add_library(cppuv::cppuv ALIAS cppuv) # Give in-tree and installed consumers one stable target name.

target_compile_features(cppuv PUBLIC cxx_std_20) # Require C++20 from this library and its consumers.

target_include_directories(cppuv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> # Use repository headers while building.
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}> # Use installed headers after package installation.
)

target_link_libraries(cppuv
    PUBLIC
        ws2_32 # Link the Windows Winsock networking library.
        Threads::Threads # Link the platform thread runtime used by the library.
)

if(MSVC) # Select warning flags for Microsoft's compiler.
    target_compile_options(cppuv PRIVATE /W4 /permissive-) # Enable useful warnings and strict conformance.
else() # Use the corresponding warning flags for GCC and MinGW.
    target_compile_options(cppuv PRIVATE -Wall -Wextra -Wpedantic) # Report common mistakes without changing consumer flags.
endif()

if(CPPUV_BUILD_EXAMPLES) # Build the sample application only when requested.
    add_executable(cppuv_example examples/main.cpp) # Compile the TCP echo-server example.
    target_link_libraries(cppuv_example PRIVATE cppuv::cppuv) # Consume the library through its public target.
endif()

# Install the compiled library and export its target metadata.
install(TARGETS cppuv
    EXPORT cppuvTargets # Include this target in the generated CMake export.
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # Install static library archives.
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # Reserve the standard location for shared libraries.
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # Install runtime binaries on platforms that use them.
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} # Publish the public include directory to consumers.
)

install(DIRECTORY include/ # Copy the public C++ headers as a directory tree.
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} # Place headers under the standard include directory.
)

# Export a namespaced target so applications can link with cppuv::cppuv.
install(EXPORT cppuvTargets
    FILE cppuvTargets.cmake # Name the generated target-import file.
    NAMESPACE cppuv:: # Prefix imported targets to avoid name collisions.
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppuv # Store package metadata beside the library.
)

configure_package_config_file(
    cmake/cppuvConfig.cmake.in # Read the package configuration template.
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfig.cmake # Write the configured package file into the build tree.
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppuv # Record the installed package location.
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfigVersion.cmake # Generate version compatibility metadata.
    VERSION ${PROJECT_VERSION} # Keep package metadata aligned with project(VERSION).
    COMPATIBILITY SameMajorVersion # Permit compatible updates within the same major version.
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfig.cmake # Install the package entry point used by find_package().
    ${CMAKE_CURRENT_BINARY_DIR}/cppuvConfigVersion.cmake # Install the package version compatibility file.
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppuv # Keep all package metadata in one directory.
)

if(BUILD_TESTING) # Build focused regression tests when CTest is enabled.
    add_subdirectory(tests) # Keep test targets separate from the library and examples.
endif()
