#include "cppuv/tcp.hpp"
#include <algorithm>
#include <memory>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <ws2tcpip.h>

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

    TcpConnection::~TcpConnection()
    {
        close();
    }

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
        postRead();
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

    void TcpConnection::pauseRead()
    {
        readPaused_ = true;
    }

    void TcpConnection::resumeRead()
    {
        if (closed_) return;
        readPaused_ = false;
        postRead();
    }

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

    void TcpConnection::close()
    {
        if (closed_) return;

        closed_ = true;

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

    void TcpConnection::postRead()
    {
        if (closed_ || reading_ || readPaused_ || backpressurePaused_ ||
            loop_->isStopping()) return;

        auto operation = std::make_unique<IoOperation>(IoOperation::Kind::Read);
        operation->keepAlive = shared_from_this();
        operation->buffer.buf = operation->readData.data();
        operation->buffer.len = static_cast<ULONG>(operation->readData.size());

        if (!loop_->beginOperation()) return;

        DWORD flags = 0;
        DWORD bytesReceived = 0;
        reading_ = true;
        ++pendingOperations_;

        const int result = WSARecv(
            socket_,
            &operation->buffer,
            1,
            &bytesReceived,
            &flags,
            &operation->overlapped,
            nullptr);

        if (result == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error != WSA_IO_PENDING)
            {
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

        const int result = WSASend(
            socket_,
            &operation->buffer,
            1,
            &bytesSent,
            0,
            &operation->overlapped,
            nullptr);

        if (result == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error != WSA_IO_PENDING)
            {
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

        std::unique_ptr<IoOperation> operation(
            reinterpret_cast<IoOperation *>(completion.overlapped));

        try
        {
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
            finishCloseIfDrained();
            throw;
        }

        if (pendingOperations_ > 0) --pendingOperations_;
        loop_->endOperation();
        finishCloseIfDrained();
    }

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
            close();
            return;
        }

        const std::string_view data(
            operation.readData.data(),
            completion.bytesTransferred);

        if (onData_) onData_(*this, data);

        if (!closed_ && !readPaused_ && !loop_->isStopping()) postRead();
    }

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
        auto callback = onError_;
        close();
        if (callback) callback(code);
    }

    void TcpConnection::finishCloseIfDrained()
    {
        if (!closed_ || pendingOperations_ != 0) return;

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

    struct TcpServer::AcceptOperation
    {
        OVERLAPPED overlapped{};
        SOCKET acceptedSocket = INVALID_SOCKET;

        static constexpr DWORD addressLength =
            static_cast<DWORD>(sizeof(sockaddr_in) + 16);

        std::array<char, addressLength * 2> addressBuffer{};
    };

    TcpServer::TcpServer()
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
    }

    TcpServer::~TcpServer()
    {
        stopAccepting();

        if (pendingAccept_ != nullptr)
        {
            // The loop is no longer guaranteed to dispatch the cancellation
            // completion during destruction. Keep the OVERLAPPED allocation
            // alive rather than freeing memory Windows may still reference.
            CancelIoEx(
                reinterpret_cast<HANDLE>(socket_),
                &pendingAccept_->overlapped);

            if (pendingAccept_->acceptedSocket != INVALID_SOCKET)
            {
                closesocket(pendingAccept_->acceptedSocket);
                pendingAccept_->acceptedSocket = INVALID_SOCKET;
            }

            if (loop_ != nullptr)
            {
                loop_->unregisterCompletionHandler(
                    reinterpret_cast<ULONG_PTR>(this));
                loop_ = nullptr;
            }

            pendingAccept_ = nullptr; // Deliberately orphaned until process exit.
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
        onConnection_ = std::move(onConnection);
        onError_ = std::move(onError);

        const auto completionKey = reinterpret_cast<ULONG_PTR>(this);

        try
        {
            loop_->associateHandle(
                reinterpret_cast<HANDLE>(socket_),
                completionKey);

            loop_->registerCompletionHandler(
                completionKey,
                [this](const Completion &completion)
                {
                    handleAcceptCompletion(completion);
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

        const BOOL result = acceptEx_(
            socket_,
            operation->acceptedSocket,
            operation->addressBuffer.data(),
            0,
            AcceptOperation::addressLength,
            AcceptOperation::addressLength,
            &bytesReceived,
            &operation->overlapped);

        if (!result)
        {
            const int error = WSAGetLastError();

            if (error != WSA_IO_PENDING)
            {
                closesocket(operation->acceptedSocket);
                loop_->endOperation();

                throw std::system_error(
                    error,
                    std::system_category(),
                    "AcceptEx failed to start");
            }
        }

        pendingAccept_ = operation.release();
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
                onError_(error);
            }

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
                onError_(code);
            }

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
            onConnection_(acceptedSocket);
        }
        catch (...)
        {
            closesocket(acceptedSocket);
            throw;
        }

        if (accepting_ && loop_ != nullptr && !loop_->isStopping())
        {
            postAccept();
        }
    }

}
