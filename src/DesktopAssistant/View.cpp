#include "App.h"
#include "MarkdownView.h"
#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Text.h>
#include <sstream>
#include <iomanip>

namespace da
{
namespace
{
enum
{
    Send = 100,
    Finish,
    CancelButton,
    SettingsButton,
    Copy,
    HideAnswer,
    Save,
    CancelSettings
};

ux::FrameworkElement LoadView(HINSTANCE instance, int resource)
{
    auto info = FindResourceW(instance, MAKEINTRESOURCEW(resource), RT_RCDATA);
    auto data = LoadResource(instance, info);
    if (!data)
        throw Error(L"无法读取 WinUI 界面资源。");
    auto bytes = static_cast<const char*>(LockResource(data));
    auto size = SizeofResource(instance, info);
    return ux::Markup::XamlReader::Load(Wide({bytes, size})).as<ux::FrameworkElement>();
}

template <class T> T Named(ux::FrameworkElement const& root, const wchar_t* name)
{
    auto value = root.FindName(name);
    if (!value)
        throw Error(L"未找到 WinUI 控件：" + std::wstring(name));
    return value.as<T>();
}

foundation::IAsyncAction Pause(std::chrono::milliseconds duration)
{
    winrt::apartment_context ui;
    co_await winrt::resume_after(duration);
    co_await ui;
}

foundation::IAsyncAction Preview(ux::FrameworkElement root, std::filesystem::path path)
{
    ux::Media::Imaging::RenderTargetBitmap bitmap;
    co_await bitmap.RenderAsync(root);
    auto buffer = co_await bitmap.GetPixelsAsync();
    Bytes pixels(buffer.Length());
    winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer).ReadBytes(pixels);
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
    using namespace winrt::Windows::Graphics::Imaging;
    auto encoder = co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), stream);
    encoder.SetPixelData(BitmapPixelFormat::Bgra8,
                         BitmapAlphaMode::Premultiplied,
                         bitmap.PixelWidth(),
                         bitmap.PixelHeight(),
                         96,
                         96,
                         pixels);
    co_await encoder.FlushAsync();
    stream.Seek(0);
    winrt::Windows::Storage::Streams::DataReader reader(stream);
    co_await reader.LoadAsync(static_cast<uint32_t>(stream.Size()));
    Bytes encoded(static_cast<size_t>(stream.Size()));
    reader.ReadBytes(encoded);
    AtomicWrite(path, {reinterpret_cast<const char*>(encoded.data()), encoded.size()});
}
} // namespace

void Application::SetAnswer(std::wstring text, bool preserveSelection)
{
    displayedAnswer_ = std::move(text);
    // Keep the user's selection while tokens arrive; refresh when it is released.
    if (preserveSelection && !answerBody_.SelectedText().empty())
        return;
    auto document = ParseMarkdown(displayedAnswer_);
    answerPlainText_ = MarkdownText(document);
    auto blocks = RenderMarkdown(document);
    size_t common = 0;
    while (common < blocks.size() && common < answerBlocks_.size() &&
           blocks[common].key == answerBlocks_[common])
        ++common;
    auto collection = answerBody_.Blocks();
    while (collection.Size() > common)
        collection.RemoveAtEnd();
    for (size_t i = common; i < blocks.size(); ++i)
        collection.Append(blocks[i].paragraph);
    answerBlocks_.clear();
    for (auto const& block : blocks)
        answerBlocks_.push_back(block.key);
    ResizeMarkdown(answerBody_);
    if (displayedAnswer_.empty())
        Named<controls::ScrollViewer>(conversationRoot_, L"AnswerScroll")
            .ChangeView(
                nullptr, winrt::box_value(0.).as<foundation::IReference<double>>(), nullptr, true);
}

