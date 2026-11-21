#include "cppuv/winsock.hpp"
#include "cppuv/tcp.hpp"
#include "cppuv/loop.hpp"
#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <thread>
#include <windows.h>

namespace
{
    HANDLE shutdownEvent = nullptr;

    BOOL WINAPI consoleControlHandler(DWORD control)
    {
        if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT)
        {
            if (shutdownEvent != nullptr) SetEvent(shutdownEvent);
            return TRUE;
        }
        return FALSE;
    }
}

int main()
{
    try
    {
        cppuv::Winsock winsock;

        std::cout << "Winsock initialized!\n";

        cppuv::Loop loop;
        cppuv::TcpServer server;

        // Each pending I/O operation retains its connection. The close callback
        // removes the application's owning shared_ptr after all I/O has drained.
        std::vector<std::shared_ptr<cppuv::TcpConnection>> connections;
        std::size_t nextClientId = 1;

        server.listen(8080);

        std::cout << "TCP echo server listening on 127.0.0.1:8080\n";
        std::cout << "Supports multiple clients. Press Ctrl+C to stop.\n";

        server.startAccepting(
            loop,
            [&loop, &connections, &nextClientId](SOCKET clientSocket)
            {
                const auto clientId = nextClientId++;

                std::cout << "[client " << clientId
                          << "] connected (socket " << clientSocket << ")\n";

                auto connection =
                    std::make_shared<cppuv::TcpConnection>(loop, clientSocket);
                connections.push_back(connection);

                connection->start(
                    [clientId](
                        cppuv::TcpConnection &client,
                        std::string_view data)
                    {
                        std::cout << "[client " << clientId
                                  << "] received " << data.size()
                                  << " bytes: " << data << '\n';

                        // Serialize this demo's read/write cycle. It keeps
                        // one operation in flight per connection at a time.
                        client.pauseRead();

                        auto *connection = &client;
                        client.write(
                            std::string(data),
                            [connection, clientId]()
                            {
                                std::cout << "[client " << clientId
                                          << "] echo sent\n";
                                connection->resumeRead();
                            });
                    },
                    [&connections, connectionPtr = connection.get(), clientId]()
                    {
                        std::cout << "[client " << clientId
                                  << "] disconnected\n";

                        // The close callback runs only after all overlapped
                        // operations have completed, so removing the owning
                        // shared_ptr here is safe.
                        connections.erase(
                            std::remove_if(
                                connections.begin(),
                                connections.end(),
                                [connectionPtr](const auto &item)
                                {
                                    return item.get() == connectionPtr;
                                }),
                            connections.end());
                    },
                    [clientId](const std::error_code &error)
                    {
                        std::cerr << "[client " << clientId
                                  << "] connection error: "
                                  << error.message() << '\n';
                    });
            },
            [](const std::error_code &error)
            {
                std::cerr << "Accept error: " << error.message() << '\n';
            });

        std::cout << "Waiting for client connections...\n";

        shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (shutdownEvent == nullptr)
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Failed to create shutdown event");
        }

        if (!SetConsoleCtrlHandler(consoleControlHandler, TRUE))
        {
            const DWORD error = GetLastError();
            CloseHandle(shutdownEvent);
            shutdownEvent = nullptr;
            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                "Failed to register console control handler");
        }

        // The console handler only signals an event. This worker transfers
        // shutdown onto the event-loop thread, where socket APIs are used.
        std::thread shutdownWatcher([&loop, &server, &connections]()
        {
            WaitForSingleObject(shutdownEvent, INFINITE);

            loop.post([&loop, &server, &connections]()
            {
                std::cout << "\\nShutdown requested. Closing listener and clients...\\n";

                server.stopAccepting();

                // close() callbacks remove entries from connections, so iterate
                // over a snapshot rather than the vector being modified.
                const auto activeConnections = connections;
                for (const auto &connection : activeConnections)
                {
                    connection->close();
                }

                // run() now waits for all cancellation completions before exit.
                loop.stop();
            });
        });

        loop.run();
        shutdownWatcher.join();

        SetConsoleCtrlHandler(consoleControlHandler, FALSE);
        CloseHandle(shutdownEvent);
        shutdownEvent = nullptr;

        std::cout << "All pending I/O has drained. Event loop stopped.\\n";
    }
    catch (const std::system_error &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
