#pragma once // Prevent duplicate inclusion of the TCP API.

#ifndef WIN32_LEAN_AND_MEAN // Keep Windows headers focused on APIs needed by this library.
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX // Stop Windows headers from defining macros that conflict with std::min/std::max.
#define NOMINMAX
#endif

#include <winsock2.h> // Provide SOCKET and Winsock socket function declarations.
#include <mswsock.h> // Provide the AcceptEx extension-function declarations.

#include "cppuv/loop.hpp" // TCP operations are submitted to and completed by a Loop.
#include <array> // Own fixed-size buffers used by overlapped reads and AcceptEx.
#include <chrono> // Express the optional TCP idle timeout.
#include <deque> // Preserve the order of writes waiting to be sent.
#include <functional> // Define data, close, write, connection, and error callbacks.
#include <memory> // Retain connections and operation buffers until completions arrive.
#include <string> // Own outbound byte sequences.
#include <string_view> // Expose received bytes without copying them.
#include <system_error> // Pass operating-system failures to error callbacks.

namespace cppuv // Keep TCP types in the library namespace.
{

    // Represents one connected TCP socket whose I/O is completed through IOCP.
    class TcpConnection : public std::enable_shared_from_this<TcpConnection>
    {
    public:
        // Describe the public lifecycle of a connection.
        enum class State
        {
            Created, // The wrapper exists, but start() has not begun asynchronous reads.
            Connected, // The connection is active and may read or write.
            Closing, // Close was requested; outstanding kernel operations are draining.
            Closed // All operations have drained and the close callback has been notified.
        };

        // Explain why the connection entered its closing state.
        enum class CloseReason
        {
            None, // No close reason has been selected yet.
            Local, // The application called close().
            Remote, // The peer performed an orderly shutdown.
            Error, // A socket operation failed.
            Timeout // The configured inbound-idle timeout expired.
        };

        using DataCallback =
            std::function<void(TcpConnection &, std::string_view)>; // Receives bytes valid for the duration of the callback.
        using WriteCallback = std::function<void()>; // Runs after the corresponding queued bytes have been sent.
        using CloseCallback = std::function<void()>; // Runs once after outstanding I/O has drained.
        using ErrorCallback =
            std::function<void(const std::error_code &)>; // Reports a socket error before final close notification.

        // Take ownership of an already-created connected Winsock socket.
        TcpConnection(Loop &loop, SOCKET socket);

        // Begin closing the socket and release resources when completions drain.
        ~TcpConnection();

        TcpConnection(const TcpConnection &) = delete; // The socket and pending operations have one logical owner.
        TcpConnection &operator=(const TcpConnection &) = delete; // Prevent copying the connection state.

        // Register callbacks and start the first asynchronous read.
        // The object must already be managed by std::shared_ptr before this call.
        void start(
            DataCallback onData, // Called whenever bytes arrive from the peer.
            CloseCallback onClose = {}, // Optional notification after the connection is fully closed.
            ErrorCallback onError = {}); // Optional notification when a socket operation fails.

        // Queue bytes for ordered asynchronous transmission.
        // The optional callback runs after all bytes in this write have been sent.
        void write(std::string data, WriteCallback onWritten = {});

        // Temporarily stop submitting new receive operations.
        void pauseRead();

        // Resume receiving after pauseRead() or write-queue backpressure.
        void resumeRead();

        // Configure how long the connection may receive no inbound data before closing.
        // A zero duration disables the timeout.
        void setIdleTimeout(std::chrono::milliseconds timeout);

        // Begin graceful asynchronous close and discard writes not yet submitted.
        void close();

        // Expose the underlying socket for advanced Windows-specific integrations.
        [[nodiscard]] SOCKET nativeHandle() const noexcept
        {
            return socket_; // Return the socket value without transferring ownership.
        }

        // Return true while closing or after fully closed.
        [[nodiscard]] bool isClosed() const noexcept
        {
            return state_ == State::Closed || state_ == State::Closing; // Treat both terminal phases as unavailable for new writes.
        }

        // Read the current lifecycle state.
        [[nodiscard]] State state() const noexcept
        {
            return state_; // State changes are performed by the event-loop thread.
        }

        // Read the reason recorded when close began.
        [[nodiscard]] CloseReason closeReason() const noexcept
        {
            return closeReason_; // The reason remains available after final close.
        }

    private:
        // Own the OVERLAPPED structure and buffers for one kernel I/O request.
        struct IoOperation
        {
            enum class Kind { Read, Write }; // Select the completion handler for this operation.

            explicit IoOperation(Kind operationKind) : kind(operationKind) {} // Record whether this request receives or sends bytes.

            OVERLAPPED overlapped{}; // Windows writes completion metadata into this structure.
            WSABUF buffer{}; // Describe the memory region passed to WSARecv or WSASend.
            Kind kind; // Remember which operation-specific completion path to execute.
            std::array<char, 8192> readData{}; // Keep receive memory alive until Windows reports completion.
            std::shared_ptr<std::string> writeData; // Keep the sent string alive while WSASend may access it.
            std::shared_ptr<TcpConnection> keepAlive; // Prevent destruction while this operation is outstanding.
            std::size_t writeOffset = 0; // Track the offset submitted when a partial write is retried.
        };

        // Store an application write until all of its bytes have been transmitted.
        struct PendingWrite
        {
            std::shared_ptr<std::string> data; // Own the complete payload across partial sends.
            WriteCallback callback; // Notify the caller when this payload has fully drained.
            std::size_t offset = 0; // Number of bytes already transmitted from this payload.
        };

