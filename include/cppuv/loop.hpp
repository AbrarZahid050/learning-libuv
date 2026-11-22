#pragma once // Prevent this public header from being included more than once per translation unit.

#include "cppuv/poller.hpp" // Reuse the Windows completion data type exposed by the IOCP layer.
#include <chrono> // Represent timer durations and monotonic deadlines.
#include <cstddef> // Provide std::size_t for operation counters.
#include <cstdint> // Provide a fixed-width integer type for timer identifiers.
#include <functional> // Provide std::function for tasks and completion callbacks.
#include <exception> // Expose callback failures to the configured exception handler.
#include <mutex> // Protect loop state shared with posting threads.
#include <queue> // Store tasks and the timer priority queue.
#include <unordered_map> // Map IOCP completion keys to their callbacks.
#include <unordered_set> // Track active timers independently from stale heap entries.
#include <vector> // Supply the backing container used by the timer heap.

namespace cppuv // Keep the public library API inside its own namespace.
{

    // Owns the IOCP event loop, task queue, timers, and completion dispatch.
    class Loop
    {
    public:
        using Task = std::function<void()>; // A callable scheduled to run on the event-loop thread.
        using TimerId = std::uint64_t; // Stable identifier returned when a timer is scheduled.
        using CompletionHandler = std::function<void(const Completion &)>; // Receives one completed overlapped operation.
        using ExceptionHandler = std::function<void(std::exception_ptr)>; // Receives exceptions thrown by loop callbacks.

        // Queue a callback for execution by the event-loop thread.
        void post(Task task);

        // Configure how exceptions from tasks, timers, and completion handlers are reported.
        // If no handler is configured, an uncaught callback exception stops the loop.
        void setExceptionHandler(ExceptionHandler handler);

        // Associate a Windows handle with this loop's IOCP instance.
        // The completion key selects the registered callback for that handle.
        void associateHandle(HANDLE handle, ULONG_PTR completionKey);

        // Register the callback that will receive completions carrying this key.
        void registerCompletionHandler(
            ULONG_PTR completionKey, // The non-zero key associated with a Windows handle.
            CompletionHandler handler); // The callback invoked when that handle completes an operation.

        // Remove a previously registered completion callback.
        void unregisterCompletionHandler(ULONG_PTR completionKey);

        // Report whether stop() has been requested.
        [[nodiscard]] bool isStopping() const;

        // Schedule a callback to run once after the supplied delay.
        // A zero or negative delay makes the callback eligible immediately.
        TimerId callAfter(
            std::chrono::milliseconds delay, // Time to wait before the callback becomes eligible.
            Task task); // Work to execute when the timer expires.

        // Schedule a callback to run repeatedly at the supplied interval.
        // Non-positive intervals are rejected by returning timer ID zero.
        TimerId repeatEvery(
            std::chrono::milliseconds interval, // Delay between successive executions.
            Task task); // Work to execute on each expiration.

        // Cancel a timer that was returned by callAfter() or repeatEvery().
        void clearTimer(TimerId id);

        // Run callbacks and IOCP completions until stop() is requested and I/O drains.
        void run();

        // Request loop shutdown; pending kernel operations are still dispatched.
        void stop();

        // Track kernel operations so stop() can drain their IOCP completions.
        // Returns false when shutdown has already started.
        [[nodiscard]] bool beginOperation();

        // Mark one previously registered kernel operation as completed.
        void endOperation();

    private:
        using Clock = std::chrono::steady_clock; // Use a monotonic clock so wall-clock changes do not affect timers.

        // Store all data needed to schedule and execute one timer.
        struct Timer
        {
            TimerId id; // Identifier used to determine whether this timer is still active.
            Clock::time_point deadline; // Absolute monotonic time when the timer becomes eligible.
            std::chrono::milliseconds interval; // Repeat delay; zero for one-shot timers.
            bool repeating; // Distinguish one-shot timers from repeating timers.
            Task task; // Callback invoked when the deadline is reached.
        };

        // Make the priority queue expose the timer with the earliest deadline.
        struct TimerCompare
        {
            bool operator()(const Timer &a, const Timer &b) const
            {
                return a.deadline > b.deadline; // Reverse the default ordering to create a min-heap.
            }
        };

        // Report a callback exception without allowing it to escape the event-loop dispatcher.
        void handleCallbackException(std::exception_ptr exception);

        // Shared implementation for one-shot and repeating timer creation.
        TimerId scheduleTimer(
            std::chrono::milliseconds delay, // Initial delay before the first execution.
            std::chrono::milliseconds interval, // Repeat interval, or zero for a one-shot timer.
            bool repeating, // Whether the timer should be scheduled again after execution.
            Task task); // Callback to retain until the timer fires or is cancelled.

        Poller poller_; // Owns the Windows IOCP handle used to wait for kernel completions.

        std::queue<Task> tasks_; // FIFO queue of callbacks posted by application threads.

        std::priority_queue<
            Timer, // Each heap entry contains one scheduled timer.
            std::vector<Timer>, // Store heap entries contiguously for efficient priority-queue operations.
            TimerCompare> // Ensure the earliest deadline is always at the top.
            timers_; // Holds active timers and lazily discarded cancelled entries.

        std::unordered_set<TimerId> activeTimerIds_; // Membership identifies timers that have not been cancelled.
        std::unordered_map<ULONG_PTR, CompletionHandler> completionHandlers_; // Dispatch table keyed by IOCP completion key.
        ExceptionHandler exceptionHandler_; // Optional application callback for exceptions from dispatched work.

        mutable std::mutex mutex_; // Protect queues, callbacks, timer IDs, and shutdown state across threads.

        TimerId nextTimerId_ = 1; // Reserve zero as an invalid timer identifier.
        bool stopping_ = false; // Prevent new work from being accepted after shutdown begins.
        std::size_t pendingOperations_ = 0; // Count overlapped operations whose completions must still be drained.
    };

} // namespace cppuv
