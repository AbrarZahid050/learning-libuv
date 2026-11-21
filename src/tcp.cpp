#include "cppuv/tcp.hpp"
#include <array>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <ws2tcpip.h>

namespace cppuv
{

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
        accepting_ = false;

        if (pendingAccept_ != nullptr)
        {
            // Keep the OVERLAPPED storage alive until Windows finishes the
            // cancelled operation. Normal shutdown in the example stops the
            // loop from the connection callback, when no accept is pending.
            CancelIoEx(
                reinterpret_cast<HANDLE>(socket_),
                &pendingAccept_->overlapped);
            pendingAccept_ = nullptr;
        }

        if (loop_ != nullptr)
        {
            loop_->unregisterCompletionHandler(
                reinterpret_cast<ULONG_PTR>(this));
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
            // Ownership of acceptedSocket is transferred to the callback.
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