        static constexpr std::size_t kWriteHighWatermark = 64 * 1024; // Pause reads when queued output reaches 64 KiB.
        static constexpr std::size_t kWriteLowWatermark = 32 * 1024; // Resume reads after queued output falls to 32 KiB.

        void postRead(); // Submit a new WSARecv operation when reading is allowed.
        void postWrite(); // Submit the next queued WSASend operation.
        void updateReadBackpressure(); // Pause or resume reads based on queued output size.
        void refreshIdleTimer(); // Restart the inbound-idle timer after received data.
        void handleCompletion(const Completion &completion); // Route one IOCP completion to its operation-specific handler.
        void handleRead(IoOperation &operation, const Completion &completion); // Process received bytes or peer disconnect.
        void handleWrite(IoOperation &operation, const Completion &completion); // Account for partial sends and advance the write queue.
        void fail(int error); // Report an operating-system error and begin closing.
        void finishCloseIfDrained(); // Remove the completion handler after every operation has completed.

        Loop *loop_; // Non-owning pointer; the Loop must outlive this connection.
        SOCKET socket_; // Owned Winsock socket, or INVALID_SOCKET after close begins.
        std::deque<PendingWrite> writes_; // FIFO queue ensures application writes preserve order.
        DataCallback onData_; // Callback for received bytes.
        CloseCallback onClose_; // Callback for final close notification.
        ErrorCallback onError_; // Callback for asynchronous socket errors.
        std::size_t pendingOperations_ = 0; // Count this connection's outstanding overlapped operations.
        std::size_t queuedWriteBytes_ = 0; // Count bytes not yet confirmed as sent.
        bool started_ = false; // Prevent start() from registering callbacks more than once.
        bool reading_ = false; // Ensure no more than one receive is outstanding.
        bool readPaused_ = false; // Record an explicit application pause.
        bool backpressurePaused_ = false; // Record a pause caused by the output queue high-water mark.
        bool writing_ = false; // Ensure no more than one send is outstanding.
        bool closed_ = false; // Prevent new operations once close has started.
        bool handlerRegistered_ = false; // Track whether this connection owns an entry in the Loop dispatch table.
        bool closeNotified_ = false; // Guarantee the close callback is delivered at most once.
        std::chrono::milliseconds idleTimeout_{0}; // Zero disables the inbound-idle timer.
        Loop::TimerId idleTimer_ = 0; // Zero means no idle timer is currently scheduled.
        State state_ = State::Created; // New connections begin before asynchronous I/O is started.
        CloseReason closeReason_ = CloseReason::None; // Record the first reason that initiates close.
    };

    // Listens for incoming TCP clients using the Windows AcceptEx extension.
    class TcpServer
    {
    public:
        using ConnectionCallback = std::function<void(SOCKET)>; // Receives ownership of each accepted socket.
        using ErrorCallback = std::function<void(const std::error_code &)>; // Reports accept or accepted-socket setup failures.

        // Create an overlapped IPv4 TCP listening socket.
        TcpServer();

        // Stop accepting and release the listening socket.
        ~TcpServer();

        TcpServer(const TcpServer &) = delete; // A listening socket and pending AcceptEx request have one owner.
        TcpServer &operator=(const TcpServer &) = delete; // Prevent copying the server's native resources.

        // Bind the listening socket to the loopback address and requested port.
        void listen(unsigned short port);

        // Start the asynchronous accept cycle and register it with the supplied loop.
        // The callback takes ownership of each accepted socket.
        void startAccepting(
            Loop &loop, // Event loop that receives AcceptEx completion packets.
            ConnectionCallback onConnection, // Called with each newly accepted socket.
            ErrorCallback onError = {}); // Optional callback for accept failures.

        // Stop accepting new clients and cancel any pending AcceptEx.
        // The event loop must continue running until its cancellation completion is dispatched.
        void stopAccepting();

        // Expose the listening socket for advanced Windows-specific integrations.
        [[nodiscard]] SOCKET nativeHandle() const noexcept
        {
            return socket_; // Return the native value without transferring ownership.
        }

    private:
        struct AcceptOperation; // Keep the Windows-specific overlapped accept details in the implementation file.
        struct CompletionLifetime; // Keep the completion callback safe if the server wrapper is destroyed.

        void postAccept(); // Submit one AcceptEx request when the server is allowed to accept.
        void handleAcceptCompletion(const Completion &completion); // Finish setup and deliver a newly accepted socket.

        SOCKET socket_ = INVALID_SOCKET; // Listening socket owned by this server.
        std::shared_ptr<CompletionLifetime> completionLifetime_; // Keeps pending AcceptEx cleanup alive beyond the wrapper lifetime.
        LPFN_ACCEPTEX acceptEx_ = nullptr; // Function pointer loaded from Winsock for AcceptEx.
        Loop *loop_ = nullptr; // Non-owning pointer; the Loop must outlive active accepting.
        ConnectionCallback onConnection_; // Application callback for successful accepts.
        ErrorCallback onError_; // Application callback for asynchronous accept failures.
        AcceptOperation *pendingAccept_ = nullptr; // Outstanding AcceptEx operation, if one has been submitted.
        bool accepting_ = false; // Prevent duplicate accept submissions after stopAccepting().
    };

} // namespace cppuv
