#pragma once // Prevent duplicate inclusion of the Winsock lifetime declaration.

namespace cppuv // Keep the helper in the public library namespace.
{

    // Initializes Winsock 2.2 for the lifetime of this object.
    // Create this object before creating TCP sockets or starting network operations.
    class Winsock
    {
    public:
        // Call WSAStartup(MAKEWORD(2, 2)); throw std::system_error if initialization fails.
        Winsock();

        // Balance the successful WSAStartup call with WSACleanup().
        ~Winsock();

        Winsock(const Winsock &) = delete; // Each successful startup must have one matching cleanup.
        Winsock &operator=(const Winsock &) = delete; // Prevent copying the lifetime owner.
    };

} // namespace cppuv
