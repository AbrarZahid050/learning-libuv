#include "cppuv/winsock.hpp"
#include "cppuv/tcp.hpp"
#include "cppuv/loop.hpp"
#include <exception>
#include <iostream>
#include <system_error>

int main()
{
    try
    {
        cppuv::Winsock winsock;

        std::cout << "Winsock initialized!\n";

        cppuv::Loop loop;

        cppuv::TcpServer server;
        server.listen(8080);

        std::cout << "TCP server listening on 127.0.0.1:8080\n";

        server.startAccepting(
            loop,
            [&loop](SOCKET clientSocket)
            {
                std::cout << "Client connected! Socket: "
                          << clientSocket << '\n';

                // This example closes the first client and stops the loop.
                // A real server would create a TcpConnection and keep it.
                closesocket(clientSocket);
                loop.stop();
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