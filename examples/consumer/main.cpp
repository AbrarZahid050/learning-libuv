#include <cppuv/cppuv.hpp> // Include the installed library through its public umbrella header.
#include <chrono> // Express the demonstration timer duration.
#include <iostream> // Print messages from the application callback.

int main() // Application entry point; this file does not compile any CppUV implementation source.
{
    cppuv::Loop loop; // Construct the event loop supplied by the installed CppUV library.

    loop.callAfter( // Schedule a one-shot callback using the public API.
        std::chrono::milliseconds(100), // Wait briefly so the example demonstrates asynchronous dispatch.
        [&loop]() // Capture the loop because the callback will request its shutdown.
        {
            std::cout << "Hello from an application using the installed CppUV package!\n"; // Show that the consumer linked successfully.
            loop.stop(); // Stop the loop after the demonstration callback has executed.
        });

    loop.run(); // Enter the library's event loop and dispatch the scheduled callback.

    return 0; // Return success after the loop has drained and stopped.
}
