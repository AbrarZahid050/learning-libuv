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
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace cppuv
{

    class TcpConnection : public std::enable_shared_from_this<TcpConnection>
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

        // TcpConnection must be owned by std::shared_ptr before start().
        void start(
            DataCallback onData,
            CloseCallback onClose = {},
            ErrorCallback onError = {});

        void write(std::string data, WriteCallback onWritten = {});
        void pauseRead();
        void resumeRead();
        void close();

        [[nodiscard]] SOCKET nativeHandle() const noexcept
        {
            return socket_;
        }

        [[nodiscard]] bool isClosed() const noexcept
        {
            return closed_;
        }

    private:
        struct IoOperation
        {
            enum class Kind { Read, Write };

            explicit IoOperation(Kind operationKind) : kind(operationKind) {}

            OVERLAPPED overlapped{};
            WSABUF buffer{};
            Kind kind;
            std::array<char, 8192> readData{};
            std::shared_ptr<std::string> writeData;
            std::shared_ptr<TcpConnection> keepAlive;
            std::size_t writeOffset = 0;
        };

        struct PendingWrite
        {
            std::shared_ptr<std::string> data;
            WriteCallback callback;
            std::size_t offset = 0;
        };

        void postRead();
        void postWrite();
        void handleCompletion(const Completion &completion);
        void handleRead(IoOperation &operation, const Completion &completion);
        void handleWrite(IoOperation &operation, const Completion &completion);
        void fail(int error);
        void finishCloseIfDrained();

        Loop *loop_;
        SOCKET socket_;
        std::deque<PendingWrite> writes_;
        DataCallback onData_;
        CloseCallback onClose_;
        ErrorCallback onError_;
        std::size_t pendingOperations_ = 0;
        bool started_ = false;
        bool reading_ = false;
        bool readPaused_ = false;
        bool writing_ = false;
        bool closed_ = false;
        bool handlerRegistered_ = false;
        bool closeNotified_ = false;
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

        // Stops accepting new clients. A pending AcceptEx is cancelled and
        // its completion is consumed by the event loop.
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
