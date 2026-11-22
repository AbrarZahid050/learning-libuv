#include "cppuv/tcp.hpp" // Include required Windows, C++, and CppUV declarations.
#include <algorithm> // Include required Windows, C++, and CppUV declarations.
#include <memory> // Include required Windows, C++, and CppUV declarations.
#include <limits> // Include required Windows, C++, and CppUV declarations.
#include <stdexcept> // Include required Windows, C++, and CppUV declarations.
#include <system_error> // Include required Windows, C++, and CppUV declarations.
#include <utility> // Include required Windows, C++, and CppUV declarations.
#include <ws2tcpip.h> // Include required Windows, C++, and CppUV declarations.

namespace cppuv
{

    TcpConnection::TcpConnection(Loop &loop, SOCKET socket)
        : loop_(&loop), socket_(socket)
    {
        if (socket_ == INVALID_SOCKET)
        {
            throw std::invalid_argument("TcpConnection requires a valid socket");
        }
    }

    // Request shutdown when the connection wrapper is destroyed.
    TcpConnection::~TcpConnection()
    {
        close();
    }

    // Validate ownership, register the IOCP handler, and begin receiving.
    void TcpConnection::start(
        DataCallback onData,
        CloseCallback onClose,
        ErrorCallback onError)
    {
        if (closed_ || started_)
        {
            throw std::logic_error("TCP connection is closed or already started");
        }
        if (weak_from_this().expired())
        {
            throw std::logic_error(
                "TcpConnection must be owned by std::shared_ptr before start()");
        }
        if (!onData)
        {
            throw std::invalid_argument("TCP data callback cannot be empty");
        }

        onData_ = std::move(onData);
        onClose_ = std::move(onClose);
        onError_ = std::move(onError);

        // Use this object's address as the IOCP completion-dispatch key.
        const auto completionKey = reinterpret_cast<ULONG_PTR>(this);
        loop_->associateHandle(reinterpret_cast<HANDLE>(socket_), completionKey);
        loop_->registerCompletionHandler(
            completionKey,
            [this](const Completion &completion)
            {
                handleCompletion(completion);
            });

        handlerRegistered_ = true;
        started_ = true;
        state_ = State::Connected;
        refreshIdleTimer();
        postRead();
    }

    void TcpConnection::setIdleTimeout(std::chrono::milliseconds timeout)
    {
        if (closed_)
        {
            throw std::logic_error("Cannot configure the idle timeout on a closed connection");
        }
        if (timeout.count() < 0)
        {
            throw std::invalid_argument("TCP idle timeout cannot be negative");
        }

        idleTimeout_ = timeout;
        refreshIdleTimer();
    }

    // Refresh the idle deadline whenever data arrives from the peer.
    void TcpConnection::refreshIdleTimer()
    {
        if (idleTimer_ != 0)
        {
            loop_->clearTimer(idleTimer_);
            idleTimer_ = 0;
        }

        if (!started_ || closed_ || idleTimeout_.count() <= 0)
        {
            return;
        }

        const std::weak_ptr<TcpConnection> weakConnection = weak_from_this();
        idleTimer_ = loop_->callAfter(idleTimeout_, [weakConnection]()
        {
            if (auto connection = weakConnection.lock())
            {
                connection->idleTimer_ = 0;

                if (!connection->closed_)
                {
                    connection->closeReason_ = CloseReason::Timeout;
                    connection->close();
                }
            }
        });
    }

    void TcpConnection::write(std::string data, WriteCallback onWritten)
    {
        if (closed_)
        {
            throw std::logic_error("Cannot write to a closed TCP connection");
        }
        if (data.empty())
        {
            if (onWritten) onWritten();
            return;
        }

        if (data.size() > std::numeric_limits<std::size_t>::max() - queuedWriteBytes_)
        {
            throw std::length_error("TCP write queue size overflow");
        }

        const auto dataSize = data.size();
        writes_.push_back(PendingWrite{
            std::make_shared<std::string>(std::move(data)),
            std::move(onWritten),
            0});
        queuedWriteBytes_ += dataSize;

        updateReadBackpressure();
        if (!writing_) postWrite();
    }

