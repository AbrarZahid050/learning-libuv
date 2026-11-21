#include "cppuv/winsock.hpp"
#include <winsock2.h>
#include <system_error>

namespace cppuv
{

    Winsock::Winsock()
    {
        WSADATA data{};

        const int result = WSAStartup(
            MAKEWORD(2, 2),
            &data);

        if (result != 0)
        {
            throw std::system_error(
                result,
                std::system_category(),
                "Failed to initialize Winsock");
        }
    }

    Winsock::~Winsock()
    {
        WSACleanup();
    }

}