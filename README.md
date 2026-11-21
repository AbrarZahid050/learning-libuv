# CppUV

CppUV is a C++20 asynchronous I/O library inspired by libuv. Its goal is to provide a reusable event loop and asynchronous networking APIs behind a consistent C++ interface.

> **Current status:** the first backend is Windows-only and uses I/O Completion Ports (IOCP) and Winsock. Linux (epoll) and macOS (kqueue) backends are future work.

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

## Project structure

- include/cppuv/ — public C++ headers
- src/ — library implementation
- cmake/ — CMake package configuration
- examples/ — example applications

The library target is built independently from the examples. Consumers link to cppuv::cppuv; they do not compile the implementation source files themselves.

## Roadmap

1. Stabilize the public handle and event-loop APIs.
2. Add asynchronous UDP and DNS.
3. Add a worker pool and asynchronous filesystem operations.
4. Separate platform-independent APIs from OS-specific backends.
5. Add Linux and macOS backends.
6. Add automated tests, CI, and release packaging.