    // Temporarily stop scheduling additional receive operations.
    void TcpConnection::pauseRead()
    {
        readPaused_ = true;
    }

    // Resume receiving when application and backpressure state allow it.
    void TcpConnection::resumeRead()
    {
        if (closed_) return;
        readPaused_ = false;
        postRead();
    }

    // Apply read-side backpressure using separate high and low queue thresholds.
    void TcpConnection::updateReadBackpressure()
    {
        if (closed_) return;

        if (!backpressurePaused_ &&
            queuedWriteBytes_ >= kWriteHighWatermark)
        {
            backpressurePaused_ = true;
        }
        else if (backpressurePaused_ &&
                 queuedWriteBytes_ <= kWriteLowWatermark)
        {
            backpressurePaused_ = false;
            postRead();
        }
    }

    // Begin socket shutdown while allowing outstanding overlapped I/O to drain.
    void TcpConnection::close()
    {
        if (closed_) return;

        closed_ = true;
        state_ = State::Closing;

        if (idleTimer_ != 0)
        {
            loop_->clearTimer(idleTimer_);
            idleTimer_ = 0;
        }
        if (closeReason_ == CloseReason::None)
        {
            closeReason_ = CloseReason::Local;
        }

        // closesocket cancels outstanding overlapped socket operations. Their
        // completion packets still own the operation objects and drain the
        // connection's pending-operation count before its handler is removed.
        if (socket_ != INVALID_SOCKET)
        {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }

        // In-flight WSASend buffers are independently owned by IoOperation.
        // Queued writes that have not been submitted can be discarded now.
        writes_.clear();
        queuedWriteBytes_ = 0;
        finishCloseIfDrained();
    }

    // Submit one receive operation whose buffer remains alive until IOCP completion.
    void TcpConnection::postRead()
    {
        if (closed_ || reading_ || readPaused_ || backpressurePaused_ ||
            loop_->isStopping()) return;

        auto operation = std::make_unique<IoOperation>(IoOperation::Kind::Read);
        // Keep the connection alive while Windows may still access this operation.
        operation->keepAlive = shared_from_this();
        operation->buffer.buf = operation->readData.data();
        operation->buffer.len = static_cast<ULONG>(operation->readData.size());

        if (!loop_->beginOperation()) return;

        DWORD flags = 0;
        DWORD bytesReceived = 0;
        reading_ = true;
        ++pendingOperations_;

        if (!operation->request.markSubmitted())
        {
            reading_ = false;
            --pendingOperations_;
            loop_->endOperation();
            throw std::logic_error("TCP read request was already submitted");
        }

        const int result = WSARecv(
            socket_,
            &operation->buffer,
            1,
            &bytesReceived,
            &flags,
            &operation->request.overlapped,
            nullptr);

        if (result == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error != WSA_IO_PENDING)
            {
                if (!operation->request.markSubmissionFailed())
                {
                    reading_ = false;
                    --pendingOperations_;
                    loop_->endOperation();
                    throw std::logic_error("TCP read request could not record submission failure");
                }

                reading_ = false;
                --pendingOperations_;
                loop_->endOperation();
                operation.reset();
                fail(error);
                return;
            }
        }

