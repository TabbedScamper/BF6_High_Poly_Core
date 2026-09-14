#include "rime_state_process.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <sstream>
#include <cstdlib>
#include <utility>

namespace rime_state {
namespace {

std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count)
        return {};
    return result;
}

std::wstring quoted(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value)
    {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(ch);
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

void append_arg(std::wstring& command, const std::string& value)
{
    command.push_back(L' ');
    command += quoted(widen(value));
}

std::string windows_error(const char* operation)
{
    std::ostringstream out;
    out << operation << " failed (Win32 " << GetLastError() << ')';
    return out.str();
}

std::string environment(const char* name)
{
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string result(value);
    std::free(value);
    return result;
}

bool exists(const std::string& path)
{
    const std::wstring wide = widen(path);
    return !wide.empty() && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<std::string> split_hosts(const std::string& value)
{
    std::vector<std::string> result;
    size_t begin = 0;
    for (;;)
    {
        const size_t end = value.find('|', begin);
        result.push_back(value.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

} // namespace

bool discover_live_capture_options(const std::string& game_directory,
                                   const std::string& route,
                                   LiveCaptureOptions& options,
                                   std::string& error)
{
    options = LiveCaptureOptions{};
    error.clear();
    options.game_directory = game_directory;
    options.route = route;
    if (environment("BF6_UI_STATE_DISABLE") == "1")
    {
        error = "disabled by BF6_UI_STATE_DISABLE";
        return false;
    }
    options.python_executable = environment("BF6_UI_STATE_PYTHON");
    if (options.python_executable.empty())
    {
        const std::string local = environment("LOCALAPPDATA");
        if (!local.empty())
            options.python_executable =
                local + "\\Programs\\Python\\Python312\\python.exe";
    }
    options.capture_tool = environment("BF6_UI_STATE_TOOL");
#ifdef BF6_UI_STATE_TOOL_DEFAULT
    if (options.capture_tool.empty())
        options.capture_tool = BF6_UI_STATE_TOOL_DEFAULT;
#endif
    const std::string hosts = environment("BF6_UI_STATE_HOSTS");
    if (!hosts.empty())
        for (const std::string& host : split_hosts(hosts))
            if (!host.empty()) options.explicit_host_inputs.push_back(host);
    if (!exists(options.python_executable))
    {
        error = "UI state Python executable not found";
        return false;
    }
    if (!exists(options.capture_tool))
    {
        error = "UI state capture tool not found";
        return false;
    }
    return true;
}

bool capture_and_apply(rime::Screen& screen, const LiveCaptureOptions& options,
                       LiveCaptureReport& report, std::string& error)
{
    report = LiveCaptureReport{};
    error.clear();
    if (options.python_executable.empty() || options.capture_tool.empty() ||
        options.game_directory.empty() || options.route.empty())
    {
        error = "incomplete live Rime state command";
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
    { error = windows_error("CreatePipe"); return false; }
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0))
    {
        error = windows_error("SetHandleInformation");
        CloseHandle(read_pipe); CloseHandle(write_pipe);
        return false;
    }

    std::wstring command = quoted(widen(options.python_executable));
    append_arg(command, options.capture_tool);
    command += L" --game"; append_arg(command, options.game_directory);
    command += L" --route"; append_arg(command, options.route);
    command += L" --max-depth ";
    command += std::to_wstring(options.max_depth);
    for (const std::string& host : options.explicit_host_inputs)
    {
        command += L" --host";
        append_arg(command, host);
    }
    command += L" --wire";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL started = CreateProcessW(
        widen(options.python_executable).c_str(), mutable_command.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process);
    CloseHandle(write_pipe);
    if (!started)
    {
        error = windows_error("CreateProcessW");
        CloseHandle(read_pipe);
        return false;
    }
    report.process_started = true;

    std::string bytes;
    char buffer[16384];
    DWORD received = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &received, nullptr) && received)
        bytes.append(buffer, buffer + received);
    CloseHandle(read_pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &report.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (report.exit_code != 0)
    {
        std::ostringstream out;
        out << "UI state capture exited " << report.exit_code;
        error = out.str();
        return false;
    }

    std::istringstream stream(bytes);
    std::vector<StateSlot> slots;
    if (!read_wire(stream, slots, report.wire, error)) return false;
    if (report.wire.route != options.route || screen.partition != options.route)
    {
        error = "UI state route does not match renderer screen";
        return false;
    }
    report.applied = apply_state(screen, slots);
    return true;
}

} // namespace rime_state
