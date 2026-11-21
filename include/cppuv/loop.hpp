#pragma once

#include "cppuv/poller.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cppuv
{

    class Loop
    {
    public:
        using Task = std::function<void()>;
        using TimerId = std::uint64_t;
        using CompletionHandler = std::function<void(const Completion &)>;

        void post(Task task);

        void associateHandle(HANDLE handle, ULONG_PTR completionKey);

        void registerCompletionHandler(
            ULONG_PTR completionKey,
            CompletionHandler handler);

        void unregisterCompletionHandler(ULONG_PTR completionKey);

        [[nodiscard]] bool isStopping() const;

        TimerId callAfter(
            std::chrono::milliseconds delay,
            Task task);

        TimerId repeatEvery(
            std::chrono::milliseconds interval,
            Task task);

        void clearTimer(TimerId id);

        void run();
        void stop();

        // Track kernel operations so stop() can drain their IOCP completions.
        [[nodiscard]] bool beginOperation();
        void endOperation();

    private:
        using Clock = std::chrono::steady_clock;

        struct Timer
        {
            TimerId id;
            Clock::time_point deadline;
            std::chrono::milliseconds interval;
            bool repeating;
            Task task;
        };

        struct TimerCompare
        {
            bool operator()(const Timer &a, const Timer &b) const
            {
                return a.deadline > b.deadline;
            }
        };

        TimerId scheduleTimer(
            std::chrono::milliseconds delay,
            std::chrono::milliseconds interval,
            bool repeating,
            Task task);

        Poller poller_;

        std::queue<Task> tasks_;

        std::priority_queue<
            Timer,
            std::vector<Timer>,
            TimerCompare>
            timers_;

        std::unordered_set<TimerId> activeTimerIds_;
        std::unordered_map<ULONG_PTR, CompletionHandler> completionHandlers_;

        mutable std::mutex mutex_;

        TimerId nextTimerId_ = 1;
        bool stopping_ = false;
        std::size_t pendingOperations_ = 0;
    };

}