# CppUV

CppUV is a C++20 asynchronous I/O library inspired by libuv. It provides a reusable event loop and asynchronous networking APIs behind a consistent C++ interface.

> **Platform:** CppUV is intentionally Windows-only. The implementation uses I/O Completion Ports (IOCP), Winsock, and Windows overlapped I/O. Linux and macOS support are not part of this project.

## Current capabilities

- IOCP-backed event loop
- Thread-safe task posting and loop wakeups
- One-shot and repeating timers
- TCP server and connection support
- Asynchronous reads and queued writes
- Write-queue backpressure
- Connection lifecycle and graceful I/O shutdown
- TCP idle timeouts

## Build

Requirements:
- Windows
- CMake 3.20 or newer
- A C++20 compiler (MinGW-w64 or MSVC)

From PowerShell:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

To build only the library:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCPPUV_BUILD_EXAMPLES=OFF
cmake --build build
```

## Run regression tests

CTest is enabled by default. Use a separate build directory so existing generated files under `build/` are not reused:

```powershell
cmake -S . -B build-tests -G "MinGW Makefiles" -DBUILD_TESTING=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The regression tests cover task dispatch, timer scheduling and cancellation, callback exception handling, empty callback validation, and draining a pending `AcceptEx` operation after its `TcpServer` wrapper is destroyed.

## Callback exceptions

Configure an exception handler to receive exceptions thrown by posted tasks, timers, or completion callbacks:

```cpp
loop.setExceptionHandler([](std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception &exception) {
        // Log or report exception.what().
    }
});
```

If no exception handler is configured, an uncaught callback exception requests loop shutdown. Outstanding overlapped I/O is still dispatched and drained.

## Install

Install into a local directory:

```powershell
cmake --install build --prefix "$PWD\install"
```

The installation contains the public headers, static library, and CMake package files.

## Use CppUV from another CMake project

After installing CppUV, a consumer project can use the exported target:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

find_package(cppuv 0.1 CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE cppuv::cppuv)
```

Configure the consumer with the installation prefix in CMAKE_PREFIX_PATH, for example:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="E:\path\to\learning-libuv\install"
cmake --build build
```

In C++ source files, include the umbrella header:

```cpp
#include <cppuv/cppuv.hpp>
```

The umbrella header exposes the current public event-loop, TCP, and Winsock APIs. Applications that need finer-grained includes can include the individual headers under cppuv/.


## Verify the installed package with a separate application

The consumer example is an independent CMake project. It finds the installed package and links to `cppuv::cppuv`; it does not include or compile files from the library's `src/` directory.

From the repository root, first build and install CppUV:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCPPUV_BUILD_EXAMPLES=ON
cmake --build build
cmake --install build --prefix "$PWD\\install"
```

Then configure and build the separate consumer project:

```powershell
cmake -S examples/consumer -B consumer-build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="$PWD\\install"
cmake --build consumer-build
.\\consumer-build\\cppuv_consumer.exe
```

A successful run prints a message from the consumer callback and exits after the event loop stops. This checks the installed headers, exported CMake target, static library, and transitive link dependencies together.

## Project structure

- include/cppuv/ — public C++ headers
- src/ — Windows-specific library implementation
- cmake/ — CMake package configuration
- examples/ — example applications

The library target is built independently from the examples. Consumers link to cppuv::cppuv; they do not compile the implementation source files themselves.

## Development direction

The next library milestones are to stabilize the public handle APIs, add automated tests, and expand Windows asynchronous I/O support with UDP, DNS, worker-pool tasks, asynchronous filesystem operations, named pipes, and process management.
