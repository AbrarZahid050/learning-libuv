#include "cppuv/tcp.hpp"
#include <algorithm>
#include <array>
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

        if (!onData)
        {
            throw std::invalid_argument("TCP data callback cannot be empty");
        }

        onData_ = std::move(onData);
        onClose_ = std::move(onClose);
        onError_ = std::move(onError);

        const auto completionKey = reinterpret_cast<ULONG_PTR>(this);

        loop_->associateHandle(
            reinterpret_cast<HANDLE>(socket_),
            completionKey);

        loop_->registerCompletionHandler(
            completionKey,
            [this](const Completion &completion)
            {
                handleCompletion(completion);
            });

        started_ = true;

        try
        {
            postRead();
        }
        catch (...)
        {
            loop_->unregisterCompletionHandler(completionKey);
            started_ = false;
            throw;
        }
    }

    void TcpConnection::write(std::string data, WriteCallback onWritten)
    {
        if (closed_)
        {
            throw std::logic_error("Cannot write to a closed TCP connection");
        }

        if (data.empty())
        {
            if (onWritten)
            {
                onWritten();
            }
            return;
        }

        writes_.push_back(std::move(data));
        writeCallbacks_.push_back(std::move(onWritten));

        if (!writing_)
        {
            postWrite();
        }
    }

    void TcpConnection::pauseRead()
    {
        readPaused_ = true;
    }

    void TcpConnection::resumeRead()
    {
        if (closed_)
        {
            return;
        }

        readPaused_ = false;
        postRead();
    }

    void TcpConnection::close()
    {
        if (closed_)
        {
            return;
        }

        closed_ = true;

        if (started_ && loop_ != nullptr)
        {
            loop_->unregisterCompletionHandler(
                reinterpret_cast<ULONG_PTR>(this));
        }

        if (socket_ != INVALID_SOCKET)
        {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }

        writes_.clear();
        writeCallbacks_.clear();
        onData_ = {};
        onClose_ = {};
        onError_ = {};
    }

    void TcpConnection::postRead()
    {
        if (closed_ || reading_ || readPaused_ || loop_->isStopping())
        {
            return;
        }

        readOperation_.overlapped = {};
        readOperation_.buffer.buf = readOperation_.data.data();
        readOperation_.buffer.len =
            static_cast<ULONG>(readOperation_.data.size());

        DWORD flags = 0;
        DWORD bytesReceived = 0;

        reading_ = true;

        const int result = WSARecv(
            socket_,
            &readOperation_.buffer,
            1,
            &bytesReceived,
            &flags,
            &readOperation_.overlapped,
            nullptr);

        if (result == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();

            if (error != WSA_IO_PENDING)
            {
                reading_ = false;
                fail(error, "WSARecv failed");
            }
        }
    }

    void TcpConnection::postWrite()
    {
        if (closed_ || writing_ || writes_.empty() || loop_->isStopping())
        {
            return;
        }

        auto &front = writes_.front();
        const auto remaining = front.size() - writeOffset_;

        writeOperation_.overlapped = {};
        writeOperation_.buffer.buf = front.data() + writeOffset_;
        writeOperation_.buffer.len = static_cast<ULONG>(
            std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));

        DWORD bytesSent = 0;
        writing_ = true;

        const int result = WSASend(
            socket_,
            &writeOperation_.buffer,
            1,
            &bytesSent,
            0,
            &writeOperation_.overlapped,
            nullptr);

        if (result == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();

            if (error != WSA_IO_PENDING)
            {
                writing_ = false;
                fail(error, "WSASend failed");
            }
        }
    }

    void TcpConnection::handleCompletion(const Completion &completion)
    {
        if (closed_ || completion.overlapped == nullptr)
        {
            return;
        }

        if (completion.overlapped == &readOperation_.overlapped)
        {
            handleRead(completion);
        }
        else if (completion.overlapped == &writeOperation_.overlapped)
        {
            handleWrite(completion);
        }
    }

    void TcpConnection::handleRead(const Completion &completion)
    {
        reading_ = false;

        if (!completion.succeeded())
        {
            fail(static_cast<int>(completion.error), "TCP read failed");
            return;
        }

        if (completion.bytesTransferred == 0)
        {
            auto callback = onClose_;
            close();

            if (callback)
            {
                callback();
            }
            return;
        }

        const std::string_view data(
            readOperation_.data.data(),
            completion.bytesTransferred);

        if (onData_)
        {
            onData_(*this, data);
        }

        if (!closed_ && !readPaused_ && !loop_->isStopping())
        {
            postRead();
        }
    }

    void TcpConnection::handleWrite(const Completion &completion)
    {
        writing_ = false;

        if (!completion.succeeded())
        {
            fail(static_cast<int>(completion.error), "TCP write failed");
            return;
        }

        if (writes_.empty())
        {
            return;
        }

        const auto remaining = writes_.front().size() - writeOffset_;
        const auto sent = static_cast<std::size_t>(completion.bytesTransferred);

        if (sent == 0)
        {
            fail(WSAECONNRESET, "TCP write completed without sending data");
            return;
        }

        if (sent > remaining)
        {
            fail(WSAEINVAL, "TCP write completion exceeded the pending buffer");
            return;
        }

        writeOffset_ += sent;

        if (writeOffset_ < writes_.front().size())
        {
            postWrite();
            return;
        }

        writes_.pop_front();
        writeOffset_ = 0;

        auto callback = std::move(writeCallbacks_.front());
        writeCallbacks_.pop_front();

        if (callback)
        {
            callback();
        }

        if (!closed_ && !loop_->isStopping())
        {
            postWrite();
        }
    }

    void TcpConnection::fail(int error, const char *operation)
    {
        auto callback = onError_;
        const std::error_code code(error, std::system_category());

        close();

        if (callback)
        {
            callback(code);
        }

        (void)operation;
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
            CancelIoEx(
                reinterpret_cast<HANDLE>(socket_),
                &pendingAccept_->overlapped);
            pendingAccept_ = nullptr;
        }

        if (socket_ != INVALID_SOCKET)
        {
            closesocket(socket_);
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

        if (loop_ != nullptr)
        {
            loop_->unregisterCompletionHandler(
                reinterpret_cast<ULONG_PTR>(this));
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
