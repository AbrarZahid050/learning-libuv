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
        std::vector<std::shared_ptr<cppuv::TcpConnection>> connections;

        server.listen(8080);

        std::cout << "TCP echo server listening on 127.0.0.1:8080\n";

        server.startAccepting(
            loop,
            [&loop, &server, &connections](SOCKET clientSocket)
            {
                std::cout << "Client connected! Socket: "
                          << clientSocket << '\n';

                // Keep the connection alive for the duration of the loop.
                auto connection =
                    std::make_shared<cppuv::TcpConnection>(loop, clientSocket);
                connections.push_back(connection);

                // This demo handles one client, then stops cleanly.
                server.stopAccepting();

                connection->start(
                    [&loop](
                        cppuv::TcpConnection &client,
                        std::string_view data)
                    {
                        std::cout << "Received: " << data << '\n';

                        // Pause reads so this one-shot demo has no outstanding
                        // read when the write completion stops the event loop.
                        client.pauseRead();

                        client.write(
                            std::string(data),
                            [&loop]()
                            {
                                std::cout << "Echo sent successfully.\n";
                                loop.stop();
                            });
                    },
                    []()
                    {
                        std::cout << "Client disconnected.\n";
                    },
                    [](const std::error_code &error)
                    {
                        std::cerr << "Connection error: "
                                  << error.message() << '\n';
                    });
            },
            [](const std::error_code &error)
            {
                std::cerr << "Accept error: " << error.message() << '\n';
            });

        std::cout << "Waiting for a client connection...\n";
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
