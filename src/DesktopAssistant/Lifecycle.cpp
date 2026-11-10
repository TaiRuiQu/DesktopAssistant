#include "Lifecycle.h"

namespace da
{
namespace
{
struct RunningApplication
{
    std::wstring executable;
    HWND controller{};
    DWORD processId{};
};

BOOL CALLBACK FindController(HWND window, LPARAM parameter)
{
    auto& application = *reinterpret_cast<RunningApplication*>(parameter);
    wchar_t title[128]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (std::wstring_view(title) != L"DesktopAssistant.Controller")
        return TRUE;

    DWORD processId{};
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return TRUE;

    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    bool matched = QueryFullProcessImageNameW(process, 0, path.data(), &size) &&
                   _wcsicmp(path.c_str(), application.executable.c_str()) == 0;
    CloseHandle(process);
    if (!matched)
        return TRUE;

    application.controller = window;
    application.processId = processId;
    return FALSE;
}
} // namespace

int ShutdownRunningApplication(const std::filesystem::path& executable)
{
    RunningApplication application{
        std::filesystem::absolute(executable).lexically_normal().wstring()};
    EnumWindows(FindController, reinterpret_cast<LPARAM>(&application));
    if (!application.controller)
        return 0;

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, application.processId);
    if (!process)
        throw Error(L"无法等待正在运行的应用退出。");

    constexpr WPARAM exitTrayCommand = 205;
    bool requested = PostMessageW(application.controller, WM_COMMAND, exitTrayCommand, 0);
    DWORD status = requested ? WaitForSingleObject(process, 10000) : WAIT_FAILED;
    CloseHandle(process);
    if (status != WAIT_OBJECT_0)
        throw Error(L"应用没有在 10 秒内正常退出，请从托盘菜单退出后重试。");
    return 0;
}
} // namespace da
