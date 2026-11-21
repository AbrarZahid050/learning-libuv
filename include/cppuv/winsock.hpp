#pragma once

namespace cppuv
{

    class Winsock
    {
    public:
        Winsock();
        ~Winsock();

        Winsock(const Winsock &) = delete;
        Winsock &operator=(const Winsock &) = delete;
    };

}