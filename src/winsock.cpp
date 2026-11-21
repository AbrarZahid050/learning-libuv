#include "cppuv/winsock.hpp" // Bring in the public Winsock lifetime helper.
#include <winsock2.h> // Provide WSAStartup, WSACleanup, and WSADATA.
#include <system_error> // Convert Winsock initialization failures into C++ exceptions.

namespace cppuv // Define the helper in the library namespace.
{

    // Initialize the Winsock runtime required by all socket operations.
    Winsock::Winsock()
    {
        WSADATA data{}; // Let Winsock fill this structure with runtime version information.

        const int result = WSAStartup( // Initialize the process-level Winsock subsystem.
            MAKEWORD(2, 2), // Request Winsock version 2.2.
            &data); // Receive details about the initialized implementation.

        if (result != 0) // WSAStartup returns zero only when initialization succeeds.
        {
            throw std::system_error( // Make initialization failure visible to C++ callers.
                result, // WSAStartup returns its error code directly.
                std::system_category(), // Use the standard system error category.
                "Failed to initialize Winsock"); // Explain why the library could not start networking.
        }
    }

    // Balance the successful WSAStartup call performed by the constructor.
    Winsock::~Winsock()
    {
        WSACleanup(); // Release this object's Winsock initialization reference.
    }

} // namespace cppuv
