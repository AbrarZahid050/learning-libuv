#include "cppuv/winsock.hpp"
#include "cppuv/tcp.hpp"
#include "cppuv/loop.hpp"
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

int main()
{
    try
    {
        cppuv::Winsock winsock;

        std::cout << "Winsock initialized!\n";

        cppuv::Loop loop;
        cppuv::TcpServer server;

        // Keep each connection alive while its overlapped operations are active.
        // This simple demo retains closed connections until the process exits.
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

                        client.write(
                            std::string(data),
                            [&client, clientId]()
                            {
                                std::cout << "[client " << clientId
                                          << "] echo sent\n";
                                client.resumeRead();
                            });
                    },
                    [clientId]()
                    {
                        std::cout << "[client " << clientId
                                  << "] disconnected\n";
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
        loop.run();

        std::cout << "Event loop stopped.\n";
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
