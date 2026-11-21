#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <optional>

namespace cppuv
{

    struct Completion
    {
        DWORD bytesTransferred;
        ULONG_PTR key;
        OVERLAPPED *overlapped;
        DWORD error;

        bool succeeded() const
        {
            return error == ERROR_SUCCESS;
        }

        bool isWake() const
        {
            return overlapped == nullptr && key == 0;
        }
    };

    class Poller
    {
    public:
        Poller();
        ~Poller();

        Poller(const Poller &) = delete;
        Poller &operator=(const Poller &) = delete;

        void wake();

        void associate(
            HANDLE handle,
            ULONG_PTR completionKey);

        std::optional<Completion> wait(DWORD timeoutMs);

    private:
        HANDLE completionPort_ = nullptr;
    };

}
