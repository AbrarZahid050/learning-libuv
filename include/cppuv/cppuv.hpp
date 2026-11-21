#pragma once // Prevent duplicate inclusion of the public umbrella header.

// Public event-loop API, including tasks, timers, and IOCP completion dispatch.
#include "cppuv/loop.hpp"

// Public TCP server and connection API built on the event loop.
#include "cppuv/tcp.hpp"

// Winsock runtime lifetime helper required before using TCP sockets.
#include "cppuv/winsock.hpp"
