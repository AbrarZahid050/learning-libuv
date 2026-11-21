#include "cppuv/winsock.hpp"
#include "cppuv/tcp.hpp"
#include "cppuv/loop.hpp"
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

        std::cout << "TCP server listening on port 8080!\n";

        // Associate the TCP socket with our IOCP.
        loop.associateHandle(server.nativeHandle(), 1);

        std::cout << "TCP socket associated with IOCP!\n";
    }
    catch (const std::system_error &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}