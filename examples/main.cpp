#include "cppuv/winsock.hpp" // Import the library API or Windows/C++ facility used below.
#include "cppuv/tcp.hpp" // Import the library API or Windows/C++ facility used below.
#include "cppuv/loop.hpp" // Import the library API or Windows/C++ facility used below.
#include <algorithm> // Import the library API or Windows/C++ facility used below.
#include <chrono> // Import the library API or Windows/C++ facility used below.
#include <exception> // Import the library API or Windows/C++ facility used below.
#include <iostream> // Import the library API or Windows/C++ facility used below.
#include <memory> // Import the library API or Windows/C++ facility used below.
#include <string> // Import the library API or Windows/C++ facility used below.
#include <system_error> // Import the library API or Windows/C++ facility used below.
#include <vector> // Import the library API or Windows/C++ facility used below.
#include <thread> // Import the library API or Windows/C++ facility used below.
#include <windows.h> // Import the library API or Windows/C++ facility used below.

namespace
{
    // Shared with the console-control callback and the shutdown watcher thread.
    HANDLE shutdownEvent = nullptr;

    // Keep the console callback minimal: signal the event and let the loop thread
    // perform all socket and connection shutdown work.
    BOOL WINAPI consoleControlHandler(DWORD control)
    {
        if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT)
        {
            if (shutdownEvent != nullptr)
            {
                SetEvent(shutdownEvent);
            }
            return TRUE;
        }

        return FALSE;
    }
}

int main()
{
    try
    {
        // Initialize Winsock before creating any sockets; its destructor calls WSACleanup.
        cppuv::Winsock winsock;

        std::cout << "Winsock initialized!\n";

        // The loop owns the IOCP dispatcher and must outlive all active connections.
        cppuv::Loop loop;
        // The server accepts sockets and hands each accepted socket to a TcpConnection.
        cppuv::TcpServer server;

        // Each pending I/O operation retains its connection. The close callback
        // removes the application's owning shared_ptr after all I/O has drained.
        std::vector<std::shared_ptr<cppuv::TcpConnection>> connections;
        std::size_t nextClientId = 1;

        // Bind the example server to 127.0.0.1:8080.
        server.listen(8080);

        std::cout << "TCP echo server listening on 127.0.0.1:8080\n";
        std::cout << "Supports multiple clients. Press Ctrl+C to stop.\n";

        // Accept clients asynchronously; each callback runs on the event-loop thread.
        server.startAccepting(
            loop,
            [&loop, &connections, &nextClientId](SOCKET clientSocket)
            {
                const auto clientId = nextClientId++;

                std::cout << "[client " << clientId
                          << "] connected (socket " << clientSocket
                          << ", state=CONNECTED)\n";

                // Wrap the accepted socket in a shared connection before starting I/O.
                auto connection =
                    std::make_shared<cppuv::TcpConnection>(loop, clientSocket);
                connections.push_back(connection);

                // Register receive, close, and error callbacks, then start the first read.
                connection->start(
                    [clientId](
                        cppuv::TcpConnection &client,
                        std::string_view data)
                    {
                        // Log the payload size, not its contents. Printing
                        // large buffers can dominate the time spent in this
                        // networking example and obscure I/O behaviour.
                        std::cout << "[client " << clientId
                                  << "] received " << data.size()
                                  << " bytes\n";

                        // Keep reading while a write is in flight. TcpConnection
                        // allows one outstanding read and one outstanding write
                        // at the same time, and queues additional writes in order.
                        // Echo the received bytes; TcpConnection queues and sends them asynchronously.
                        client.write(
                            std::string(data),
                            [clientId]()
                            {
                                std::cout << "[client " << clientId
                                          << "] echo sent\n";
                            });
                    },
                    [&connections, connectionPtr = connection.get(), clientId]()
                    {
                        std::cout << "[client " << clientId << "] ";
                        switch (connectionPtr->closeReason())
                        {
                        case cppuv::TcpConnection::CloseReason::Remote:
                            std::cout << "peer disconnected";
                            break;
                        case cppuv::TcpConnection::CloseReason::Local:
                            std::cout << "closed by server";
                            break;
                        case cppuv::TcpConnection::CloseReason::Error:
                            std::cout << "closed after socket error";
                            break;
                        case cppuv::TcpConnection::CloseReason::Timeout:
                            std::cout << "closed after idle timeout";
                            break;
                        case cppuv::TcpConnection::CloseReason::None:
                            std::cout << "closed";
                            break;
                        }
                        std::cout << " (state=CLOSED)\n";

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

                // Close clients that remain inactive for 10 seconds.
                // Close a client if it sends no inbound data for ten seconds.
                connection->setIdleTimeout(std::chrono::seconds(10));
            },
            [](const std::error_code &error)
            {
                std::cerr << "Accept error: " << error.message() << '\n';
            });

        std::cout << "Waiting for client connections...\n";

        // Create a manual-reset event used to pass console shutdown to a worker thread.
        shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (shutdownEvent == nullptr)
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Failed to create shutdown event");
        }

        // Install the Windows console handler only after the event exists.
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
        // Wait outside the event-loop thread, then post shutdown onto the loop.
        std::thread shutdownWatcher([&loop, &server, &connections]()
        {
            // Block this worker until the console handler signals shutdown.
            WaitForSingleObject(shutdownEvent, INFINITE);

            // post() safely transfers the shutdown sequence to the loop thread.
            loop.post([&loop, &server, &connections]()
            {
                std::cout << "\nShutdown requested. Closing listener and clients...\n";

                // Cancel the outstanding AcceptEx request before closing client connections.
                server.stopAccepting();

                // close() callbacks remove entries from connections, so iterate
                // over a snapshot rather than the vector being modified.
                // Use a snapshot because close callbacks erase entries from the original vector.
                const auto activeConnections = connections;
                for (const auto &connection : activeConnections)
                {
                    connection->close();
                }

                // run() now waits for all cancellation completions before exit.
                // The loop exits only after outstanding I/O cancellation completions drain.
                loop.stop();
            });
        });

        // This call blocks while tasks, timers, and IOCP completions are dispatched.
        loop.run();
        // Ensure the shutdown worker has finished before releasing its event handle.
        shutdownWatcher.join();

        // Unregister the console callback before cleaning up its shared event.
        SetConsoleCtrlHandler(consoleControlHandler, FALSE);
        CloseHandle(shutdownEvent);
        shutdownEvent = nullptr;

        std::cout << "All pending I/O has drained. Event loop stopped.\n";
    }
    // Report Windows/IOCP/Winsock failures using their standard error messages.
    catch (const std::system_error &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    // Also report validation and other standard C++ exceptions.
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    // Signal successful completion to the operating system.
    return 0;
}
