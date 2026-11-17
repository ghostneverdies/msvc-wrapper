#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

static bool capture(const std::wstring &command, std::string &output)
{
    FILE *pipe = _wpopen(command.c_str(), L"rb");
    if (!pipe)
        return false;
    char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), pipe)) != 0)
        output.append(buffer, count);
    return _pclose(pipe) == 0;
}

static int fail(const wchar_t *message)
{
    fwprintf(stderr, L"cl wrapper: %ls\n", message);
    return 1;
}

static std::wstring variable(const wchar_t *name)
{
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size)
        return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written >= size)
        return {};
    value.resize(written);
    return value;
}

static void hash_text(uint64_t &hash, const std::wstring &text)
{
    for (wchar_t ch : text)
    {
        hash ^= static_cast<uint16_t>(ch);
        hash *= 1099511628211ULL;
    }
    hash ^= 0xffff;
    hash *= 1099511628211ULL;
}

static uint64_t cache_key(const std::filesystem::path &installation)
{
    uint64_t hash = 14695981039346656037ULL;
    // A cached PATH must never replace a different caller's PATH (or CL/SDK settings).
    wchar_t *environment = GetEnvironmentStringsW();
    if (!environment)
        return 0;
    for (const wchar_t *entry = environment; *entry; entry += wcslen(entry) + 1)
        hash_text(hash, entry);
    FreeEnvironmentStringsW(environment);
    hash_text(hash, installation.wstring());
    const auto stamp = [&](const std::filesystem::path &path)
    {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(path, error);
        hash_text(hash, path.wstring());
        hash_text(hash, error ? L"missing" : std::to_wstring(time.time_since_epoch().count()));
    };
    stamp(installation / L"VC/Auxiliary/Build");
    stamp(installation / L"VC/Auxiliary/Build/vcvars64.bat");
    stamp(installation / L"VC/Auxiliary/Build/vcvarsall.bat");
    stamp(installation / L"VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt");
    const auto inventory = [&](const std::filesystem::path &path)
    {
        stamp(path);
        std::error_code error;
        for (std::filesystem::directory_iterator it(path, error), end;
             !error && it != end; it.increment(error))
            stamp(it->path());
    };
    inventory(installation / L"VC/Tools/MSVC");
    for (const wchar_t *root : {L"ProgramFiles(x86)", L"ProgramFiles"})
    {
        for (const wchar_t *folder : {L"Include", L"Lib", L"bin"})
            inventory(std::filesystem::path(variable(root)) / L"Windows Kits/10" / folder);
    }
    return hash;
}

static std::wstring changed_variables(const std::wstring &environment)
{
    std::wstring changes;
    size_t start = 0;
    while (start < environment.size())
    {
        const size_t end = environment.find(L'\n', start);
        std::wstring line = environment.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        const size_t equals = line.find(L'=');
        if (equals != std::wstring::npos && equals != 0 &&
            variable(line.substr(0, equals).c_str()) != line.substr(equals + 1))
            changes += line + L'\n';
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return changes;
}

static bool valid_environment(const std::wstring &environment)
{
    if (environment.empty() || environment.find(L'\0') != std::wstring::npos ||
        environment.back() != L'\n')
        return false;
    bool compiler_found = false;
    size_t start = 0;
    while (start < environment.size())
    {
        const size_t end = environment.find(L'\n', start);
        const auto line = environment.substr(start, end - start);
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos || equals == 0)
            return false;
        const auto name = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (_wcsicmp(name.c_str(), L"VCToolsInstallDir") == 0)
        {
            std::error_code error;
            compiler_found = std::filesystem::is_regular_file(
                std::filesystem::path(value) / L"bin/Hostx64/x64/cl.exe", error);
            if (!compiler_found)
                return false;
        }
        if (_wcsicmp(name.c_str(), L"INCLUDE") == 0 || _wcsicmp(name.c_str(), L"LIB") == 0)
        {
            size_t offset = 0;
            while (offset < value.size())
            {
                const size_t separator = value.find(L';', offset);
                const auto path = value.substr(offset, separator - offset);
                std::error_code error;
                if (!path.empty() && !std::filesystem::is_directory(path, error))
                    return false;
                if (separator == std::wstring::npos)
                    break;
                offset = separator + 1;
            }
        }
        start = end + 1;
    }
    // The compiler directory can already exist in the caller's environment.
    if (!compiler_found)
    {
        std::error_code error;
        compiler_found = std::filesystem::is_regular_file(
            std::filesystem::path(variable(L"VCToolsInstallDir")) / L"bin/Hostx64/x64/cl.exe", error);
    }
    return compiler_found;
}

