#include "install.h"
#include "crypto.h"
#include "xex.h"
#include "spawn.h"
#include "../log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#endif
#ifdef MW2_USE_SDL
#include <SDL3/SDL.h>
#endif

namespace fs = std::filesystem;

// The executable this build was recompiled from, by its SHA-256, which CMake
// reads from mw2/ at configure time. Any other -- another region, another
// version -- has other code at other addresses, so it is not run.
namespace
{
#ifdef MW2_TITLE_MP
    constexpr const char* kXex = "default_mp.xex";
    constexpr const char* kSha256 = MW2_XEX_SHA256_MP;
#else
    constexpr const char* kXex = "default.xex";
    constexpr const char* kSha256 = MW2_XEX_SHA256_SP;
#endif
    constexpr const char* kGameFolder = "game";
#ifdef _WIN32
    constexpr const char* kLauncher = "mw2-launcher.exe";
#else
    constexpr const char* kLauncher = "mw2-launcher";
#endif

    // Paths as UTF-8, which SDL and the log take, whatever the system's code page.
    std::string Utf8(const fs::path& path)
    {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

#if defined(__APPLE__) && !TARGET_OS_IOS
    // The folder the running program is in, inside the app or not.
    fs::path ProgramFolder()
    {
        char path[4096];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) != 0) return {};
        std::error_code ec;
        return fs::weakly_canonical(path, ec).parent_path();
    }
#endif

    fs::path ExecutableFolder()
    {
#ifdef _WIN32
        wchar_t path[MAX_PATH * 4];
        const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
        return fs::path(std::wstring(path, length)).parent_path();
#elif TARGET_OS_IOS
        // The app bundle is sealed and read-only, so what a player's copy
        // keeps beside the executable -- game/, saves/, .env -- lives in the
        // app's Documents, which the Files app shows (UIFileSharingEnabled)
        // for copying the installed game in.
        const char* home = std::getenv("HOME");   // the app's data container
        return fs::path(home ? home : "") / "Documents";
#elif defined(__APPLE__)
        const fs::path folder = ProgramFolder();
        // Packaged as MW2.app (tools/macos/make_apps.sh): the player's
        // game/, saves/ and .env are beside the app, never inside it, where
        // they would break its signature.
        if (folder.filename() == "MacOS" && folder.parent_path().filename() == "Contents" &&
            folder.parent_path().parent_path().extension() == ".app")
            return folder.parent_path().parent_path().parent_path();
        return folder;
#else
        std::error_code ec;
        return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
    }

    void ShowError(const std::string& text)
    {
        LOGE("%s", text.c_str());
        std::fprintf(stderr, "%s\n", text.c_str());
#ifdef MW2_USE_SDL
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Modern Warfare 2", text.c_str(), nullptr);
#endif
    }

    // The launcher beside this executable, left running on its own.
    bool StartLauncher()
    {
#ifdef MW2_USE_SDL
        std::error_code ec;
        const fs::path launcher = fs::absolute(kLauncher, ec);
        if (!fs::is_regular_file(launcher, ec)) return false;
        const std::string path = Utf8(launcher);
        const char* const args[] = { path.c_str(), nullptr };
        SDL_Process* process = spawn::Start(args);
        if (!process) { LOGE("cannot start %s: %s", path.c_str(), SDL_GetError()); return false; }
        SDL_DestroyProcess(process);    // forgets it; the launcher keeps running
        return true;
#else
        return false;
#endif
    }

    // The program of each of the title's executables, beside this one.
    const char* ProgramFor(const std::string& xex)
    {
#ifdef _WIN32
        if (xex == "default.xex") return "mw2-sp.exe";
        if (xex == "default_mp.xex") return "mw2-mp.exe";
#else
        if (xex == "default.xex") return "mw2-sp";
        if (xex == "default_mp.xex") return "mw2-mp";
#endif
        return nullptr;
    }

    // The other executable's program: beside this one, or, in the one app
    // that holds both on macOS (tools/macos/make_apps.sh), in Contents/MacOS.
    fs::path ProgramFile(const std::string& xex)
    {
        const char* program = ProgramFor(xex);
        if (!program) return {};
        std::error_code ec;
        fs::path file = fs::absolute(program, ec);
#if defined(__APPLE__) && !TARGET_OS_IOS
        if (!fs::is_regular_file(file, ec)) file = ProgramFolder() / program;
#endif
        return file;
    }

    constexpr const char* kLaunchDataVariable = "MW2_LAUNCH_DATA";
    bool g_playerStart = false;
    std::string g_nextTitle;

