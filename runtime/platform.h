#pragma once
// The few things the runtime asks of the host system that Linux, Windows and
// Apple's systems spell differently. Everything else goes through the C++
// library or SDL.
#include <cstdint>
#include <functional>
#include <thread>

namespace platform
{
    // A thread that runs guest code. The recompiled code nests deep on the
    // host stack: Linux gives a thread 8 MB and the Windows build asks for
    // 16 MB, but macOS and iOS give a secondary thread 512 KB, which the title
    // overflows in its first frames. On Apple this is a pthread with 16 MB of
    // stack; elsewhere it is a std::thread. Joined or detached like one.
    class GuestThread
    {
    public:
        GuestThread() = default;
        explicit GuestThread(std::function<void()> body);
        GuestThread(GuestThread&& other) noexcept;
        GuestThread& operator=(GuestThread&& other) noexcept;
        GuestThread(const GuestThread&) = delete;
        GuestThread& operator=(const GuestThread&) = delete;
        ~GuestThread();

        bool joinable() const;
        void join();
        void detach();

    private:
#ifdef __APPLE__
        void* handle_ = nullptr;   // pthread_t
#else
        std::thread thread_;
#endif
    };

    // First thing in main, before any other thread starts: Windows sleeps in
    // 15.6 ms steps unless asked for 1 ms, and the title paces itself with
    // short sleeps; and a release build there finds the terminal it was
    // started from, if any.
    void Initialise();

    // The system's id for the calling thread, as a profiler shows it.
    uint32_t ThreadId();
    bool IsMainThread();
    // Up to fifteen characters are kept on Linux.
    void SetThreadName(const char* name);
    // Background work that must not take time from the frame.
    void LowerThreadPriority();
    // Only the time nothing else wants.
    void IdleThreadPriority();
    // On the frame's path: Apple's systems otherwise park a new thread on the
    // efficiency cores and throttle it first. Nothing elsewhere.
    void InteractiveThreadPriority();

    // The calling thread's host stack to stderr. The recompiled functions are
    // named sub_XXXXXXXX, so where symbols resolve this is the guest's stack.
    void PrintBacktrace(const char* why);

    // A shared library: loaded, or only if something already loaded it.
    void* OpenLibrary(const char* path);
    void* LoadedLibrary(const char* name);
    void* Symbol(void* library, const char* name);
}
