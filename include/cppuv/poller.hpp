#pragma once // Prevent duplicate inclusion of this header.

#ifndef WIN32_LEAN_AND_MEAN // Avoid including unrelated Windows declarations from windows.h.
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX // Prevent Windows headers from defining min and max macros.
#define NOMINMAX
#endif

#include <windows.h> // Provide HANDLE, DWORD, ULONG_PTR, OVERLAPPED, and IOCP functions.
#include <optional> // Represent a timeout as the absence of a completion.

namespace cppuv // Keep implementation types in the library namespace.
{

    // A normalized view of one result removed from the Windows completion port.
    struct Completion
    {
        DWORD bytesTransferred; // Number of bytes reported by the completed I/O operation.
        ULONG_PTR key; // Application-defined key associated with the completed handle.
        OVERLAPPED *overlapped; // Operation object supplied when the I/O was submitted; null for wake packets.
        DWORD error; // Windows error code, or ERROR_SUCCESS when the operation succeeded.

        // Return true when Windows reports successful completion of the operation.
        bool succeeded() const
        {
            return error == ERROR_SUCCESS; // IOCP failures are delivered as completions with a non-zero error.
        }

        // Identify a private wake packet posted by Poller::wake().
        bool isWake() const
        {
            return overlapped == nullptr && key == 0; // The library reserves this combination for loop wakeups.
        }
    };

    // Small RAII wrapper around a Windows I/O Completion Port.
    class Poller
    {
    public:
        // Create a new completion port or throw std::system_error on failure.
        Poller();

        // Close the completion-port handle owned by this object.
        ~Poller();

        Poller(const Poller &) = delete; // A completion-port handle must have exactly one owning wrapper.
        Poller &operator=(const Poller &) = delete; // Prevent accidental copying of the native handle.

        // Post a private completion packet to interrupt a thread waiting in wait().
        void wake();

        // Attach a file or socket handle to this completion port.
        void associate(
            HANDLE handle, // Native Windows handle to associate with the port.
            ULONG_PTR completionKey); // Key returned with every completion for this handle.

        // Wait for a completion; return an empty optional when the timeout expires.
        std::optional<Completion> wait(DWORD timeoutMs);

    private:
        HANDLE completionPort_ = nullptr; // Owned IOCP handle; null means construction did not establish a port.
    };

} // namespace cppuv
