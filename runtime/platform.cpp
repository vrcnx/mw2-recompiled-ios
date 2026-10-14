#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <string>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef _WIN32

#include <cmath>

// C23's round-half-to-even, which the recompiled vector rounding reaches
// through simde and the UCRT does not have. Whatever the rounding mode: a tie
// goes to the even neighbour, anything else to the nearer one.
extern "C" float roundevenf(float x)
{
    const float nearest = std::round(x);   // ties away from zero
    if (std::fabs(nearest - x) != 0.5f) return nearest;
    return 2.0f * std::round(x * 0.5f);
}

namespace { DWORD g_mainThread = 0; }

void platform::Initialise()
{
    g_mainThread = GetCurrentThreadId();
    timeBeginPeriod(1);
    // A player's build has no console window of its own. Started from a
    // terminal, it writes there, as a console program would.
    if (!GetConsoleWindow() && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}

uint32_t platform::ThreadId() { return uint32_t(GetCurrentThreadId()); }
bool platform::IsMainThread() { return GetCurrentThreadId() == g_mainThread; }

void platform::SetThreadName(const char* name)
{
    wchar_t wide[64];
    const int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
    if (n <= 0) return;
    // Windows 10 1607 and later; looked up, so older systems just go without.
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto set = reinterpret_cast<SetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), wide);
}

void platform::LowerThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
void platform::IdleThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE); }

// No symbols without a PDB: each frame as its offset in the executable, which
// `llvm-symbolizer --obj=mw2.exe` turns back into a name on the build machine.
void platform::PrintBacktrace(const char* why)
{
    void* frames[64];
    const USHORT n = CaptureStackBackTrace(0, 64, frames, nullptr);
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "\n[E] ---- %s (%u frames) ----\n", why, unsigned(n));
    for (USHORT i = 0; i < n; i++)
    {
        const auto at = reinterpret_cast<uintptr_t>(frames[i]);
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(frames[i]), &owner);
        if (reinterpret_cast<uintptr_t>(owner) == module)
            std::fprintf(stderr, "[E]   exe+0x%llx\n", (unsigned long long)(at - module));
        else
            std::fprintf(stderr, "[E]   %p\n", frames[i]);
    }
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}

void* platform::OpenLibrary(const char* path)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wide(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), n);
    // The library's own folder is searched for what it loads in turn.
    return LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void* platform::LoadedLibrary(const char* name) { return GetModuleHandleA(name); }

void* platform::Symbol(void* library, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}

#elif defined(__APPLE__)

void platform::Initialise() {}

uint32_t platform::ThreadId()
{
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return uint32_t(id);
}

bool platform::IsMainThread() { return pthread_main_np() != 0; }

// Apple's takes the calling thread's name only, up to 63 characters.
void platform::SetThreadName(const char* name) { pthread_setname_np(name); }

// Apple schedules by quality of service, not by nice value or policy.
void platform::LowerThreadPriority() { pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); }
void platform::IdleThreadPriority() { pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0); }

#else

void platform::Initialise() {}
uint32_t platform::ThreadId() { return uint32_t(gettid()); }
bool platform::IsMainThread() { return gettid() == getpid(); }

void platform::SetThreadName(const char* name)
{
    char shortName[16];
    std::snprintf(shortName, sizeof shortName, "%s", name);
    pthread_setname_np(pthread_self(), shortName);
}

void platform::LowerThreadPriority() { setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10); }

void platform::IdleThreadPriority()
{
    sched_param none{};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &none) != 0) LowerThreadPriority();
}

#endif

#ifndef _WIN32

void platform::PrintBacktrace(const char* why)
{
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fprintf(stderr, "\n[E] ---- %s (%d frames) ----\n", why, n);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, n, 2);
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}

void* platform::OpenLibrary(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* platform::LoadedLibrary(const char* name) { return dlopen(name, RTLD_NOW | RTLD_NOLOAD); }
void* platform::Symbol(void* library, const char* name) { return dlsym(library, name); }

#endif

#ifdef __APPLE__

namespace
{
    // The 16 MB the Windows build asks of its linker (CMakeLists.txt).
    constexpr size_t kGuestStackBytes = 16u << 20;

    void* RunGuestThread(void* body)
    {
        std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(body));
#if TARGET_OS_IOS
        // Left to itself iOS parks a new thread on the efficiency cores and
        // throttles it first; every guest thread is on the frame's path.
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        (*owned)();
        return nullptr;
    }
}

platform::GuestThread::GuestThread(std::function<void()> body)
{
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGuestStackBytes);
    auto* owned = new std::function<void()>(std::move(body));
    pthread_t thread;
    if (pthread_create(&thread, &attributes, RunGuestThread, owned) != 0)
    {
        delete owned;
        std::fprintf(stderr, "[E] could not start a guest thread\n");
        std::abort();
    }
    pthread_attr_destroy(&attributes);
    handle_ = reinterpret_cast<void*>(thread);
}

platform::GuestThread::GuestThread(GuestThread&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

platform::GuestThread& platform::GuestThread::operator=(GuestThread&& other) noexcept
{
    if (joinable()) std::terminate();   // as std::thread does
    handle_ = other.handle_;
    other.handle_ = nullptr;
    return *this;
}

platform::GuestThread::~GuestThread() { if (joinable()) std::terminate(); }

bool platform::GuestThread::joinable() const { return handle_ != nullptr; }

void platform::GuestThread::join()
{
    pthread_join(reinterpret_cast<pthread_t>(handle_), nullptr);
    handle_ = nullptr;
}

void platform::GuestThread::detach()
{
    pthread_detach(reinterpret_cast<pthread_t>(handle_));
    handle_ = nullptr;
}

#else

platform::GuestThread::GuestThread(std::function<void()> body) : thread_(std::move(body)) {}
platform::GuestThread::GuestThread(GuestThread&& other) noexcept = default;
platform::GuestThread& platform::GuestThread::operator=(GuestThread&& other) noexcept = default;
platform::GuestThread::~GuestThread() = default;
bool platform::GuestThread::joinable() const { return thread_.joinable(); }
void platform::GuestThread::join() { thread_.join(); }
void platform::GuestThread::detach() { thread_.detach(); }

#endif
