#include "cppuv/tcp.hpp"
#include <ws2tcpip.h>
#include <system_error>

namespace cppuv
{

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

}