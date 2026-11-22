#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <type_traits>

namespace cppuv::detail
{
    // Tracks the ownership phase of one overlapped kernel operation.
    //
    // Request owns the OVERLAPPED structure. The operation-specific owner
    // (for example, a TCP read/write context) owns the buffers and any
    // resources that must remain alive until completion.
    struct Request
    {
        enum class State
        {
            Created,
            Submitted,
            Completed,
            SubmissionFailed
        };

        // Keep this as the first member so an OVERLAPPED pointer can later
        // be mapped back to its Request without relying on inheritance.
        OVERLAPPED overlapped{};

        State state = State::Created;

        // A request can be submitted exactly once.
        [[nodiscard]] bool markSubmitted() noexcept
        {
            if (state != State::Created)
            {
                return false;
            }

            state = State::Submitted;
            return true;
        }

        // A submitted request is completed exactly once, whether the I/O
        // itself succeeded or failed. The completion status lives in
        // cppuv::Completion, not in this ownership state.
        [[nodiscard]] bool markCompleted() noexcept
        {
            if (state != State::Submitted)
            {
                return false;
            }

            state = State::Completed;
            return true;
        }

        // A synchronous submission error means Windows rejected an attempted
        // operation, so no IOCP completion is expected for this request.
        [[nodiscard]] bool markSubmissionFailed() noexcept
        {
            if (state != State::Submitted)
            {
                return false;
            }

            state = State::SubmissionFailed;
            return true;
        }
    };

    static_assert(std::is_standard_layout_v<Request>,
                  "Request must remain standard-layout for OVERLAPPED recovery");
    static_assert(offsetof(Request, overlapped) == 0,
                  "OVERLAPPED must remain the first Request member");
}