#if TARGET_OS_IOS
    // The executable the app starts the next time it is opened ("sp" or
    // "mp"), once, and the launch data waiting for it: ios/main.m reads both,
    // hands the data over in kLaunchDataVariable and removes them, so the
    // app otherwise opens on the campaign's main menu.
    constexpr const char* kTitleFile = "title";
    constexpr const char* kLaunchDataFile = "launch_data";

    const char* ShortName(const std::string& xex)
    {
        if (xex == "default.xex") return "sp";
        if (xex == "default_mp.xex") return "mp";
        return nullptr;
    }
#endif

    std::string LaunchHex()
    {
        std::string hex;
        for (const uint8_t byte : install::LaunchData())
        {
            char text[3];
            std::snprintf(text, sizeof(text), "%02x", byte);
            hex += text;
        }
        return hex;
    }

}


namespace
{
    enum class Installed { Yes, No, Unreadable };

    // The title's image from game/, if the executable there is the one this
    // build was made from.
    Installed LoadInstalled(std::vector<uint8_t>& image, std::string& error)
    {
        const fs::path path = fs::path(kGameFolder) / kXex;
        std::vector<uint8_t> file;
        {
            std::ifstream in(path, std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(in), {});
        }
        if (file.empty()) return Installed::No;
        install::crypto::Sha256 hash;
        hash.Update(file.data(), file.size());
        if (hash.FinalHex() != kSha256) return Installed::No;
        if (!install::xex::Image(file, image, error))
        {
            error = Utf8(path) + ": " + error;
            return Installed::Unreadable;
        }
        return Installed::Yes;
    }
}

bool install::PlayerStart(int argc, char**)
{
    return argc == 1;
}

fs::path install::Folder(int argc, char** argv)
{
    return PlayerStart(argc, argv) ? ExecutableFolder() : fs::path();
}

bool install::Prepare(int, char**, Launch& launch, int& exitCode)
{
    exitCode = 1;
    g_playerStart = true;
    // Everything a player's copy keeps -- game/, saves/ -- is beside the executable.
    std::error_code ec;
    if (const fs::path folder = ExecutableFolder(); !folder.empty()) fs::current_path(folder, ec);

    std::string error;
    switch (LoadInstalled(launch.image, error))
    {
    case Installed::Yes:
        launch.gameRoot = kGameFolder;
        return true;
    case Installed::Unreadable:
        ShowError(error);
        return false;
    case Installed::No:
        break;
    }
    // Not installed, or installed for another version of this build: both are
    // the launcher's to put right.
#if TARGET_OS_IOS
    // No launcher on iOS: the game is installed on a computer and its game/
    // folder copied into the app's Documents (ios/README.md).
    ShowError(std::string("The game is not installed. Copy the game folder your computer's launcher"
                          " installed into this app's Documents (") + kXex + " included).");
#else
    if (StartLauncher()) { exitCode = 0; return false; }
    ShowError(std::string("The game is not installed here, and ") + kLauncher +
              " is not beside this program to install it.");
#endif
    return false;
}

