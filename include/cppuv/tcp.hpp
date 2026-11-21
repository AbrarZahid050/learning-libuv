#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <mswsock.h>

#include "cppuv/loop.hpp"
#include <functional>
#include <system_error>

namespace cppuv
{

    class TcpServer
    {
    public:
        using ConnectionCallback = std::function<void(SOCKET)>;
        using ErrorCallback = std::function<void(const std::error_code &)>;

        TcpServer();
        ~TcpServer();

        TcpServer(const TcpServer &) = delete;
        TcpServer &operator=(const TcpServer &) = delete;

        void listen(unsigned short port);

        // Starts continuous asynchronous accepting. The connection callback
        // takes ownership of each accepted socket and must eventually close it.
        void startAccepting(
            Loop &loop,
            ConnectionCallback onConnection,
            ErrorCallback onError = {});

        [[nodiscard]] SOCKET nativeHandle() const noexcept
        {
            return socket_;
        }

    private:
        struct AcceptOperation;

        void postAccept();
        void handleAcceptCompletion(const Completion &completion);

        SOCKET socket_ = INVALID_SOCKET;
        LPFN_ACCEPTEX acceptEx_ = nullptr;
        Loop *loop_ = nullptr;
        ConnectionCallback onConnection_;
        ErrorCallback onError_;
        AcceptOperation *pendingAccept_ = nullptr;
        bool accepting_ = false;
    };

}