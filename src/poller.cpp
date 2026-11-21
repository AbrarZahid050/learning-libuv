#include "cppuv/poller.hpp" // Bring in the Poller and Completion declarations.
#include <system_error> // Convert Windows failures into standard C++ exceptions.

namespace cppuv // Define the IOCP wrapper in the library namespace.
{

    // Create a new Windows I/O Completion Port.
    Poller::Poller()
    {
        completionPort_ = CreateIoCompletionPort( // Create an IOCP not yet associated with a file handle.
            INVALID_HANDLE_VALUE, // This special value requests creation of a fresh completion port.
            nullptr, // No existing port is being reused.
            0, // No completion key is needed for the initial port creation.
            1); // Allow one thread to run concurrently inside the completion port.

        if (completionPort_ == nullptr) // Windows returns null when IOCP creation fails.
        {
            throw std::system_error( // Surface the native error through the standard C++ error mechanism.
                static_cast<int>(GetLastError()), // Capture the Windows error code immediately.
                std::system_category(), // Interpret the value as a system error.
                "Failed to create IOCP"); // Add context to the exception message.
        }
    }

    // Release the native completion-port handle.
    Poller::~Poller()
    {
        if (completionPort_ != nullptr) // Check that construction successfully created the port.
        {
            CloseHandle(completionPort_); // Release the operating-system resource.
        }
    }

    // Post a private completion packet to wake a thread blocked in GetQueuedCompletionStatus.
    void Poller::wake()
    {
        if (!PostQueuedCompletionStatus( // Insert a synthetic packet into the completion queue.
                completionPort_, // Target this Poller's completion port.
                0, // Wake packets do not represent transferred bytes.
                0, // Completion key zero is reserved for wake packets.
                nullptr)) // Null OVERLAPPED distinguishes the packet from real I/O.
        {
            throw std::system_error( // Report failure to wake the event loop.
                static_cast<int>(GetLastError()), // Preserve the native Windows error.
                std::system_category(), // Use the Windows system error category.
                "Failed to wake IOCP"); // Explain which operation failed.
        }
    }

    // Associate a file or socket handle with this completion port.
    void Poller::associate(
        HANDLE handle, // Native handle whose I/O completions should arrive at this port.
        ULONG_PTR completionKey) // Application-defined value returned with each completion.
    {
        HANDLE result = CreateIoCompletionPort( // Associate the handle with an existing completion port.
            handle, // The file or socket handle to attach.
            completionPort_, // The completion port owned by this Poller.
            completionKey, // The key used later to dispatch the completion.
            0); // Keep the concurrency setting established when the port was created.

        if (result == nullptr) // A null result indicates association failure.
        {
            throw std::system_error( // Convert the Windows error into a C++ exception.
                static_cast<int>(GetLastError()), // Capture the reason returned by Windows.
                std::system_category(), // Interpret the code as a system error.
                "Failed to associate handle with IOCP"); // Identify the failed operation.
        }
    }

    // Wait for one completion packet or return empty when the wait times out.
    std::optional<Completion> Poller::wait(DWORD timeoutMs)
    {
        DWORD bytesTransferred = 0; // Receive the number of bytes transferred by the completed operation.
        ULONG_PTR completionKey = 0; // Receive the key associated with the completed handle.
        OVERLAPPED *overlapped = nullptr; // Receive the operation structure supplied at I/O submission.

        const BOOL result = GetQueuedCompletionStatus( // Remove one packet from the IOCP queue.
            completionPort_, // Wait on this Poller's completion port.
            &bytesTransferred, // Output the transferred byte count.
            &completionKey, // Output the handle's completion key.
            &overlapped, // Output the OVERLAPPED pointer for the operation.
            timeoutMs); // Stop waiting when this many milliseconds have elapsed.

        if (!result) // FALSE may mean timeout, a failed I/O operation, or a wait error.
        {
            const DWORD error = GetLastError(); // Capture the failure reason before calling another Windows API.

            // No completion arrived before the timeout.
            if (overlapped == nullptr && error == WAIT_TIMEOUT) // A null operation plus WAIT_TIMEOUT means no packet was dequeued.
            {
                return std::nullopt; // Let the event loop recalculate timers and queued tasks.
            }

            // An overlapped operation completed with an error.
            // This is still a valid completion to dispatch.
            if (overlapped != nullptr) // Windows supplies the operation pointer for failed I/O completions too.
            {
                return Completion{ // Preserve the failed completion for its owning handle.
                    bytesTransferred, // Include any byte count reported by Windows.
                    completionKey, // Preserve the key used to find the completion handler.
                    overlapped, // Preserve the operation pointer so its owner can release resources.
                    error}; // Store the failure code for operation-specific handling.
            }

            throw std::system_error( // A null operation outside the timeout case indicates a wait-level failure.
                static_cast<int>(error), // Preserve the native Windows error code.
                std::system_category(), // Interpret the code as a system error.
                "IOCP wait failed"); // Explain the source of the exception.
        }

        return Completion{ // A successful dequeue always produces a normalized completion record.
            bytesTransferred, // Include the number of bytes transferred.
            completionKey, // Include the associated handle's key.
            overlapped, // Include the operation pointer, or null for a wake packet.
            ERROR_SUCCESS}; // Mark the completion as successful.
    }

} // namespace cppuv
