#pragma once

#include <winsock2.h>

namespace cppuv
{

    class TcpServer
    {
    public:
        TcpServer();
        ~TcpServer();

        TcpServer(const TcpServer &) = delete;
        TcpServer &operator=(const TcpServer &) = delete;

        void listen(unsigned short port);

    private:
        SOCKET socket_ = INVALID_SOCKET;
    };

}