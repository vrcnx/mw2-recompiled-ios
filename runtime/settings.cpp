#include "settings.h"

#include <cstdlib>
#include <fstream>
#include <vector>

std::string settings::Load(const std::filesystem::path& path)
{
    std::string set;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t equals = line.find('=');
        // The runtime's own switches, not a way to set any variable at all.
        if (equals == std::string::npos || line.rfind("MW2_", 0) != 0) continue;
        const std::string name = line.substr(0, equals), value = line.substr(equals + 1);
        if (std::getenv(name.c_str())) continue;
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 0);
#endif
        set += (set.empty() ? "" : " ") + line;
    }
    return set;
}

bool settings::Store(const std::filesystem::path& path, const std::string& name, const std::string& value)
{
    std::vector<std::string> lines;
    {
        std::ifstream file(path);
        for (std::string line; std::getline(file, line);) lines.push_back(line);
    }
    bool found = false;
    for (auto it = lines.begin(); it != lines.end();)
    {
        if (it->rfind(name + "=", 0) == 0)
        {
            if (found || value.empty()) { it = lines.erase(it); continue; }
            *it = name + "=" + value;
            found = true;
        }
        ++it;
    }
    if (!found && !value.empty()) lines.push_back(name + "=" + value);
    // Beside it, then over it: a run killed mid-write keeps the old file.
    const std::filesystem::path partial = path.string() + ".partial";
    {
        std::ofstream out(partial, std::ios::trunc);
        if (!out) return false;
        for (const std::string& line : lines) out << line << '\n';
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(partial, path, ec);
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    if (value.empty()) unsetenv(name.c_str());
    else setenv(name.c_str(), value.c_str(), 1);
#endif
    return !ec;
}
