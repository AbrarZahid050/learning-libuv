#include "cppuv/loop.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace cppuv
{

    void Loop::post(Task task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopping_)
            {
                return;
            }

            tasks_.push(std::move(task));
        }

        poller_.wake();
    }

    void Loop::associateHandle(
        HANDLE handle,
        ULONG_PTR completionKey)
    {
        poller_.associate(handle, completionKey);
    }

    void Loop::registerCompletionHandler(
        ULONG_PTR completionKey,
        CompletionHandler handler)
    {
        if (completionKey == 0)
        {
            throw std::invalid_argument("IOCP completion key 0 is reserved");
        }

        if (!handler)
        {
            throw std::invalid_argument("Completion handler cannot be empty");
        }

        std::lock_guard<std::mutex> lock(mutex_);

        const bool inserted =
            completionHandlers_.emplace(completionKey, std::move(handler)).second;

        if (!inserted)
        {
            throw std::logic_error("An IOCP handler is already registered for this key");
        }
    }

    void Loop::unregisterCompletionHandler(ULONG_PTR completionKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completionHandlers_.erase(completionKey);
    }

    bool Loop::isStopping() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    Loop::TimerId Loop::scheduleTimer(
        std::chrono::milliseconds delay,
        std::chrono::milliseconds interval,
        bool repeating,
        Task task)
    {
        TimerId id;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopping_ || (repeating && interval.count() <= 0))
            {
                return 0;
            }

            id = nextTimerId_++;

            if (nextTimerId_ == 0)
            {
                nextTimerId_ = 1;
            }

            while (activeTimerIds_.find(id) != activeTimerIds_.end())
            {
                id = nextTimerId_++;

                if (nextTimerId_ == 0)
                {
                    nextTimerId_ = 1;
                }
            }

            activeTimerIds_.insert(id);

            timers_.push(Timer{
                id,
                Clock::now() + std::max(
                                   delay,
                                   std::chrono::milliseconds::zero()),
                interval,
                repeating,
                std::move(task)});
        }

        poller_.wake();

        return id;
    }

    Loop::TimerId Loop::callAfter(
        std::chrono::milliseconds delay,
        Task task)
    {
        return scheduleTimer(
            delay,
            std::chrono::milliseconds::zero(),
            false,
            std::move(task));
    }

    Loop::TimerId Loop::repeatEvery(
        std::chrono::milliseconds interval,
        Task task)
    {
        return scheduleTimer(
            interval,
            interval,
            true,
            std::move(task));
    }

    void Loop::clearTimer(TimerId id)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeTimerIds_.erase(id);
        }

        poller_.wake();
    }

    void Loop::run()
    {
        while (true)
        {
            Task task;
            Timer timer;

            bool hasTask = false;
            bool hasTimer = false;

            DWORD timeout = INFINITE;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                if (stopping_ && tasks_.empty())
                {
                    return;
                }

                if (!tasks_.empty())
                {
                    task = std::move(tasks_.front());
                    tasks_.pop();

                    hasTask = true;
                }
                else
                {
                    // Remove cancelled timers.
                    while (!timers_.empty() &&
                           activeTimerIds_.find(timers_.top().id) ==
                               activeTimerIds_.end())
                    {
                        timers_.pop();
                    }

                    if (!timers_.empty())
                    {
                        const auto deadline = timers_.top().deadline;
                        const auto now = Clock::now();

                        if (now >= deadline)
                        {
                            timer = timers_.top();
                            timers_.pop();

                            hasTimer = true;

                            if (!timer.repeating)
                            {
                                activeTimerIds_.erase(timer.id);
                            }
                        }
                        else
                        {
                            auto remaining =
                                std::chrono::ceil<std::chrono::milliseconds>(
                                    deadline - now);

                            const auto milliseconds = remaining.count();
                            using MillisecondsRep = std::chrono::milliseconds::rep;

                            if (milliseconds >=
                                static_cast<MillisecondsRep>(INFINITE - 1))
                            {
                                timeout = INFINITE - 1;
                            }
                            else
                            {
                                timeout = static_cast<DWORD>(milliseconds);
                            }
                        }
                    }
                }
            }

            if (hasTask)
            {
                task();
                continue;
            }

            if (hasTimer)
            {
                timer.task();

                if (timer.repeating)
                {
                    std::lock_guard<std::mutex> lock(mutex_);

                    if (!stopping_ &&
                        activeTimerIds_.find(timer.id) !=
                            activeTimerIds_.end())
                    {
                        timer.deadline = Clock::now() + timer.interval;
                        timers_.push(std::move(timer));
                    }
                }

                continue;
            }

            const auto completion = poller_.wait(timeout);
            if (!completion.has_value() || completion->isWake())
            {
                continue;
            }

            CompletionHandler handler;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = completionHandlers_.find(completion->key);

                if (it != completionHandlers_.end())
                {
                    handler = it->second;
                }
            }

            if (handler)
            {
                handler(*completion);
            }
        }
    }

    void Loop::stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }

        poller_.wake();
    }

}
