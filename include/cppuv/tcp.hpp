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
#include <array>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

namespace cppuv
{

    class TcpConnection
    {
    public:
        using DataCallback =
            std::function<void(TcpConnection &, std::string_view)>;
        using WriteCallback = std::function<void()>;
        using CloseCallback = std::function<void()>;
        using ErrorCallback =
            std::function<void(const std::error_code &)>;

        TcpConnection(Loop &loop, SOCKET socket);
        ~TcpConnection();

        TcpConnection(const TcpConnection &) = delete;
        TcpConnection &operator=(const TcpConnection &) = delete;

        void start(
            DataCallback onData,
            CloseCallback onClose = {},
            ErrorCallback onError = {});

        void write(std::string data, WriteCallback onWritten = {});
        void close();

        [[nodiscard]] SOCKET nativeHandle() const noexcept
        {
            return socket_;
        }

    private:
        struct ReadOperation
        {
            OVERLAPPED overlapped{};
            WSABUF buffer{};
            std::array<char, 8192> data{};
        };

        struct WriteOperation
        {
            OVERLAPPED overlapped{};
            WSABUF buffer{};
        };

        void postRead();
        void postWrite();
        void handleCompletion(const Completion &completion);
        void handleRead(const Completion &completion);
        void handleWrite(const Completion &completion);
        void fail(int error, const char *operation);

        Loop *loop_;
        SOCKET socket_;
        ReadOperation readOperation_{};
        WriteOperation writeOperation_{};
        std::deque<std::string> writes_;
        std::deque<WriteCallback> writeCallbacks_;
        std::size_t writeOffset_ = 0;
        DataCallback onData_;
        CloseCallback onClose_;
        ErrorCallback onError_;
        bool started_ = false;
        bool reading_ = false;
        bool writing_ = false;
        bool closed_ = false;
    };

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

        // The callback takes ownership of each accepted socket.
        void startAccepting(
            Loop &loop,
            ConnectionCallback onConnection,
            ErrorCallback onError = {});

        // Stops accepting. Call from the connection callback to prevent
        // another accept from being posted.
        void stopAccepting();

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