        operation.release();
    }

    // Submit the unsent portion of the first queued payload.
    void TcpConnection::postWrite()
    {
        if (closed_ || writing_ || writes_.empty() || loop_->isStopping()) return;

        auto &pending = writes_.front();
        auto operation = std::make_unique<IoOperation>(IoOperation::Kind::Write);
        operation->keepAlive = shared_from_this();
        operation->writeData = pending.data;
        operation->writeOffset = pending.offset;

        const auto remaining = pending.data->size() - pending.offset;
        operation->buffer.buf = pending.data->data() + pending.offset;
        operation->buffer.len = static_cast<ULONG>(
            std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));

        if (!loop_->beginOperation()) return;

        DWORD bytesSent = 0;
        writing_ = true;
        ++pendingOperations_;

        if (!operation->request.markSubmitted())
        {
            writing_ = false;
            --pendingOperations_;
            loop_->endOperation();
            throw std::logic_error("TCP write request was already submitted");
        }

        const int result = WSASend(
            socket_,
            &operation->buffer,
            1,
            &bytesSent,
            0,
            &operation->request.overlapped,
            nullptr);

        if (result == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error != WSA_IO_PENDING)
            {
                if (!operation->request.markSubmissionFailed())
                {
                    writing_ = false;
                    --pendingOperations_;
                    loop_->endOperation();
                    throw std::logic_error("TCP write request could not record submission failure");
                }

                writing_ = false;
                --pendingOperations_;
                loop_->endOperation();
                operation.reset();
                fail(error);
                return;
            }
        }

        operation.release();
    }

    void TcpConnection::handleCompletion(const Completion &completion)
    {
        auto self = shared_from_this();
        (void)self; // Keep the connection alive until this dispatch returns.
        if (completion.overlapped == nullptr) return;

        auto *request = reinterpret_cast<detail::Request *>(completion.overlapped);
        std::unique_ptr<IoOperation> operation(
            reinterpret_cast<IoOperation *>(request));

        try
        {
            if (!operation->request.markCompleted())
            {
                throw std::logic_error("TCP completion received for a request that was not submitted");
            }

            if (operation->kind == IoOperation::Kind::Read)
            {
                handleRead(*operation, completion);
            }
            else
            {
                handleWrite(*operation, completion);
            }
        }
        catch (...)
        {
            if (pendingOperations_ > 0) --pendingOperations_;
            loop_->endOperation();

            // A throwing application callback must not leave this connection
            // open with no receive or send operation scheduled.
            if (!closed_)
            {
                closeReason_ = CloseReason::Error;
                close();
            }

            finishCloseIfDrained();
            throw;
        }

        if (pendingOperations_ > 0) --pendingOperations_;
        loop_->endOperation();
        finishCloseIfDrained();
    }

    // Process received bytes, peer shutdown, and receive errors.
    void TcpConnection::handleRead(
        IoOperation &operation,
        const Completion &completion)
    {
        reading_ = false;

        if (closed_) return;
        if (!completion.succeeded())
        {
            fail(static_cast<int>(completion.error));
            return;
        }
        if (completion.bytesTransferred == 0)
        {
            closeReason_ = CloseReason::Remote;
            close();
            return;
        }

        // This view points into the operation buffer and is valid only during the callback.
        const std::string_view data(
            operation.readData.data(),
            completion.bytesTransferred);

        refreshIdleTimer();

        if (onData_) onData_(*this, data);

        if (!closed_ && !readPaused_ && !loop_->isStopping()) postRead();
    }

    // Account for partial sends and advance the ordered write queue.
    void TcpConnection::handleWrite(
        IoOperation &operation,
        const Completion &completion)
    {
        writing_ = false;

        if (closed_) return;
        if (!completion.succeeded())
        {
            fail(static_cast<int>(completion.error));
            return;
        }
        if (writes_.empty()) return;

        auto &pending = writes_.front();
        const auto remaining = pending.data->size() - pending.offset;
        // A send can be partial; account only for the bytes confirmed by Windows.
        const auto sent = static_cast<std::size_t>(completion.bytesTransferred);

        if (sent == 0 || sent > remaining)
        {
            fail(sent == 0 ? WSAECONNRESET : WSAEINVAL);
            return;
        }

        pending.offset += sent;
        queuedWriteBytes_ -= sent;

        if (pending.offset < pending.data->size())
        {
            updateReadBackpressure();
            postWrite();
            return;
        }

        auto callback = std::move(pending.callback);
        writes_.pop_front();

        if (callback) callback();

        // A write callback may enqueue more data, so evaluate the queue only
        // after it has returned.
        updateReadBackpressure();
        if (!closed_ && !loop_->isStopping()) postWrite();

        (void)operation;
    }

    void TcpConnection::fail(int error)
    {
        const std::error_code code(error, std::system_category());
        closeReason_ = CloseReason::Error;
        auto callback = onError_;
        close();
        if (callback) callback(code);
    }

    // Finalize only after every overlapped operation has completed.
    void TcpConnection::finishCloseIfDrained()
    {
        if (!closed_ || pendingOperations_ != 0) return;

        state_ = State::Closed;

        if (handlerRegistered_ && loop_ != nullptr)
        {
            loop_->unregisterCompletionHandler(
                reinterpret_cast<ULONG_PTR>(this));
            handlerRegistered_ = false;
        }

        if (!closeNotified_)
        {
            closeNotified_ = true;
            auto callback = std::move(onClose_);
            onData_ = {};
            onError_ = {};
            if (callback) callback();
        }
    }

    // Store the AcceptEx OVERLAPPED structure and address buffer together.
    struct TcpServer::AcceptOperation
    {
        OVERLAPPED overlapped{};
        SOCKET acceptedSocket = INVALID_SOCKET;

        static constexpr DWORD addressLength =
            static_cast<DWORD>(sizeof(sockaddr_in) + 16);

        std::array<char, addressLength * 2> addressBuffer{};
    };

    // Keep the accept completion dispatchable after TcpServer itself is destroyed.
    struct TcpServer::CompletionLifetime
    {
        TcpServer *owner = nullptr;
        Loop *loop = nullptr;
        ULONG_PTR completionKey = 0;
        AcceptOperation *operation = nullptr;
        bool detached = false;

        void dispatch(const Completion &completion)
        {
            if (owner != nullptr)
            {
                owner->handleAcceptCompletion(completion);
                return;
            }

            if (!detached || loop == nullptr || operation == nullptr ||
                completion.overlapped != &operation->overlapped)
            {
                return;
            }

            std::unique_ptr<AcceptOperation> completed(operation);
            operation = nullptr;

            if (completed->acceptedSocket != INVALID_SOCKET)
            {
                closesocket(completed->acceptedSocket);
                completed->acceptedSocket = INVALID_SOCKET;
            }

            loop->endOperation();
            loop->unregisterCompletionHandler(completionKey);
        }
    };

    // Create an overlapped IPv4 TCP listening socket.
    TcpServer::TcpServer()
        : completionLifetime_(std::make_shared<CompletionLifetime>())
    {
        socket_ = WSASocketW(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            nullptr,
            0,
            WSA_FLAG_OVERLAPPED);

        if (socket_ == INVALID_SOCKET)
        {
            throw std::system_error(
                WSAGetLastError(),
                std::system_category(),
                "Failed to create TCP socket");
        }

        completionLifetime_->owner = this;
    }

    // Cancel pending accepts without freeing memory still owned by Windows.
    TcpServer::~TcpServer()
    {
        stopAccepting();

        completionLifetime_->owner = nullptr;

        if (pendingAccept_ != nullptr)
        {
            // The registered completion callback retains CompletionLifetime.
            // It will release the OVERLAPPED operation and decrement the loop's
            // pending-operation count after Windows reports completion.
            completionLifetime_->loop = loop_;
            completionLifetime_->completionKey = reinterpret_cast<ULONG_PTR>(this);
            completionLifetime_->operation = pendingAccept_;
            completionLifetime_->detached = true;
            pendingAccept_ = nullptr;
            loop_ = nullptr;
        }

        if (socket_ != INVALID_SOCKET)
        {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    void TcpServer::listen(unsigned short port)
    {
        sockaddr_in address{};

        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(
                socket_,
                reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) == SOCKET_ERROR)
        {
            throw std::system_error(
                WSAGetLastError(),
                std::system_category(),
                "Failed to bind TCP socket");
        }

        if (::listen(socket_, SOMAXCONN) == SOCKET_ERROR)
        {
            throw std::system_error(
                WSAGetLastError(),
                std::system_category(),
                "Failed to listen on TCP socket");
        }
    }

    // Load AcceptEx and connect the listener to the event loop.
    void TcpServer::startAccepting(
        Loop &loop,
        ConnectionCallback onConnection,
        ErrorCallback onError)
    {
        if (socket_ == INVALID_SOCKET)
        {
            throw std::logic_error("TCP server socket is not initialized");
        }

        if (accepting_)
        {
            throw std::logic_error("TCP server is already accepting connections");
        }

        if (!onConnection)
        {
            throw std::invalid_argument("Connection callback cannot be empty");
        }

        GUID acceptExGuid = WSAID_ACCEPTEX;
        DWORD bytesReturned = 0;

        if (WSAIoctl(
                socket_,
                SIO_GET_EXTENSION_FUNCTION_POINTER,
                &acceptExGuid,
                sizeof(acceptExGuid),
                &acceptEx_,
                sizeof(acceptEx_),
                &bytesReturned,
                nullptr,
                nullptr) == SOCKET_ERROR)
        {
            throw std::system_error(
                WSAGetLastError(),
                std::system_category(),
                "Failed to load AcceptEx");
        }

        loop_ = &loop;
        completionLifetime_->owner = this;
        completionLifetime_->loop = &loop;
        completionLifetime_->completionKey = reinterpret_cast<ULONG_PTR>(this);
        completionLifetime_->detached = false;
        onConnection_ = std::move(onConnection);
        onError_ = std::move(onError);

        const auto completionKey = reinterpret_cast<ULONG_PTR>(this);

        try
        {
            loop_->associateHandle(
                reinterpret_cast<HANDLE>(socket_),
                completionKey);

            const auto lifetime = completionLifetime_;
            loop_->registerCompletionHandler(
                completionKey,
                [lifetime](const Completion &completion)
                {
                    lifetime->dispatch(completion);
                });

            accepting_ = true;
            postAccept();
        }
        catch (...)
        {
            accepting_ = false;
            loop_->unregisterCompletionHandler(completionKey);
            loop_ = nullptr;
            throw;
        }
    }

    // Cancel the pending accept while preserving its completion cleanup path.
    void TcpServer::stopAccepting()
    {
        accepting_ = false;

        if (pendingAccept_ != nullptr && loop_ != nullptr)
        {
            // Keep the completion handler registered. It owns cleanup of the
            // pending AcceptOperation after Windows reports cancellation.
            CancelIoEx(
                reinterpret_cast<HANDLE>(socket_),
                &pendingAccept_->overlapped);
            return;
        }

        if (loop_ != nullptr)
        {
            loop_->unregisterCompletionHandler(
                reinterpret_cast<ULONG_PTR>(this));
            loop_ = nullptr;
        }
    }

    // Submit one AcceptEx request while the server is accepting.
    void TcpServer::postAccept()
    {
        if (!accepting_ || loop_ == nullptr || loop_->isStopping() ||
            pendingAccept_ != nullptr)
        {
            return;
        }

        auto operation = std::make_unique<AcceptOperation>();

        operation->acceptedSocket = WSASocketW(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            nullptr,
            0,
            WSA_FLAG_OVERLAPPED);

        if (operation->acceptedSocket == INVALID_SOCKET)
        {
            throw std::system_error(
                WSAGetLastError(),
                std::system_category(),
                "Failed to create accepted TCP socket");
        }

        if (!loop_->beginOperation())
        {
            closesocket(operation->acceptedSocket);
            return;
        }

        DWORD bytesReceived = 0;

        // Transfer ownership before submission: a completion may be dequeued
        // immediately by another thread after AcceptEx queues its packet.
        AcceptOperation *submittedOperation = operation.release();
        pendingAccept_ = submittedOperation;
        completionLifetime_->operation = submittedOperation;

        // AcceptEx completes asynchronously and returns its result through IOCP.
        const BOOL result = acceptEx_(
            socket_,
            submittedOperation->acceptedSocket,
            submittedOperation->addressBuffer.data(),
            0,
            AcceptOperation::addressLength,
            AcceptOperation::addressLength,
            &bytesReceived,
            &submittedOperation->overlapped);

        if (!result)
        {
            const int error = WSAGetLastError();

            if (error != WSA_IO_PENDING)
            {
                pendingAccept_ = nullptr;
                completionLifetime_->operation = nullptr;
                std::unique_ptr<AcceptOperation> failedOperation(submittedOperation);
                closesocket(failedOperation->acceptedSocket);
                failedOperation->acceptedSocket = INVALID_SOCKET;
                loop_->endOperation();

                throw std::system_error(
                    error,
                    std::system_category(),
                    "AcceptEx failed to start");
            }
        }
    }

    void TcpServer::handleAcceptCompletion(const Completion &completion)
    {
        auto *operation =
            reinterpret_cast<AcceptOperation *>(completion.overlapped);

        if (operation == nullptr || operation != pendingAccept_)
        {
            return;
        }

        pendingAccept_ = nullptr;
        completionLifetime_->operation = nullptr;
        const auto lifetime = completionLifetime_;
        std::unique_ptr<AcceptOperation> operationGuard(operation);
        loop_->endOperation();

        if (!accepting_)
        {
            if (operation->acceptedSocket != INVALID_SOCKET)
            {
                closesocket(operation->acceptedSocket);
                operation->acceptedSocket = INVALID_SOCKET;
            }

            if (loop_ != nullptr)
            {
                loop_->unregisterCompletionHandler(
                    reinterpret_cast<ULONG_PTR>(this));
                loop_ = nullptr;
            }
            return;
        }

        if (!completion.succeeded())
        {
            closesocket(operation->acceptedSocket);
            operation->acceptedSocket = INVALID_SOCKET;

            if (onError_)
            {
                const std::error_code error(
                    static_cast<int>(completion.error),
                    std::system_category());
                auto callback = onError_;
                try
                {
                    callback(error);
                }
                catch (...)
                {
                    if (lifetime->owner == this) stopAccepting();
                    throw;
                }
            }

            if (lifetime->owner != this) return;

            if (accepting_ && loop_ != nullptr && !loop_->isStopping())
            {
                postAccept();
            }

            return;
        }

        if (setsockopt(
                operation->acceptedSocket,
                SOL_SOCKET,
                SO_UPDATE_ACCEPT_CONTEXT,
                reinterpret_cast<const char *>(&socket_),
                sizeof(socket_)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            closesocket(operation->acceptedSocket);
            operation->acceptedSocket = INVALID_SOCKET;

            if (onError_)
            {
                const std::error_code code(error, std::system_category());
                auto callback = onError_;
                try
                {
                    callback(code);
                }
                catch (...)
                {
                    if (lifetime->owner == this) stopAccepting();
                    throw;
                }
            }

            if (lifetime->owner != this) return;

            if (accepting_ && loop_ != nullptr && !loop_->isStopping())
            {
                postAccept();
            }

            return;
        }

        const SOCKET acceptedSocket = operation->acceptedSocket;
        operation->acceptedSocket = INVALID_SOCKET;

        try
        {
            auto callback = onConnection_;
            callback(acceptedSocket);
        }
        catch (...)
        {
            closesocket(acceptedSocket);
            if (lifetime->owner == this) stopAccepting();
            throw;
        }

        // The callback is allowed to destroy the TcpServer wrapper.
        if (lifetime->owner != this) return;

        if (accepting_ && loop_ != nullptr && !loop_->isStopping())
        {
            postAccept();
        }
    }

}