bool install::LoadImageFile(const fs::path& path, std::vector<uint8_t>& image)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { LOGE("cannot open image %s", Utf8(path).c_str()); return false; }
    std::vector<uint8_t> file(std::istreambuf_iterator<char>(in), {});
    if (file.size() >= 4 && std::memcmp(file.data(), "XEX2", 4) == 0)
    {
        std::string error;
        if (!xex::Image(file, image, error)) { LOGE("%s: %s", Utf8(path).c_str(), error.c_str()); return false; }
        return true;
    }
    image = std::move(file);
    return true;
}

std::vector<uint8_t>& install::LaunchData()
{
    static std::vector<uint8_t> data = [] {
        std::vector<uint8_t> bytes;
        const char* hex = std::getenv(kLaunchDataVariable);
        for (; hex && hex[0] && hex[1]; hex += 2)
        {
            unsigned value = 0;
            if (std::sscanf(hex, "%2x", &value) != 1) return std::vector<uint8_t>{};
            bytes.push_back(uint8_t(value));
        }
        return bytes;
    }();
    return data;
}

void install::SetNextTitle(const std::string& xex)
{
    g_nextTitle = xex;
#if TARGET_OS_IOS && defined(MW2_USE_SDL)
    // The app closes to start the other title, which looked like a crash:
    // it says so first, and waits for the player.
    if (g_playerStart && ShortName(xex))
    {
        const std::string text = std::string(xex == "default_mp.xex" ? "Multiplayer" : "The campaign and Special Ops") +
            " will start the next time MW2 is opened. iPadOS does not let an app reopen itself, so MW2 "
            "closes now: open it again from the Home Screen.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Modern Warfare 2", text.c_str(), nullptr);
    }
#endif
}

void install::SwitchTitle()
{
    LaunchData().clear();
    SetNextTitle(std::string(kXex) == "default.xex" ? "default_mp.xex" : "default.xex");
}

void install::StartNextTitle()
{
    if (g_nextTitle.empty()) return;
    const char* program = ProgramFor(g_nextTitle);
    if (!program) { LOGE("the title asked for %s, which is not one of its executables", g_nextTitle.c_str()); return; }
    // A development run was given its image and game folder by hand, and
    // nothing says where the other title's are.
    if (!g_playerStart)
    {
        LOGW("the title asked for %s: start that build yourself, a development run does not", g_nextTitle.c_str());
        return;
    }
#if TARGET_OS_IOS
    // The app holds both titles (ios/main.m) and cannot start itself again:
    // the next title is the one it opens with the next time.
    {
        std::ofstream(kTitleFile) << ShortName(g_nextTitle) << "\n";
        std::error_code ec;
        const std::string hex = LaunchHex();
        if (hex.empty()) fs::remove(kLaunchDataFile, ec);
        else std::ofstream(kLaunchDataFile) << hex;
        LOGI("%s starts the next time the app is opened", g_nextTitle.c_str());
    }
#elif defined(MW2_USE_SDL)
    std::error_code ec;
    const fs::path file = ProgramFile(g_nextTitle);
    if (!fs::is_regular_file(file, ec)) { ShowError(std::string(program) + " is not beside this program."); return; }
    const std::string hex = LaunchHex();
    SDL_Environment* environment = SDL_GetEnvironment();
    if (hex.empty()) SDL_UnsetEnvironmentVariable(environment, kLaunchDataVariable);
    else SDL_SetEnvironmentVariable(environment, kLaunchDataVariable, hex.c_str(), true);
    // It writes on in this run's log, where there is one (main.cpp).
    SDL_SetEnvironmentVariable(environment, "MW2_LOG_APPEND", "1", true);
    const std::string path = Utf8(file);
    const char* const args[] = { path.c_str(), nullptr };
    SDL_Process* process = spawn::Start(args, environment);
    if (!process) { ShowError("cannot start " + path + ": " + SDL_GetError()); return; }
    SDL_DestroyProcess(process);
    LOGI("started %s", path.c_str());
#else
    LOGE("this build cannot start %s", program);
#endif
}
