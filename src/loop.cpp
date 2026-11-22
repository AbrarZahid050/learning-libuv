#include "cppuv/loop.hpp" // Bring in the Loop declaration and its private state.
#include <algorithm> // Provide std::max for clamping negative timer delays.
#include <chrono> // Provide steady-clock and duration conversions.
#include <stdexcept> // Represent invalid callbacks and internal counter misuse.
#include <utility> // Move callbacks into queues without unnecessary copies.

namespace cppuv // Define the implementation in the library namespace.
{

    // Enqueue application work so it executes on the event-loop thread.
    void Loop::post(Task task)
    {
        if (!task)
        {
            throw std::invalid_argument("Cannot post an empty task");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_); // Protect the queue and shutdown flag from concurrent callers.

            if (stopping_) // Do not accept new work once shutdown has begun.
            {
                return; // The caller's task is discarded during shutdown.
            }

            tasks_.push(std::move(task)); // Transfer the callback into the FIFO task queue.
        } // Release the mutex before waking the event loop.

        poller_.wake(); // Interrupt an IOCP wait so the loop notices the newly queued task.
    }

    // Configure the policy for exceptions thrown by application callbacks.
    void Loop::setExceptionHandler(ExceptionHandler handler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exceptionHandler_ = std::move(handler);
    }

    // Report callback failures without allowing them to escape the event-loop dispatcher.
    void Loop::handleCallbackException(std::exception_ptr exception)
    {
        ExceptionHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = exceptionHandler_;
        }

        if (!handler)
        {
            stop();
            return;
        }

        try
        {
            handler(exception);
        }
        catch (...)
        {
            stop();
        }
    }

    // Associate a native Windows handle with this loop's completion port.
    void Loop::associateHandle(
        HANDLE handle, // Native file or socket handle to associate.
        ULONG_PTR completionKey) // Key used to find the callback for later completions.
    {
        poller_.associate(handle, completionKey); // Delegate the Windows-specific operation to Poller.
    }

    // Add the callback used to dispatch completions for a particular key.
    void Loop::registerCompletionHandler(
        ULONG_PTR completionKey, // Non-zero identifier attached to the native handle.
        CompletionHandler handler) // Callback that processes completed overlapped I/O.
    {
        if (completionKey == 0) // Zero is reserved for private wake packets.
        {
            throw std::invalid_argument("IOCP completion key 0 is reserved"); // Keep wakeups separate from real I/O.
        }

        if (!handler) // An empty callback cannot process a completion.
        {
            throw std::invalid_argument("Completion handler cannot be empty"); // Reject invalid registrations early.
        }

        std::lock_guard<std::mutex> lock(mutex_); // Protect the dispatch table from concurrent changes.

        const bool inserted =
            completionHandlers_.emplace(completionKey, std::move(handler)).second; // Insert only if the key is unused.

        if (!inserted) // A key must identify exactly one registered callback.
        {
            throw std::logic_error("An IOCP handler is already registered for this key"); // Avoid ambiguous completion routing.
        }
    }

    // Remove the callback associated with a completion key.
    void Loop::unregisterCompletionHandler(ULONG_PTR completionKey)
    {
        std::lock_guard<std::mutex> lock(mutex_); // Prevent the event loop from reading the map during erasure.
        completionHandlers_.erase(completionKey); // Erasing an unknown key is intentionally harmless.
    }

    // Read the shutdown state safely from any thread.
    bool Loop::isStopping() const
    {
        std::lock_guard<std::mutex> lock(mutex_); // Synchronize access to stopping_.
        return stopping_; // Return a consistent snapshot of the shutdown flag.
    }

    // Insert a timer into the timer heap and mark its ID active.
    Loop::TimerId Loop::scheduleTimer(
        std::chrono::milliseconds delay, // Initial delay before the timer becomes eligible.
        std::chrono::milliseconds interval, // Repeat interval, or zero for one-shot timers.
        bool repeating, // Select one-shot or recurring behavior.
        Task task) // Callback retained by the timer.
    {
        if (!task)
        {
            throw std::invalid_argument("Cannot schedule an empty timer callback");
        }

        TimerId id; // Store the identifier that will be returned to the caller.

        {
            std::lock_guard<std::mutex> lock(mutex_); // Protect timer IDs, active IDs, and the heap.

            if (stopping_ || (repeating && interval.count() <= 0)) // Reject timers that cannot be scheduled safely.
            {
                return 0; // Zero is the invalid timer identifier.
            }

            id = nextTimerId_++; // Allocate the next candidate identifier.

            if (nextTimerId_ == 0) // Detect unsigned integer wraparound.
            {
                nextTimerId_ = 1; // Keep zero reserved as an invalid identifier.
            }

            while (activeTimerIds_.find(id) != activeTimerIds_.end()) // Avoid reusing an ID that is still active.
            {
                id = nextTimerId_++; // Try the next candidate identifier.

                if (nextTimerId_ == 0) // Handle wraparound while searching for an unused ID.
                {
                    nextTimerId_ = 1; // Skip the reserved zero value.
                }
            }

            activeTimerIds_.insert(id); // Mark this timer active before making it visible in the heap.

            timers_.push(Timer{ // Insert a complete timer record into the priority queue.
                id, // Preserve the public identifier for cancellation checks.
                Clock::now() + std::max( // Compute an absolute deadline from the monotonic clock.
                    delay, // Use the caller's requested delay.
                    std::chrono::milliseconds::zero()), // Clamp negative delays to immediate execution.
                interval, // Preserve the repeat interval for recurring timers.
                repeating, // Preserve whether this timer repeats.
                std::move(task)}); // Transfer ownership of the callback into the timer.
        } // Release the mutex before waking the IOCP waiter.

        poller_.wake(); // Recalculate the wait timeout because this timer may be earlier.

        return id; // Give the caller the handle needed to cancel this timer.
    }

    // Schedule a callback that runs once after a delay.
    Loop::TimerId Loop::callAfter(
        std::chrono::milliseconds delay, // Wait duration before execution.
        Task task) // Callback to execute once.
    {
        return scheduleTimer( // Reuse the common timer insertion logic.
            delay, // Preserve the caller's requested delay.
            std::chrono::milliseconds::zero(), // One-shot timers have no repeat interval.
            false, // Mark this timer as non-repeating.
            std::move(task)); // Transfer the callback into the scheduler.
    }

    // Schedule a callback that runs repeatedly at a fixed interval.
    Loop::TimerId Loop::repeatEvery(
        std::chrono::milliseconds interval, // Delay between timer executions.
        Task task) // Callback to execute on every expiration.
    {
        return scheduleTimer( // Reuse the common timer insertion logic.
            interval, // The first execution occurs after one interval.
            interval, // Reuse the same duration for subsequent executions.
            true, // Mark this timer as repeating.
            std::move(task)); // Transfer the callback into the scheduler.
    }

    // Cancel a timer by removing its ID from the active set.
    void Loop::clearTimer(TimerId id)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_); // Protect the active-ID set.
            activeTimerIds_.erase(id); // The heap entry is removed lazily when it reaches the top.
        } // Do not hold the mutex while waking the poller.

        poller_.wake(); // Recompute the next timer deadline after cancellation.
    }

    // Run queued tasks, expired timers, and IOCP completions until shutdown drains.
    void Loop::run()
    {
        while (true) // Keep dispatching until stop was requested and all kernel I/O has completed.
        {
            Task task; // Hold one task selected for execution outside the lock.
            Timer timer; // Hold one expired timer selected for execution outside the lock.

            bool hasTask = false; // Record whether a task was removed from the queue.
            bool hasTimer = false; // Record whether an expired timer was removed from the heap.

            DWORD timeout = INFINITE; // Wait indefinitely unless the next timer has a deadline.

            {
                std::unique_lock<std::mutex> lock(mutex_); // Protect all scheduler state while selecting work.

                if (stopping_ && tasks_.empty() && pendingOperations_ == 0) // Shutdown is complete only after tasks and I/O drain.
                {
                    return; // Exit the event loop.
                }

                if (stopping_) // Timers should not keep the loop alive after stop().
                {
                    while (!timers_.empty()) timers_.pop(); // Discard all remaining timer heap entries.
                    activeTimerIds_.clear(); // Mark every timer cancelled.
                }

                if (!tasks_.empty()) // Give posted tasks priority over timer and I/O dispatch.
                {
                    task = std::move(tasks_.front()); // Move the oldest task out of the queue.
                    tasks_.pop(); // Remove the selected task from the queue.

                    hasTask = true; // Tell the dispatch section that a task is ready.
                }
                else // No posted task is waiting; inspect the timer heap.
                {
                    // Remove cancelled timers that have reached the top of the heap.
                    while (!timers_.empty() &&
                           activeTimerIds_.find(timers_.top().id) ==
                               activeTimerIds_.end())
                    {
                        timers_.pop(); // Discard one stale timer entry.
                    }

                    if (!timers_.empty()) // A remaining heap entry represents an active timer.
                    {
                        const auto deadline = timers_.top().deadline; // Read the earliest timer deadline.
                        const auto now = Clock::now(); // Sample the monotonic clock once for this decision.

                        if (now >= deadline) // The earliest timer is ready to execute.
                        {
                            timer = timers_.top(); // Copy the timer so its callback can run outside the lock.
                            timers_.pop(); // Remove this expiration from the heap.

                            hasTimer = true; // Tell the dispatch section that a timer is ready.

                            if (!timer.repeating) // A one-shot timer must not remain active after firing.
                            {
                                activeTimerIds_.erase(timer.id); // Make later cancellation harmless.
                            }
                        }
                        else // No timer is ready; calculate how long IOCP may safely sleep.
                        {
                            auto remaining =
                                std::chrono::ceil<std::chrono::milliseconds>(
                                    deadline - now); // Round up so the loop does not wake before the deadline.

                            const auto milliseconds = remaining.count(); // Convert the duration to its integer count.
                            using MillisecondsRep = std::chrono::milliseconds::rep; // Name the duration's representation type.

                            if (milliseconds >=
                                static_cast<MillisecondsRep>(INFINITE - 1)) // Reserve INFINITE for an unbounded wait.
                            {
                                timeout = INFINITE - 1; // Use the largest finite DWORD timeout.
                            }
                            else // The timer deadline fits into a finite Windows wait.
                            {
                                timeout = static_cast<DWORD>(milliseconds); // Convert to the timeout type expected by IOCP.
                            }
                        }
                    }
                }
            } // Release the scheduler mutex before invoking user callbacks or waiting.

            if (hasTask) // A posted callback was selected above.
            {
                try
                {
                    task(); // Execute user code without holding the loop mutex.
                }
                catch (...)
                {
                    handleCallbackException(std::current_exception());
                }
                continue; // Start a fresh scheduling pass after the callback returns.
            }

            if (hasTimer) // An expired timer was selected above.
            {
                try
                {
                    timer.task(); // Execute the timer callback on the event-loop thread.
                }
                catch (...)
                {
                    handleCallbackException(std::current_exception());
                }

                if (timer.repeating) // Reschedule only recurring timers.
                {
                    std::lock_guard<std::mutex> lock(mutex_); // Protect active IDs and the timer heap.

                    if (!stopping_ &&
                        activeTimerIds_.find(timer.id) !=
                            activeTimerIds_.end()) // The callback may have cancelled itself or stopped the loop.
                    {
                        timer.deadline = Clock::now() + timer.interval; // Schedule relative to callback completion.
                        timers_.push(std::move(timer)); // Return the timer to the heap for its next expiration.
                    }
                }

                continue; // Re-evaluate tasks and deadlines before waiting for I/O.
            }

            const auto completion = poller_.wait(timeout); // Wait for I/O, a wake packet, or the next timer deadline.
            if (!completion.has_value() || completion->isWake()) // A timeout or private wake packet requires no I/O dispatch.
            {
                continue; // Re-enter the scheduler and inspect current state.
            }

            CompletionHandler handler; // Copy the selected callback before releasing the map mutex.

            {
                std::lock_guard<std::mutex> lock(mutex_); // Protect the completion-handler map.
                const auto it = completionHandlers_.find(completion->key); // Find the owner of this completion key.

                if (it != completionHandlers_.end()) // Ignore completions whose owner has already unregistered.
                {
                    handler = it->second; // Copy the callback so it remains valid during dispatch.
                }
            } // The callback may register or unregister handlers, so release the mutex first.

            if (handler) // Dispatch only when a handler is still registered.
            {
                try
                {
                    handler(*completion); // Let the owning handle process its completed operation.
                }
                catch (...)
                {
                    handleCallbackException(std::current_exception());
                }
            }
        }
    }

    // Reserve one unit in the shutdown-drain counter before submitting kernel I/O.
    bool Loop::beginOperation()
    {
        std::lock_guard<std::mutex> lock(mutex_); // Synchronize with stop() and endOperation().
        if (stopping_) return false; // Do not allow new overlapped work after shutdown starts.
        ++pendingOperations_; // Keep run() alive until the corresponding completion is dispatched.
        return true; // The caller may now submit the kernel operation.
    }

    // Release one unit after an overlapped operation's completion is handled.
    void Loop::endOperation()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_); // Protect the pending-operation counter.
            if (pendingOperations_ == 0) // Detect mismatched beginOperation()/endOperation() calls.
            {
                throw std::logic_error("Loop operation counter underflow"); // Fail loudly rather than wrapping the counter.
            }
            --pendingOperations_; // Mark one kernel operation as fully drained.
        } // Release the mutex before waking the event loop.

        poller_.wake(); // Allow run() to notice that shutdown may now be complete.
    }

    // Request shutdown without abandoning outstanding overlapped operations.
    void Loop::stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_); // Synchronize shutdown with posting and operation tracking.
            stopping_ = true; // Prevent new tasks and kernel operations from being accepted.
            activeTimerIds_.clear(); // Cancel all timers so they cannot keep the loop alive.
        } // Release the mutex before posting a wake packet.

        poller_.wake(); // Interrupt an infinite IOCP wait so run() can observe shutdown.
    }

} // namespace cppuv
