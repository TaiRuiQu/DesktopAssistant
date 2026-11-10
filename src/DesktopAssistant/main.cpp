#include "App.h"
#include "Lifecycle.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        int argc{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool diagnostics = false;
        std::filesystem::path output = L"artifacts/ui";
        std::wstring fixture;
        std::filesystem::path shutdownExecutable;
        for (int i = 1; i < argc; ++i)
        {
            if (std::wstring(argv[i]) == L"--ui-checks")
                diagnostics = true;
            else if (std::wstring(argv[i]) == L"--outputdir" && i + 1 < argc)
                output = argv[++i];
            else if (std::wstring(argv[i]) == L"--fixture-url" && i + 1 < argc)
                fixture = argv[++i];
            else if (std::wstring(argv[i]) == L"--shutdown-running" && i + 1 < argc)
                shutdownExecutable = argv[++i];
        }
        LocalFree(argv);
        if (!shutdownExecutable.empty())
            return da::ShutdownRunningApplication(shutdownExecutable);
        if (!fixture.empty())
        {
            auto endpoint = da::ParseEndpoint(fixture);
            if (endpoint.secure || endpoint.host != L"127.0.0.1" || !diagnostics)
                throw da::Error(L"测试服务仅允许诊断模式下的本机回环地址。");
        }
        wchar_t user[256]{};
        DWORD size = 256;
        GetUserNameW(user, &size);
        HANDLE mutex =
            diagnostics ? nullptr
                        : CreateMutexW(nullptr,
                                       TRUE,
                                       (L"Local\\DesktopAssistant-" + std::wstring(user)).c_str());
        if (!diagnostics && (!mutex || GetLastError() == ERROR_ALREADY_EXISTS))
        {
            MessageBoxW(nullptr,
                        L"desktop assistant 已在运行，请从系统托盘打开设置。",
                        L"desktop assistant",
                        MB_OK);
            if (mutex)
                CloseHandle(mutex);
            return 0;
        }
        int result = 0;
        std::unique_ptr<da::Application> controller;
        da::ux::Application::Start(
            [&](auto const&)
            {
                winrt::make<da::XamlApplication>(
                    [&]
                    {
                        try
                        {
                            // WinUI's control metadata must be initialized after Application
                            // construction and before loading the embedded XAML views.
                            da::ux::XamlTypeInfo::XamlControlsXamlMetaDataProvider::Initialize();
                            da::ux::Application::Current().Resources().MergedDictionaries().Append(
                                da::controls::XamlControlsResources());
                            controller = std::make_unique<da::Application>(instance, diagnostics);
                            controller->Run(output,
                                            fixture,
                                            [&](int code)
                                            {
                                                result = code;
                                            });
                        }
                        catch (...)
                        {
                            result = 1;
                            auto message = da::ErrorText();
                            if (diagnostics)
                            {
                                try
                                {
                                    std::filesystem::create_directories(output);
                                    da::AtomicWrite(output / L"startup-error.txt",
                                                    da::Utf8(message));
                                }
                                catch (...)
                                {
                                }
                            }
                            else
                                MessageBoxW(nullptr,
                                            message.c_str(),
                                            L"desktop assistant",
                                            MB_OK | MB_ICONERROR);
                            da::ux::Application::Current().Exit();
                        }
                    });
            });
        controller.reset();
        if (mutex)
            CloseHandle(mutex);
        return result;
    }
    catch (...)
    {
        MessageBoxW(nullptr, da::ErrorText().c_str(), L"desktop assistant", MB_OK | MB_ICONERROR);
        return 1;
    }
}