void Application::CreateMainWindow()
{
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance_;
    cls.lpfnWndProc = Procedure;
    cls.lpszClassName = L"DesktopAssistant.Controller";
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw Error(L"无法创建托盘控制器。");
    host_ = CreateWindowExW(0,
                            cls.lpszClassName,
                            L"DesktopAssistant.Controller",
                            0,
                            0,
                            0,
                            0,
                            0,
                            nullptr,
                            nullptr,
                            instance_,
                            this);
    window_ = ux::Window();
    window_.SystemBackdrop(ux::Media::MicaBackdrop());
    window_.Title(L"问答 · desktop assistant");
    conversationRoot_ = LoadView(instance_, 102);
    settingsRoot_ = LoadView(instance_, 103);
    settingsRoot_.Visibility(ux::Visibility::Collapsed);
    controls::Grid shell;
    shell.Children().Append(conversationRoot_);
    shell.Children().Append(settingsRoot_);
    window_.Content(shell);
    Check(window_.as<IWindowNative>()->get_WindowHandle(&panel_));
    questionEdit_ = Named<controls::TextBox>(conversationRoot_, L"QuestionBox");
    answerBody_ = Named<controls::RichTextBlock>(conversationRoot_, L"AnswerBody");
    answerBody_.SizeChanged(
        [this](auto const&, auto const&)
        {
            ResizeMarkdown(answerBody_);
        });
    Named<controls::ScrollViewer>(conversationRoot_, L"AnswerScroll")
        .SizeChanged(
            [this](auto const& sender, ux::SizeChangedEventArgs const& args)
            {
                auto scroll = sender.template as<controls::ScrollViewer>();
                auto padding = scroll.Padding();
                answerBody_.Width(
                    std::max(32., args.NewSize().Width - padding.Left - padding.Right));
                ResizeMarkdown(answerBody_);
            });
    submittedQuestion_ = Named<controls::TextBlock>(conversationRoot_, L"SubmittedQuestion");
    answerBody_.SelectionChanged(
        [this](auto const&, auto const&)
        {
            if (answerBody_.SelectedText().empty())
                Post(
                    [this]
                    {
                        SetAnswer(displayedAnswer_, true);
                    });
        });
    status_ = Named<controls::TextBlock>(conversationRoot_, L"StatusLabel");
    answerStatus_ = Named<controls::TextBlock>(conversationRoot_, L"AnswerStatus");
    send_ = Named<controls::Button>(conversationRoot_, L"SendButton");
    finishButton_ = Named<controls::Button>(conversationRoot_, L"FinishButton");
    cancel_ = Named<controls::Button>(conversationRoot_, L"CancelButton");
    settingsButton_ = Named<controls::Button>(conversationRoot_, L"SettingsButton");
    activity_ = Named<controls::ProgressRing>(conversationRoot_, L"ActivityRing");
    auto wire = [this](auto root, const wchar_t* name, int command)
    {
        Named<controls::Button>(root, name)
            .Click(
                [this, command](auto const&, auto const&)
                {
                    Action(command);
                });
    };
    wire(conversationRoot_, L"SendButton", Send);
    wire(conversationRoot_, L"FinishButton", Finish);
    wire(conversationRoot_, L"CancelButton", CancelButton);
    wire(conversationRoot_, L"SettingsButton", SettingsButton);
    wire(conversationRoot_, L"CopyButton", Copy);
    wire(conversationRoot_, L"HideButton", HideAnswer);
    Named<controls::Button>(conversationRoot_, L"VoiceButton")
        .Click(
            [this](auto const&, auto const&)
            {
                Start(true);
            });
    Named<controls::Button>(conversationRoot_, L"SpeakButton")
        .Click(
            [this](auto const&, auto const&)
            {
                ToggleSpeech();
            });
    Named<controls::Button>(conversationRoot_, L"EditQuestionButton")
        .Click(
            [this](auto const&, auto const&)
            {
                if (!busy_)
                {
                    if (!editingQuestion_)
                        SetQuestion(L"");
                    EditQuestion(true);
                }
            });
    Named<controls::Button>(conversationRoot_, L"CopyQuestionButton")
        .Click(
            [this](auto const&, auto const&)
            {
                CopyText(editingQuestion_ ? questionEdit_.Text() : submittedQuestion_.Text());
            });
    wire(settingsRoot_, L"SaveSettings", Save);
    wire(settingsRoot_, L"CancelSettings", CancelSettings);
    questionEdit_.TextChanged(
        [this](auto const&, auto const&)
        {
            if (!suppressEdit_)
                ++revision_;
        });
    auto shortcut = [this](ux::FrameworkElement root,
                           winrt::Windows::System::VirtualKey key,
                           winrt::Windows::System::VirtualKeyModifiers modifiers,
                           std::function<void()> action)
    {
        ux::Input::KeyboardAccelerator accelerator;
        accelerator.Key(key);
        accelerator.Modifiers(modifiers);
        accelerator.Invoked(
            [this, root, action = std::move(action)](
                auto const&, ux::Input::KeyboardAcceleratorInvokedEventArgs const& args)
            {
                if (root.Visibility() != ux::Visibility::Visible)
                    return;
                args.Handled(true);
                try
                {
                    action();
                }
                catch (...)
                {
                    Notice(ErrorText());
                }
            });
        root.KeyboardAccelerators().Append(accelerator);
    };
    using Key = winrt::Windows::System::VirtualKey;
    using Mod = winrt::Windows::System::VirtualKeyModifiers;
    shortcut(conversationRoot_,
             Key::Enter,
             Mod::Control,
             [this]
             {
                 if (editingQuestion_)
                     Start(false);
             });
    shortcut(conversationRoot_,
             Key::Escape,
             Mod::None,
             [this]
             {
                 Dismiss();
             });
    shortcut(settingsRoot_,
             Key::Escape,
             Mod::None,
             [this]
             {
                 CloseSettings();
             });
    auto appWindow = window_.AppWindow();
    auto presenter = windowing::OverlappedPresenter::Create();
    presenter.IsAlwaysOnTop(true);
    presenter.IsMaximizable(false);
    presenter.IsMinimizable(false);
    presenter.SetBorderAndTitleBar(true, false);
    appWindow.SetPresenter(presenter);
    appWindow.IsShownInSwitchers(true);
    SetWindowSubclass(panel_, WindowProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    appWindow.Closing(
        [this](auto const&, windowing::AppWindowClosingEventArgs const& args)
        {
            if (exiting_)
                return;
            args.Cancel(true);
            if (showingSettings_)
                CloseSettings();
            else
                Dismiss();
        });
    SendMessageW(panel_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_));
    SendMessageW(panel_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
    int dpi = static_cast<int>(GetDpiForWindow(panel_));
    appWindow.Resize({MulDiv(720, dpi, 96), MulDiv(740, dpi, 96)});
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
}

void Application::AddText(int page,
                          const wchar_t* key,
                          const wchar_t*,
                          std::wstring Settings::*member,
                          bool password,
                          bool)
{
    Field f;
    f.page = page;
    f.key = key;
    f.password = password;
    f.control = Named<controls::Control>(settingsRoot_, key);
    f.control.FontFamily(ux::Media::FontFamily(L"Microsoft YaHei UI"));
    f.read = [member](const Settings& s)
    {
        return s.*member;
    };
    f.write = [member](Settings& s, std::wstring value)
    {
        s.*member = Trim(std::move(value));
    };
    fields_.push_back(std::move(f));
}

void Application::AddNumber(int page,
                            const wchar_t* key,
                            const wchar_t*,
                            std::function<double(const Settings&)> read,
                            std::function<void(Settings&, double)> write)
{
    Field f;
    f.page = page;
    f.key = key;
    f.number = true;
    f.control = Named<controls::Control>(settingsRoot_, key);
    f.read = [read](const Settings& s)
    {
        std::wostringstream out;
        out << std::setprecision(8) << read(s);
        return out.str();
    };
    f.write = [write](Settings& s, std::wstring text)
    {
        size_t end{};
        double value;
        try
        {
            value = std::stod(text, &end);
        }
        catch (...)
        {
            throw Error(L"请填写有效数值。");
        }
        if (end != text.size() || !std::isfinite(value))
            throw Error(L"请填写有效数值。");
        write(s, value);
    };
    fields_.push_back(std::move(f));
}

void Application::AddInteger(int page, const wchar_t* key, int Settings::*member)
{
    AddNumber(
        page,
        key,
        L"",
        [member](const Settings& settings)
        {
            return static_cast<double>(settings.*member);
        },
        [member](Settings& settings, double value)
        {
            if (value != std::trunc(value) || value < INT_MIN || value > INT_MAX)
                throw Error(L"此项需要填写整数。");
            settings.*member = static_cast<int>(value);
        });
}

void Application::AddChoice(int page,
                            const wchar_t* key,
                            const wchar_t*,
                            std::wstring Settings::*member,
                            std::vector<std::pair<std::wstring, std::wstring>> choices)
{
    Field f;
    f.page = page;
    f.key = key;
    f.choices = std::move(choices);
    f.control = Named<controls::Control>(settingsRoot_, key);
    auto box = f.control.as<controls::ComboBox>();
    for (auto const& choice : f.choices)
    {
        controls::ComboBoxItem item;
        item.Content(winrt::box_value(choice.second));
        box.Items().Append(item);
    }
    f.read = [member](const Settings& s)
    {
        return s.*member;
    };
    f.write = [member](Settings& s, std::wstring value)
    {
        s.*member = std::move(value);
    };
    fields_.push_back(std::move(f));
}

void Application::AddCheck(int page, const wchar_t* key, const wchar_t*, bool Settings::*member)
{
    Field f;
    f.page = page;
    f.key = key;
    f.check = true;
    f.control = Named<controls::Control>(settingsRoot_, key);
    f.read = [member](const Settings& s)
    {
        return s.*member ? L"1" : L"0";
    };
    f.write = [member](Settings& s, std::wstring value)
    {
        s.*member = value == L"1";
    };
    fields_.push_back(std::move(f));
}

void Application::CreateSettings()
{
    navigation_ = Named<controls::NavigationView>(settingsRoot_, L"SettingsNavigation");
    settingsError_ = Named<controls::InfoBar>(settingsRoot_, L"SettingsError");
    pages_ = {Named<ux::FrameworkElement>(settingsRoot_, L"ModelPage"),
              Named<ux::FrameworkElement>(settingsRoot_, L"AudioPage"),
              Named<ux::FrameworkElement>(settingsRoot_, L"ActivationPage"),
              Named<ux::FrameworkElement>(settingsRoot_, L"AdvancedPage"),
              Named<ux::FrameworkElement>(settingsRoot_, L"TtsPage")};
    navigation_.SelectionChanged(
        [this](auto const&, controls::NavigationViewSelectionChangedEventArgs const& args)
        {
            if (!args.SelectedItem())
                return;
            auto tag = args.SelectedItem().as<controls::NavigationViewItem>().Tag();
            SelectPage(std::stoi(std::wstring(winrt::unbox_value<winrt::hstring>(tag))));
        });
    AddText(0, L"BaseUrl", L"", &Settings::BaseUrl);
    AddText(0, L"ApiKey", L"", &Settings::ApiKey, true);
    AddText(0, L"AnswerModel", L"", &Settings::AnswerModel);
    AddCheck(0, L"SendScreenshots", L"", &Settings::SendScreenshots);
    AddText(1, L"SpeechBaseUrl", L"", &Settings::SpeechBaseUrl);
    AddText(1, L"SpeechApiKey", L"", &Settings::SpeechApiKey, true);
    AddChoice(1,
              L"SpeechApiMode",
              L"",
              &Settings::SpeechApiMode,
              {{L"Transcriptions", L"Audio Transcriptions"},
               {L"ChatCompletions", L"Chat Completions（音频输入）"},
               {L"DashScope", L"DashScope Qwen-ASR"}});
    AddText(4, L"TtsBaseUrl", L"", &Settings::TtsBaseUrl);
    AddText(4, L"TtsApiKey", L"", &Settings::TtsApiKey, true);
    AddText(4, L"TtsModel", L"", &Settings::TtsModel);
    AddText(4, L"TtsVoice", L"", &Settings::TtsVoice);
    AddChoice(0,
              L"ApiMode",
              L"",
              &Settings::ApiMode,
              {{L"Responses", L"Responses"}, {L"ChatCompletions", L"Chat Completions"}});
    AddChoice(1,
              L"DesktopSpeechProvider",
              L"",
              &Settings::DesktopSpeechProvider,
              {{L"Windows", L"Windows 本机"}, {L"OpenAI", L"云端 API"}});
    AddText(1, L"DesktopTranscriptionModel", L"", &Settings::DesktopTranscriptionModel);
    AddChoice(1,
              L"MicrophoneSpeechProvider",
              L"",
              &Settings::MicrophoneSpeechProvider,
              {{L"Windows", L"Windows 本机"}, {L"OpenAI", L"云端 API"}});
    AddText(1, L"MicrophoneTranscriptionModel", L"", &Settings::MicrophoneTranscriptionModel);
    std::vector<std::pair<std::wstring, std::wstring>> languages;
    try
    {
        languages = SpeechLanguages();
    }
    catch (...)
    {
    }
    if (languages.empty())
        languages = {{L"zh-CN", L"zh-CN（未检测到本机识别器）"}};
    AddChoice(1, L"SpeechLanguage", L"", &Settings::SpeechLanguage, std::move(languages));
    AddCheck(1, L"CaptureDesktopAudio", L"", &Settings::CaptureDesktopAudio);
    AddCheck(1, L"DuckOtherAudio", L"", &Settings::DuckOtherAudio);
    AddCheck(1, L"WakeWordEnabled", L"", &Settings::WakeWordEnabled);
    AddText(1, L"WakePhrase", L"", &Settings::WakePhrase);
    AddInteger(1, L"AudioLookbackSeconds", &Settings::AudioLookbackSeconds);
    AddNumber(
        1,
        L"SilenceSeconds",
        L"",
        [](const Settings& s)
        {
            return s.SilenceSeconds;
        },
        [](Settings& s, double v)
        {
            s.SilenceSeconds = v;
        });
    AddChoice(2,
              L"ActivationKey",
              L"",
              &Settings::ActivationKey,
              {{L"RightShift", L"右 Shift"},
               {L"LeftShift", L"左 Shift"},
               {L"RightControl", L"右 Ctrl"},
               {L"LeftControl", L"左 Ctrl"},
               {L"RightAlt", L"右 Alt"},
               {L"F8", L"F8"},
               {L"F9", L"F9"}});
    AddInteger(2, L"DoubleTapMilliseconds", &Settings::DoubleTapMilliseconds);
    AddInteger(2, L"HistoryTurns", &Settings::HistoryTurns);
    AddCheck(2, L"SaveHistory", L"", &Settings::SaveHistory);
    AddCheck(2, L"StartMinimized", L"", &Settings::StartMinimized);
    AddInteger(3, L"MaxRecordingSeconds", &Settings::MaxRecordingSeconds);
    AddInteger(3, L"ScreenshotMaxDimension", &Settings::ScreenshotMaxDimension);
    AddNumber(
        3,
        L"VoiceThreshold",
        L"",
        [](const Settings& s)
        {
            return s.VoiceThreshold;
        },
        [](Settings& s, double v)
        {
            s.VoiceThreshold = v;
        });
    AddText(3, L"SystemPrompt", L"", &Settings::SystemPrompt, false, true);
    UpdateFields();
    navigation_.SelectedItem(navigation_.MenuItems().GetAt(0));
}

void Application::SelectPage(int page)
{
    if (page < 0 || page >= static_cast<int>(pages_.size()))
        return;
    for (size_t i = 0; i < pages_.size(); ++i)
        pages_[i].Visibility(i == page ? ux::Visibility::Visible : ux::Visibility::Collapsed);
    auto scroll = Named<controls::ScrollViewer>(settingsRoot_, L"SettingsScroll");
    scroll.ChangeView(
        nullptr, winrt::box_value(0.).as<foundation::IReference<double>>(), nullptr, true);
}

void Application::UpdateFields()
{
    for (auto& f : fields_)
    {
        auto value = f.read(settings_);
        if (f.check)
            f.control.as<controls::ToggleSwitch>().IsOn(value == L"1");
        else if (f.password)
            f.control.as<controls::PasswordBox>().Password(value);
        else if (f.number)
            f.control.as<controls::NumberBox>().Value(std::stod(value));
        else if (!f.choices.empty())
        {
            int index = -1;
            for (size_t i = 0; i < f.choices.size(); ++i)
                if (f.choices[i].first == value)
                    index = static_cast<int>(i);
            auto box = f.control.as<controls::ComboBox>();
            if (index < 0)
            {
                f.choices.emplace_back(value, value + L"（当前配置）");
                controls::ComboBoxItem item;
                item.Content(winrt::box_value(f.choices.back().second));
                box.Items().Append(item);
                index = static_cast<int>(f.choices.size() - 1);
            }
            box.SelectedIndex(index);
        }
        else
            f.control.as<controls::TextBox>().Text(value);
    }
}

void Application::SaveSettingsForm()
{
    Settings candidate = settings_;
    for (auto const& f : fields_)
    {
        std::wstring value;
        try
        {
            if (f.check)
                value = f.control.as<controls::ToggleSwitch>().IsOn() ? L"1" : L"0";
            else if (f.password)
                value = f.control.as<controls::PasswordBox>().Password();
            else if (f.number)
            {
                double n = f.control.as<controls::NumberBox>().Value();
                if (!std::isfinite(n))
                    throw Error(L"请填写有效数值。");
                std::wostringstream text;
                text << std::setprecision(12) << n;
                value = text.str();
            }
            else if (!f.choices.empty())
            {
                int index = f.control.as<controls::ComboBox>().SelectedIndex();
                if (index < 0)
                    throw Error(L"请选择有效选项。");
                value = f.choices.at(index).first;
            }
            else
                value = f.control.as<controls::TextBox>().Text();
            f.write(candidate, std::move(value));
        }
        catch (...)
        {
            navigation_.SelectedItem(navigation_.MenuItems().GetAt(f.page));
            f.control.StartBringIntoView();
            f.control.Focus(ux::FocusState::Programmatic);
            throw;
        }
    }
    Validate(candidate);
    if (candidate.WakeWordEnabled)
    {
        auto languages = SpeechLanguages();
        if (std::none_of(languages.begin(),
                         languages.end(),
                         [&](auto const& language)
                         {
                             return language.first == candidate.SpeechLanguage;
                         }))
        {
            navigation_.SelectedItem(navigation_.MenuItems().GetAt(1));
            throw Error(L"语音唤醒需要 Windows 本机语音组件：" + candidate.SpeechLanguage +
                        L"。请先安装对应语言的语音识别组件。");
        }
    }
    if (!diagnostics_)
        SaveSettings(candidate);
    settings_ = std::move(candidate);
    if (!settings_.SaveHistory && !diagnostics_)
        ClearHistory();
    else
        Persist();
    ApplySettings();
    CloseSettings();
}

winrt::fire_and_forget Application::Diagnostics(std::filesystem::path output,
                                                std::wstring fixture,
                                                std::function<void(int)> completed)
{
    JsonArray errors;

    int checks = 0;
    auto expect = [&](bool condition, const wchar_t* message)
    {
        ++checks;
        if (!condition)
            errors.Append(J(message));
    };
    auto visibleWindows = []
    {
        int count = 0;
        EnumWindows(
            [](HWND window, LPARAM data) -> BOOL
            {
                DWORD process{};
                GetWindowThreadProcessId(window, &process);
                wchar_t title[256]{};
                GetWindowTextW(window, title, 256);
                if (process == GetCurrentProcessId() && IsWindowVisible(window) &&
                    std::wstring_view(title).find(L" · desktop assistant") !=
                        std::wstring_view::npos)
                    ++*reinterpret_cast<int*>(data);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&count));
        return count;
    };
    try
    {
        std::filesystem::create_directories(output);
        expect(IsWindow(panel_), L"one native WinUI window created");
        expect(winrt::get_class_name(questionEdit_) == L"Microsoft.UI.Xaml.Controls.TextBox",
               L"question is a WinUI 3 TextBox");
        expect(winrt::get_class_name(answerBody_) == L"Microsoft.UI.Xaml.Controls.RichTextBlock",
               L"answer uses selectable WinUI rich text");
        expect(fields_.size() == 32, L"all independent API and wake settings use WinUI controls");
        expect(!hook_ && !operation_.joinable(), L"diagnostics skip capture and external requests");
        SetQuestion(L"HTTP/3 为什么比 HTTP/2 快？用三点说清楚。");
        submittedQuestion_.Text(questionEdit_.Text());
        answerText_ =
            L"HTTP/3 更快，关键不在版本号，而在它把传输层从 TCP 换成了跑在 UDP 之上的 QUIC。\n\n"
            L"拆开看，主要是三件事：\n\n"
            L"1. **队头阻塞被拆到每条流**\nHTTP/2 的多路复用仍挤在一条 TCP "
            L"连接里，一个包丢失会拖住后面所有流；QUIC 让每条流独立重传，丢包只影响它自己。\n\n"
            L"2. **握手少了一个来回**\nTLS 1.3 直接内置进 QUIC，首次连接只需 1-RTT，恢复连接还能 "
            L"0-RTT；TCP 和 TLS 的握手则是叠加的。\n\n"
            L"3. **连接可以跟着你迁移**\nQUIC 用连接 ID 标识会话，Wi-Fi "
            L"切到蜂窝网络时不用重新握手，正在进行的请求也不会中断。\n\n"
            L"### 适用范围\n\n收益最明显的是弱网、移动网络和高延迟链路；在稳定的光纤环境里，HTTP/2 "
            L"和 HTTP/3 的差距可能很小。";
        SetAnswer(answerText_);
        ShowConversation();
        Status(L"截图已就绪 · WinUI 3 界面预览");
        answerStatus_.Text(L"回答完成");
        auto dpi = static_cast<int>(GetDpiForWindow(panel_));
        window_.AppWindow().Resize({MulDiv(720, dpi, 96), MulDiv(742, dpi, 96)});
        co_await Pause(std::chrono::milliseconds(150));
        expect(IsWindowVisible(panel_) && !showingSettings_,
               L"unified question and answer window visible");
        auto nativeWindow = panel_;
        expect(visibleWindows() == 1 && settingsRoot_.Visibility() == ux::Visibility::Collapsed,
               L"question entry and answer share exactly one visible window");
        expect(conversationRoot_.ActualWidth() > 0 && conversationRoot_.ActualHeight() > 0,
               L"WinUI floating layouts measured");
        co_await Preview(conversationRoot_, output / L"question.png");
        SaveWindowPreview(panel_, output / L"question-native.png");
        EditQuestion(false);
        co_await Pause(std::chrono::milliseconds(80));
        co_await Preview(conversationRoot_, output / L"answer.png");
        expect(answerBody_.Blocks().Size() == 7,
               L"answer renders lead, list paragraphs and section heading");
        auto first = answerBody_.Blocks().GetAt(0);
        SetAnswer(answerText_ + L"\n\n**补充**：`QUIC` 是传输协议。", true);
        expect(answerBody_.Blocks().GetAt(0) == first && answerBody_.Blocks().Size() == 8,
               L"streaming appends rich text without replacing completed paragraphs");
        SetQuestion(L"下一轮尚未发送的问题");
        expect(submittedQuestion_.Text() == L"HTTP/3 为什么比 HTTP/2 快？用三点说清楚。",
               L"editing the next question preserves submitted question snapshot");
        window_.AppWindow().Resize({MulDiv(520, dpi, 96), MulDiv(420, dpi, 96)});
        co_await Pause(std::chrono::milliseconds(100));
        auto scroll = Named<controls::ScrollViewer>(conversationRoot_, L"AnswerScroll");
        expect(scroll.ScrollableHeight() > 0 && scroll.ActualHeight() > 80,
               L"compact answer scrolls while footer stays visible");
        co_await Preview(conversationRoot_, output / L"answer-compact.png");
        SetAnswer(L"```cpp\nstd::string marker = \"**literal**\";\n```");
        auto code = answerBody_.Blocks().GetAt(0).as<ux::Documents::Paragraph>();
        expect(code.Inlines().Size() == 1 &&
                   code.Inlines().GetAt(0).try_as<ux::Documents::InlineUIContainer>() &&
                   answerPlainText_.find(L"**literal**") != std::wstring::npos,
               L"fenced code uses native code card and preserves literal characters");
        SetAnswer(answerText_);
        SetQuestion(std::wstring(submittedQuestion_.Text()));
        SetAnswer(L"## 简体中文字体\n\n| 字体名 | 英文名 | 备注 |\n| :--- | :--- | :--- |\n| "
                  L"**微软雅黑** | Microsoft YaHei | UI 首选，含 Light 字重 |\n| 等线 | DengXian | "
                  L"Windows 正文字体 |\n| 宋体 | SimSun | 经典衬线 "
                  L"|\n\n正文保持正常字重，**只有强调的文字加粗**。\n\n- 一级列表\n  - "
                  L"嵌套的中文内容\n\n> 引用段落\n\n```cpp\nstd::wstring text = "
                  L"L\"中文代码\";\n```\n\n[文档](https://example.com) 和 ~~删除线~~");
        window_.AppWindow().Resize({MulDiv(720, dpi, 96), MulDiv(820, dpi, 96)});
        co_await Pause(std::chrono::milliseconds(100));
        auto tableParagraph = answerBody_.Blocks().GetAt(1).as<ux::Documents::Paragraph>();
        expect(tableParagraph.Inlines().GetAt(0).try_as<ux::Documents::InlineUIContainer>() !=
                   nullptr,
               L"Markdown table renders as a native table instead of pipe-delimited text");
        auto tableFrame = tableParagraph.Inlines()
                              .GetAt(0)
                              .as<ux::Documents::InlineUIContainer>()
                              .Child()
                              .as<ux::FrameworkElement>();
        expect(tableFrame.Width() > 600 && tableFrame.Width() < conversationRoot_.ActualWidth(),
               L"Markdown table expands to the current viewport after window resize");
        auto listParagraph = answerBody_.Blocks().GetAt(3).as<ux::Documents::Paragraph>();
        expect(listParagraph.Inlines().Size() >= 2,
               L"tight Markdown list keeps its Chinese text after the bullet");
        expect(answerPlainText_.find(L"Microsoft YaHei") != std::wstring::npos &&
                   answerPlainText_.find(L"| :---") == std::wstring::npos,
               L"copy and speech include table text without Markdown separators");
        expect(answerBody_.FontFamily().Source() == L"Microsoft YaHei UI" &&
                   answerBody_.Blocks().GetAt(2).FontWeight().Weight == 400,
               L"Chinese body text uses explicit UI font with normal weight");
        co_await Preview(conversationRoot_, output / L"markdown.png");
        auto shell = window_.Content().as<ux::FrameworkElement>();
        shell.RequestedTheme(ux::ElementTheme::Dark);
        co_await Pause(std::chrono::milliseconds(500));
        auto darkInk = answerBody_.Foreground().as<ux::Media::SolidColorBrush>().Color();
        expect(darkInk.R > 200 && darkInk.G > 200,
               L"dark theme keeps inherited answer text legible");
        co_await Preview(conversationRoot_, output / L"markdown-dark.png");
        SaveWindowPreview(panel_, output / L"markdown-dark-native.png");
        shell.RequestedTheme(ux::ElementTheme::Light);
        co_await Pause(std::chrono::milliseconds(100));
        auto lightInk = answerBody_.Foreground().as<ux::Media::SolidColorBrush>().Color();
        expect(lightInk.R < 100 && lightInk.G < 100,
               L"light theme restores native primary text color");
        shell.RequestedTheme(ux::ElementTheme::Default);
        SetAnswer(answerText_);
        HideWindow();
        expect(!IsWindowVisible(panel_), L"unified window hides without ending application");
        OpenSettings();
        co_await Pause(std::chrono::milliseconds(100));
        expect(IsWindowVisible(panel_) && showingSettings_,
               L"settings are shown inside the same native window");
        expect(panel_ == nativeWindow && visibleWindows() == 1 &&
                   conversationRoot_.Visibility() == ux::Visibility::Collapsed,
               L"settings replace conversation without creating another window");
        for (int page = 0; page < static_cast<int>(pages_.size()); ++page)
        {
            navigation_.SelectedItem(navigation_.MenuItems().GetAt(page));
            co_await Pause(std::chrono::milliseconds(100));
            expect(pages_[page].Visibility() == ux::Visibility::Visible,
                   L"settings navigation selects correct content");
            for (auto const& f : fields_)
                if (f.page == page)
                {
                    expect(f.control.ActualWidth() > 0 && f.control.ActualHeight() > 0,
                           L"WinUI setting control is laid out");
                    expect(winrt::get_class_name(f.control).starts_with(
                               L"Microsoft.UI.Xaml.Controls."),
                           L"setting uses WinUI 3 control");
                }
            co_await Preview(settingsRoot_,
                             output / (L"settings-" + std::to_wstring(page) + L".png"));
        }
        auto savedAnswer = answerText_;
        SendMessageW(panel_, WM_CLOSE, 0, 0);
        co_await Pause(std::chrono::milliseconds(100));
        expect(!showingSettings_ && visibleWindows() == 0 && panel_ == nativeWindow &&
                   answerText_ == savedAnswer,
               L"closing settings hides to tray without opening conversation and preserves answer");
        for (int i = 0; i < 3; ++i)
        {
            OpenSettings();
            Action(CancelSettings);
        }
        expect(!showingSettings_ && visibleWindows() == 0,
               L"repeated settings dismissal never opens conversation");
        OpenSettings();
        CloseSettings();
        expect(!IsWindowVisible(panel_) && !showingSettings_,
               L"settings Escape dismissal uses the hidden conversation state");
        OpenSettings();
        Action(Save);
        expect(!IsWindowVisible(panel_) && !showingSettings_,
               L"saving settings hides to tray without opening conversation");
        OpenSettings();
        window_.AppWindow().Presenter().as<windowing::OverlappedPresenter>().Maximize();
        co_await Pause(std::chrono::milliseconds(100));
        SendMessageW(panel_, WM_CLOSE, 0, 0);
        co_await Pause(std::chrono::milliseconds(100));
        expect(!IsWindowVisible(panel_), L"closing maximized settings stays hidden");
        ShowConversation();
        expect(visibleWindows() == 1 && answerText_ == savedAnswer,
               L"explicit tray show restores preserved conversation after settings dismissal");
        if (!fixture.empty())
        {
            ShowConversation();
            settings_.BaseUrl = fixture;
            settings_.ApiKey = L"fixture-key";
            settings_.CaptureDesktopAudio = false;
            settings_.DesktopSpeechProvider = L"Windows";
            screens_ = {{L"fixture-display", {1, 2, 3}, 100, 80}};
            SetQuestion(L"http-ok");
            OpenSettings();
            Start(false);
            auto deadline = Seconds() + 10;
            while (busy_ && Seconds() < deadline)
                co_await Pause(std::chrono::milliseconds(10));
            expect(!busy_, L"WinUI text question finishes through C++ backend");
            expect(!showingSettings_ && visibleWindows() == 1,
                   L"starting a question from settings switches to conversation without blocking");
            expect(answerText_ == L"fixture answer" && history_.size() == 1,
                   L"WinUI receives streamed answer and completed history");
            expect(submittedQuestion_.Text() == L"http-ok" && displayedAnswer_ == L"fixture answer",
                   L"model pipeline updates question card and rich answer");
            screens_ = {{L"fixture-display", {1, 2, 3}, 100, 80}};
            SetQuestion(L"http-blocked");
            Start(false);
            deadline = Seconds() + 10;
            while (answerText_.empty() && Seconds() < deadline)
                co_await Pause(std::chrono::milliseconds(10));
            expect(!answerText_.empty(), L"WinUI receives partial streamed answer");
            auto start = Seconds();
            OpenSettings();
            if (operation_.joinable())
                operation_.join();
            co_await Pause(std::chrono::milliseconds(10));
            expect(Seconds() - start < 2 && !busy_ && history_.size() == 1,
                   L"WinUI cancellation interrupts network and discards partial history");
            expect(showingSettings_ && visibleWindows() == 1,
                   L"opening settings during answer cancels request and keeps one window");
            ShowConversation();
            NewConversation();
            expect(history_.empty() && questionEdit_.Text().empty() &&
                       answerBody_.Blocks().Size() == 0,
                   L"new conversation clears WinUI panels");
        }
    }
    catch (...)
    {
        errors.Append(J(ErrorText()));
    }
    int result = errors.Size() == 0 ? 0 : 1;
    try
    {
        JsonObject report;
        report.SetNamedValue(L"Passed", J(result == 0));
        report.SetNamedValue(L"Checks", J(checks));
        report.SetNamedValue(L"Errors", errors);
        report.SetNamedValue(L"Framework", J(L"C++/WinRT / WinUI 3 / Windows App SDK"));
        AtomicWrite(output / L"ui-report.json", Utf8(report.Stringify()));
    }
    catch (...)
    {
        result = 1;
    }
    if (completed)
        completed(result);
    Shutdown();
}
} // namespace da
