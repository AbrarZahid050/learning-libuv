#include "cppuv/poller.hpp"
#include <system_error>

namespace cppuv
{

    Poller::Poller()
    {
        completionPort_ = CreateIoCompletionPort(
            INVALID_HANDLE_VALUE,
            nullptr,
            0,
            1);

        if (completionPort_ == nullptr)
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Failed to create IOCP");
        }
    }

    Poller::~Poller()
    {
        if (completionPort_ != nullptr)
        {
            CloseHandle(completionPort_);
        }
    }

    void Poller::wake()
    {
        if (!PostQueuedCompletionStatus(
                completionPort_,
                0,
                0,
                nullptr))
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Failed to wake IOCP");
        }
    }

    void Poller::associate(
        HANDLE handle,
        ULONG_PTR completionKey)
    {
        HANDLE result = CreateIoCompletionPort(
            handle,
            completionPort_,
            completionKey,
            0);

        if (result == nullptr)
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Failed to associate handle with IOCP");
        }
    }

    std::optional<Completion> Poller::wait(DWORD timeoutMs)
    {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED *overlapped = nullptr;

        const BOOL result = GetQueuedCompletionStatus(
            completionPort_,
            &bytesTransferred,
            &completionKey,
            &overlapped,
            timeoutMs);

        if (!result)
        {
            const DWORD error = GetLastError();

            // No completion arrived before the timeout.
            if (overlapped == nullptr && error == WAIT_TIMEOUT)
            {
                return std::nullopt;
            }

            // An overlapped operation completed with an error.
            // This is still a valid completion to dispatch.
            if (overlapped != nullptr)
            {
                return Completion{
                    bytesTransferred,
                    completionKey,
                    overlapped,
                    error};
            }

            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                "IOCP wait failed");
        }

        return Completion{
            bytesTransferred,
            completionKey,
            overlapped,
            ERROR_SUCCESS};
    }

}