static bool load_cache(const std::filesystem::path &path, uint64_t key, std::wstring &environment)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const std::streamoff size = input.tellg();
    if (size < 16 || size > 4 * 1024 * 1024 || (size - 16) % sizeof(wchar_t))
        return false;
    input.seekg(0);
    uint64_t header[2]{};
    input.read(reinterpret_cast<char *>(header), sizeof(header));
    if (header[0] != 0x434c434143484531ULL || header[1] != key)
        return false;
    environment.resize(static_cast<size_t>(size - 16) / sizeof(wchar_t));
    input.read(reinterpret_cast<char *>(environment.data()), size - 16);
    return input.good() && valid_environment(environment);
}

static void save_cache(const std::filesystem::path &path, uint64_t key, const std::wstring &environment)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return; // Caching is optional; compilation must still work.
    const std::filesystem::path pending = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    const uint64_t header[]{0x434c434143484531ULL, key};
    output.write(reinterpret_cast<const char *>(header), sizeof(header));
    output.write(reinterpret_cast<const char *>(environment.data()), environment.size() * sizeof(wchar_t));
    output.close();
    if (!output || !MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        std::filesystem::remove(pending, error);
}

// Keep this executable reachable from any directory. On the first run the
// directory containing this binary is added to the current user's PATH; on
// later runs the recorded location is verified and, if the binary has moved,
// the stale entry is redirected to the directory it is running from now.
// This self-check is intentionally silent.
static void sync_user_path()
{
    wchar_t module[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return;
    const std::filesystem::path executable(module);
    std::error_code error;
    const auto directory = executable.parent_path().lexically_normal();
    if (directory.empty() || !std::filesystem::is_directory(directory, error))
        return;
    const std::wstring current = directory.wstring();

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
        return;

    DWORD type = REG_EXPAND_SZ;
    DWORD bytes = 0;
    const LONG queried = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &bytes);
    if (queried != ERROR_SUCCESS && queried != ERROR_FILE_NOT_FOUND)
    {
        RegCloseKey(key);
        return;
    }

    std::wstring path;
    if (queried == ERROR_SUCCESS && bytes >= sizeof(wchar_t))
    {
        path.resize(bytes / sizeof(wchar_t));
        DWORD read = bytes;
        if (RegQueryValueExW(key, L"Path", nullptr, &type,
                            reinterpret_cast<BYTE *>(path.data()), &read) != ERROR_SUCCESS)
        {
            RegCloseKey(key);
            return;
        }
        path.resize(read / sizeof(wchar_t));
        while (!path.empty() && path.back() == L'\0')
            path.pop_back();
    }

    std::wstring installed;
    DWORD markerBytes = 0;
    if (RegQueryValueExW(key, L"CL_WRAPPER_DIR", nullptr, nullptr, nullptr, &markerBytes) == ERROR_SUCCESS &&
        markerBytes >= sizeof(wchar_t))
    {
        installed.resize(markerBytes / sizeof(wchar_t));
        DWORD read = markerBytes;
        if (RegQueryValueExW(key, L"CL_WRAPPER_DIR", nullptr, nullptr,
                            reinterpret_cast<BYTE *>(installed.data()), &read) == ERROR_SUCCESS)
            installed.resize(read / sizeof(wchar_t));
        else
            installed.clear();
        while (!installed.empty() && installed.back() == L'\0')
            installed.pop_back();
    }

    const bool stale = !installed.empty() && _wcsicmp(installed.c_str(), current.c_str()) != 0;
    const auto same = [](const std::wstring &left, const std::wstring &right)
    {
        std::wstring a = left;
        std::wstring b = right;
        while (!a.empty() && (a.back() == L'\\' || a.back() == L'/'))
            a.pop_back();
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/'))
            b.pop_back();
        return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
    };

    std::wstring updated;
    bool changed = false;
    bool present = false;
    size_t start = 0;
    while (start <= path.size())
    {
        const size_t separator = path.find(L';', start);
        const std::wstring entry =
            separator == std::wstring::npos ? path.substr(start) : path.substr(start, separator - start);
        if (stale && same(entry, installed))
        {
            changed = true; // Redirect the old location to the current one.
        }
        else
        {
            if (same(entry, current))
                present = true;
            if (!updated.empty())
                updated += L';';
            updated += entry;
        }
        if (separator == std::wstring::npos)
            break;
        start = separator + 1;
    }
    if (!present)
    {
        if (!updated.empty())
            updated += L';';
        updated += current;
        changed = true;
    }

    if (changed || installed != current)
    {
        RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ,
                       reinterpret_cast<const BYTE *>(updated.c_str()),
                       static_cast<DWORD>((updated.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(key, L"CL_WRAPPER_DIR", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(current.c_str()),
                       static_cast<DWORD>((current.size() + 1) * sizeof(wchar_t)));
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            reinterpret_cast<LPARAM>(L"Environment"),
                            SMTO_ABORTIFHUNG, 1000, nullptr);
    }
    RegCloseKey(key);
}

int wmain()
{
    sync_user_path();

    wchar_t programFiles[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", programFiles, MAX_PATH))
        return fail(L"Cannot find Program Files (x86).");

    const auto vswhere = std::filesystem::path(programFiles) /
                         L"Microsoft Visual Studio/Installer/vswhere.exe";
    std::string output;
    if (!capture(L"\"\"" + vswhere.wstring() +
                     L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
                     L" -utf8 -property installationPath\"",
                 output))
        return fail(L"Cannot locate Visual Studio with x64 C++ tools.");

    while (!output.empty() && (output.back() == '\r' || output.back() == '\n'))
        output.pop_back();
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           output.data(), static_cast<int>(output.size()), nullptr, 0);
    if (!length)
        return fail(L"Visual Studio C++ tools are not installed.");
    std::wstring installation(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output.data(),
                        static_cast<int>(output.size()), installation.data(), length);

    const auto setup = std::filesystem::path(installation) /
                       L"VC/Auxiliary/Build/vcvars64.bat";
    const auto local = variable(L"LOCALAPPDATA");
    const auto cache = std::filesystem::path(local) / L"cl-wrapper/environment-v1.bin";
    const uint64_t key = cache_key(installation);
    std::wstring environment;
    if (local.empty() || !key || !load_cache(cache, key, environment))
    {
        wchar_t system[MAX_PATH];
        if (!GetSystemDirectoryW(system, MAX_PATH))
            return fail(L"Cannot locate cmd.exe.");
        const auto cmd = std::filesystem::path(system) / L"cmd.exe";
        output.clear();
        if (!capture(L"\"\"" + cmd.wstring() + L"\" /d /u /s /c \"\"" +
                         setup.wstring() + L"\" >nul && set\"\"",
                     output))
            return fail(L"Could not initialize the x64 compiler environment.");

        // cmd /u writes UTF-16. Import its environment into this process.
        if (output.size() % sizeof(wchar_t))
            return fail(L"Invalid environment output.");
        environment.resize(output.size() / sizeof(wchar_t));
        std::memcpy(environment.data(), output.data(), output.size());
        // Save only changes made by vcvars64, not unrelated inherited secrets/settings.
        environment = changed_variables(environment);
        if (!local.empty() && key && valid_environment(environment))
            save_cache(cache, key, environment);
    }
    size_t start = 0;
    while (start < environment.size())
    {
        const size_t end = environment.find(L'\n', start);
        std::wstring line = environment.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        const size_t equals = line.find(L'=');
        if (equals != std::wstring::npos && equals != 0)
        {
            const auto name = line.substr(0, equals);
            if (!SetEnvironmentVariableW(name.c_str(), line.c_str() + equals + 1))
                return fail(L"Could not import the compiler environment.");
        }
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }

    wchar_t tools[32768];
    const DWORD size = GetEnvironmentVariableW(L"VCToolsInstallDir", tools, 32768);
    if (!size || size >= 32768)
        return fail(L"VCToolsInstallDir was not initialized.");
    const auto compiler = std::filesystem::path(tools) / L"bin/Hostx64/x64/cl.exe";
    // Keep the original argument text, including spaces, quotes and response files.
    const wchar_t *tail = GetCommandLineW();
    if (*tail == L'"')
    {
        ++tail;
        while (*tail && *tail != L'"')
            ++tail;
        if (*tail)
            ++tail;
    }
    else
    {
        while (*tail && *tail != L' ' && *tail != L'\t')
            ++tail;
    }
    std::wstring command = L"\"" + compiler.wstring() + L"\"" + tail;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(compiler.c_str(), command.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &startup, &process))
        return fail(L"Could not start the real cl.exe.");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(result);
}