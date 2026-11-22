#include "cppuv/cppuv.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

namespace
{
    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void testPostedTask()
    {
        cppuv::Loop loop;
        bool called = false;

        loop.post([&]()
        {
            called = true;
            loop.stop();
        });

        loop.run();
        require(called, "posted task did not run");
    }

    void testOneShotTimer()
    {
        cppuv::Loop loop;
        bool fired = false;

        const auto timer = loop.callAfter(1ms, [&]()
        {
            fired = true;
            loop.stop();
        });

        require(timer != 0, "one-shot timer was not scheduled");
        loop.run();
        require(fired, "one-shot timer did not fire");
    }

    void testCancelledTimer()
    {
        cppuv::Loop loop;
        bool cancelledCallbackRan = false;

        const auto cancelled = loop.callAfter(1ms, [&]()
        {
            cancelledCallbackRan = true;
        });

        require(cancelled != 0, "cancelled timer was not scheduled");
        loop.clearTimer(cancelled);

        loop.callAfter(10ms, [&]()
        {
            loop.stop();
        });

        loop.run();
        require(!cancelledCallbackRan, "cancelled timer callback ran");
    }

    void testCallbackExceptionHandler()
    {
        cppuv::Loop loop;
        bool exceptionReceived = false;

        loop.setExceptionHandler([&](std::exception_ptr exception)
        {
            try
            {
                std::rethrow_exception(exception);
            }
            catch (const std::runtime_error &)
            {
                exceptionReceived = true;
            }

            loop.stop();
        });

        loop.post([]()
        {
            throw std::runtime_error("expected callback failure");
        });

        loop.run();
        require(exceptionReceived, "exception handler did not receive callback failure");
    }

    void testEmptyCallbacksAreRejected()
    {
        cppuv::Loop loop;
        bool postRejected = false;
        bool timerRejected = false;

        try
        {
            loop.post(cppuv::Loop::Task{});
        }
        catch (const std::invalid_argument &)
        {
            postRejected = true;
        }

        try
        {
            loop.callAfter(1ms, cppuv::Loop::Task{});
        }
        catch (const std::invalid_argument &)
        {
            timerRejected = true;
        }

        require(postRejected, "empty posted task was accepted");
        require(timerRejected, "empty timer callback was accepted");
    }

    void testPendingAcceptIsDrainedAfterServerDestruction()
    {
        cppuv::Winsock winsock;
        cppuv::Loop loop;

        {
            cppuv::TcpServer server;
            server.listen(0);
            server.startAccepting(loop, [](SOCKET acceptedSocket)
            {
                closesocket(acceptedSocket);
            });
        }

        // Destroying the server cancels its pending AcceptEx. The loop must
        // still dispatch that completion before its pending-operation count
        // reaches zero and run() is allowed to return.
        loop.stop();
        loop.run();
    }
}

int main()
{
    testPostedTask();
    testOneShotTimer();
    testCancelledTimer();
    testCallbackExceptionHandler();
    testEmptyCallbacksAreRejected();
    testPendingAcceptIsDrainedAfterServerDestruction();

    std::cout << "All CppUV regression tests passed.\n";
    return 0;
}
