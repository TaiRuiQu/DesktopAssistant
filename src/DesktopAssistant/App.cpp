#include "App.h"
#include <dwmapi.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>

namespace da
{
Application* Application::active_{};

namespace
{
constexpr UINT TrayMessage = WM_APP + 2, ActivateMessage = WM_APP + 3;

enum
{
    Send = 100,
    Finish,
    CancelButton,
    SettingsButton,
    Copy,
    HideAnswer,
    Save,
    CancelSettings,
    TraySettings = 200,
    TrayActivate,
    TrayShow,
    TrayNew,
    TrayPause,
    TrayExit
};

int Scale(HWND h, int v)
{
    return MulDiv(v, static_cast<int>(GetDpiForWindow(h)), 96);
}

void FitToMonitor(HWND h, RECT r)
{
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &info);
    const auto& area = info.rcWork;
    int w = std::min(r.right - r.left, area.right - area.left),
        height = std::min(r.bottom - r.top, area.bottom - area.top);
    int x = std::clamp(static_cast<int>(r.left),
                       static_cast<int>(area.left),
                       static_cast<int>(area.right) - w),
        y = std::clamp(static_cast<int>(r.top),
                       static_cast<int>(area.top),
                       static_cast<int>(area.bottom) - height);
    SetWindowPos(h, nullptr, x, y, w, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
} // namespace

Application::Application(HINSTANCE instance, bool diagnostics)
    : instance_(instance), diagnostics_(diagnostics)
{
    active_ = this;
    dispatcher_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    CreateMainWindow();
    CreateSettings();
    if (!diagnostics_)
    {
        try
        {
            settings_ = ReadSettings();
        }
        catch (...)
        {
            Notice(ErrorText());
        }
        if (settings_.SaveHistory)
            try
            {
                history_ = ReadHistory();
            }
            catch (...)
            {
                Notice(ErrorText());
            }
        UpdateFields();
        AddTray();
        ApplySettings();
        if (!settings_.StartMinimized)
            OpenSettings();
    }
}

Application::~Application()
{
    Shutdown();
    active_ = nullptr;
}

void Application::HideWindow()
{
    window_.AppWindow().Hide();
}

void Application::ShowView(bool settings, bool activate)
{
    if (exiting_)
        return;
    if (!positioned_)
    {
        RestoreLayout();
        positioned_ = true;
    }
    auto appWindow = window_.AppWindow();
    auto presenter = appWindow.Presenter().as<windowing::OverlappedPresenter>();
    if (settings != showingSettings_)
    {
        if (settings)
            GetWindowRect(panel_, &conversationBounds_);
        showingSettings_ = settings;
        conversationRoot_.Visibility(settings ? ux::Visibility::Collapsed
                                              : ux::Visibility::Visible);
        settingsRoot_.Visibility(settings ? ux::Visibility::Visible : ux::Visibility::Collapsed);
        presenter.IsAlwaysOnTop(!settings);
        presenter.IsMaximizable(settings);
        presenter.IsMinimizable(settings);
        presenter.SetBorderAndTitleBar(true, settings);
        window_.Title(settings ? L"设置 · desktop assistant" : L"问答 · desktop assistant");
        if (settings)
        {
            RECT bounds{};
            GetWindowRect(panel_, &bounds);
            FitToMonitor(panel_,
                         {bounds.left,
                          bounds.top,
                          bounds.left + Scale(panel_, 800),
                          bounds.top + Scale(panel_, 780)});
        }
        else
        {
            if (activate)
                presenter.Restore();
            if (conversationBounds_.right > conversationBounds_.left)
                FitToMonitor(panel_, conversationBounds_);
        }
    }
    if (activate)
    {
        if (!settings && presenter.State() != windowing::OverlappedPresenterState::Restored)
        {
            presenter.Restore();
            FitToMonitor(panel_, conversationBounds_);
        }
        appWindow.Show(true);
        window_.Activate();
        if (!settings && editingQuestion_)
            questionEdit_.Focus(ux::FocusState::Programmatic);
    }
}

void Application::ShowConversation()
{
    ShowView(false);
    StartWakeWord();
}

void Application::EditQuestion(bool editing)
{
    editingQuestion_ = editing;
    auto visible = ux::Visibility::Visible, hidden = ux::Visibility::Collapsed;
    questionEdit_.Visibility(editing ? visible : hidden);
    conversationRoot_.FindName(L"QuestionSnapshot")
        .as<ux::UIElement>()
        .Visibility(editing ? hidden : visible);
    conversationRoot_.FindName(L"QuestionToolbar")
        .as<ux::UIElement>()
        .Visibility(editing || busy_ ? visible : hidden);
    conversationRoot_.FindName(L"EditQuestionButton").as<controls::Button>().IsEnabled(!busy_);
    if (editing && !showingSettings_)
        questionEdit_.Focus(ux::FocusState::Programmatic);
}

void Application::OpenSettings()
{
    if (busy_)
        Cancel();
    StopSpeech();
    if (!showingSettings_)
    {
        UpdateFields();
        settingsError_.IsOpen(false);
    }
    ShowView(true);
    StopWakeWord();
}

void Application::CloseSettings()
{
    // Restore conversation geometry while hidden; closing never activates another page.
    HideWindow();
    ShowView(false, false);
    UpdateFields();
    settingsError_.IsOpen(false);
    StartWakeWord();
}

void Application::StopWakeWord()
{
    ++wakeGeneration_;
    wakeListening_ = false;
    wakeWord_.Stop();
}

void Application::StartWakeWord()
{
    if (wakeListening_ || !settings_.WakeWordEnabled || paused_ || busy_ || speaking_ ||
        showingSettings_ || diagnostics_ || exiting_)
        return;
    // Complete microphone/ducking and playback cleanup before returning to standby.
    if (operation_.joinable())
        operation_.join();
    if (speechOperation_.joinable())
        speechOperation_.join();
    auto generation = ++wakeGeneration_;
    wakeListening_ = true;
    wakeWord_.Start(
        settings_.WakePhrase,
        settings_.SpeechLanguage,
        [this, generation]
        {
            Post(
                [this, generation]
                {
                    if (generation != wakeGeneration_ || !wakeListening_ || busy_ || speaking_ ||
                        showingSettings_ || paused_)
                        return;
                    Start(true);
                });
        },
        [this, generation](auto message)
        {
            Post(
                [this, generation, message]
                {
                    if (generation == wakeGeneration_)
                    {
                        Status(message);
                        // Surface actual standby failures even while the window is in the tray.
                        auto notification = tray_;
                        notification.uFlags = NIF_INFO;
                        notification.dwInfoFlags = NIIF_WARNING;
                        wcscpy_s(notification.szInfoTitle, L"语音唤醒暂不可用");
                        wcsncpy_s(notification.szInfo, message.c_str(), _TRUNCATE);
                        Shell_NotifyIconW(NIM_MODIFY, &notification);
                    }
                });
        },
        [this, generation]
        {
            Post(
                [this, generation]
                {
                    if (generation == wakeGeneration_)
                        Status(L"语音唤醒已开启 · 说「" + settings_.WakePhrase + L"」开始提问");
                });
        });
}

void Application::Post(std::function<void()> action)
{
    dispatcher_.TryEnqueue(
        [this, action = std::move(action)]
        {
            if (!exiting_)
                try
                {
                    action();
                }
                catch (...)
                {
                    if (busy_)
                        Cancel();
                    Notice(ErrorText());
                }
        });
}

void Application::Status(std::wstring text)
{
    status_.Text(text);
}

void Application::Notice(std::wstring text)
{
    MessageBoxW(panel_, text.c_str(), L"desktop assistant", MB_OK | MB_ICONINFORMATION);
}

void Application::SetQuestion(std::wstring text)
{
    suppressEdit_ = true;
    questionEdit_.Text(text);
    suppressEdit_ = false;
}

void Application::ApplySettings()
{
    StopWakeWord();
    if (hook_)
    {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    desktop_.Stop();
    detector_ = DoubleTap{};
    detector_.interval = settings_.DoubleTapMilliseconds;
    keysDown_.fill(false);
    activationKey_ = settings_.ActivationKey == L"LeftShift"      ? VK_LSHIFT
                     : settings_.ActivationKey == L"RightControl" ? VK_RCONTROL
                     : settings_.ActivationKey == L"LeftControl"  ? VK_LCONTROL
                     : settings_.ActivationKey == L"RightAlt"     ? VK_RMENU
                     : settings_.ActivationKey == L"F8"           ? VK_F8
                     : settings_.ActivationKey == L"F9"           ? VK_F9
                                                                  : VK_RSHIFT;
    auto shortcut = settings_.ActivationKey == L"LeftShift"      ? L"左 Shift"
                    : settings_.ActivationKey == L"RightControl" ? L"右 Ctrl"
                    : settings_.ActivationKey == L"LeftControl"  ? L"左 Ctrl"
                    : settings_.ActivationKey == L"RightAlt"     ? L"右 Alt"
                    : settings_.ActivationKey == L"F8"           ? L"F8"
                    : settings_.ActivationKey == L"F9"           ? L"F9"
                                                                 : L"右 Shift";
    conversationRoot_.FindName(L"AnswerShortcut")
        .as<controls::TextBlock>()
        .Text(settings_.WakeWordEnabled ? L"说「" + settings_.WakePhrase + L"」唤醒"
                                        : std::wstring(L"双击 ") + shortcut);
    if (!paused_ && !diagnostics_)
    {
        hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, Keyboard, instance_, 0);
        if (!hook_)
            Notice(L"全局快捷键安装失败。");
        if (settings_.CaptureDesktopAudio && settings_.AudioLookbackSeconds)
            desktop_.Start(settings_.AudioLookbackSeconds,
                           [this](auto message)
                           {
                               Post(
                                   [this, message]
                                   {
                                       Status(message);
                                   });
                           });
    }
    Status(paused_ ? L"采集与快捷键已暂停"
                   : L"双击 " + settings_.ActivationKey + L" 开始 · " +
                         std::to_wstring(history_.size()) + L" 轮对话");
    StartWakeWord();
}

std::wstring Application::Transcribe(const Settings& s,
                                     const Audio& audio,
                                     bool desktop,
                                     std::stop_token token)
{
    return (desktop ? s.DesktopSpeechProvider : s.MicrophoneSpeechProvider) == L"Windows"
               ? TranscribeLocal(audio, s.SpeechLanguage, token)
               : TranscribeRemote(s,
                                  audio,
                                  desktop ? s.DesktopTranscriptionModel
                                          : s.MicrophoneTranscriptionModel,
                                  token);
}

void Application::Start(bool voice)
{
    if (busy_)
    {
        if (listening_)
            finish_ = true;
        return;
    }
    if (paused_)
    {
        ShowConversation();
        Status(L"采集已暂停，请从托盘恢复。");
        return;
    }
    if (!voice && Trim(std::wstring(questionEdit_.Text())).empty())
    {
        Status(L"请输入问题，或双击快捷键使用语音。");
        return;
    }
    StopSpeech();
    StopWakeWord();
    // Finish the previous scoped ducking restore before another recording can attenuate sessions.
    if (operation_.joinable())
    {
        operation_.request_stop();
        operation_.join();
    }
    busy_ = true;
    activity_.IsActive(true);
    listening_ = voice;
    finish_ = false;
    uint64_t generation = ++generation_;
    settingsButton_.IsEnabled(true);
    cancel_.Visibility(ux::Visibility::Visible);
    finishButton_.Visibility(voice ? ux::Visibility::Visible : ux::Visibility::Collapsed);
    send_.Visibility(voice ? ux::Visibility::Collapsed : ux::Visibility::Visible);
    send_.IsEnabled(false);
    conversationRoot_.FindName(L"VoiceButton").as<controls::Button>().IsEnabled(false);
    try
    {
        EditQuestion(true);
        if (voice)
            SetQuestion(L"");
        if (voice || screens_.empty())
        {
            desktopContext_ = settings_.CaptureDesktopAudio ? desktop_.Snapshot() : Audio{};
            transcript_.clear();
            HideWindow();
            DwmFlush();
            screens_ = settings_.SendScreenshots ? CaptureScreens(settings_.ScreenshotMaxDimension)
                                                 : std::vector<Screen>{};
        }
        ShowConversation();
        answerText_.clear();
        SetAnswer(L"");
        submittedQuestion_.Text(voice ? L"正在听你说话…" : questionEdit_.Text());
        answerStatus_.Text(voice ? L"等待你的问题" : L"正在整理上下文…");
        Status(voice ? L"正在听你说话… 说完后自动提交" : L"正在整理桌面声音…");
        auto s = settings_;
        auto desktop = desktopContext_;
        auto revision = revision_;
        operation_ = std::jthread(
            [this, s, desktop = std::move(desktop), voice, revision, generation](
                std::stop_token token)
            {
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    std::wstring recognized;
                    if (voice)
                    {
                        AudioDucking ducking(s.DuckOtherAudio, token);
                        auto audio = RecordMicrophone(s, token, finish_);
                        Post(
                            [this, generation]
                            {
                                if (generation == generation_)
                                {
                                    listening_ = false;
                                    finishButton_.Visibility(ux::Visibility::Collapsed);
                                    Status(L"正在识别问题与桌面声音…");
                                }
                            });
                        recognized = Transcribe(s, audio, false, token);
                    }
                    std::wstring transcript;
                    try
                    {
                        transcript = Transcribe(s, desktop, true, token);
                    }
                    catch (const Cancelled&)
                    {
                        throw;
                    }
                    catch (...)
                    {
                        auto message = ErrorText();
                        Post(
                            [this, message, generation]
                            {
                                if (generation == generation_)
                                    Status(L"桌面声音转写失败，将继续使用截图和问题：" + message);
                            });
                    }
                    CheckCancel(token);
                    Post(
                        [this,
                         recognized = std::move(recognized),
                         transcript = std::move(transcript),
                         voice,
                         revision,
                         generation]
                        {
                            if (generation != generation_)
                                return;
                            if (voice && revision_ == revision)
                                SetQuestion(recognized);
                            transcript_ = transcript;
                            auto question = Trim(std::wstring(questionEdit_.Text()));
                            if (question.empty())
                            {
                                FinishOperation(generation,
                                                L"没有识别到问题。可输入文字并发送本次截图。");
                                return;
                            }
                            StartAnswer(question, generation);
                        });
                }
                catch (const Cancelled&)
                {
                    Post(
                        [this, generation]
                        {
                            if (generation == generation_)
                            {
                                screens_.clear();
                                FinishOperation(generation, L"已取消");
                            }
                        });
                }
                catch (...)
                {
                    auto message = ErrorText();
                    Post(
                        [this, message, generation]
                        {
                            if (generation == generation_)
                                FinishOperation(generation, message);
                        });
                }
            });
    }
    catch (...)
    {
        ShowConversation();
        FinishOperation(generation, ErrorText());
    }
}

void Application::StartAnswer(std::wstring question, uint64_t generation)
{
    Status(L"正在回答…");
    answerStatus_.Text(L"正在回答…");
    submittedQuestion_.Text(question);
    EditQuestion(false);
    questionEdit_.IsReadOnly(true);
    send_.IsEnabled(false);
    auto s = settings_;
    auto screens = screens_;
    auto transcript = transcript_;
    auto history = history_;
    operation_ = std::jthread(
        [this,
         s,
         screens = std::move(screens),
         transcript = std::move(transcript),
         history = std::move(history),
         question,
         generation](std::stop_token token)
        {
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                auto answer = Answer(s,
                                     question,
                                     transcript,
                                     screens,
                                     history,
                                     token,
                                     [this, generation](auto delta)
                                     {
                                         Post(
                                             [this, generation, delta]
                                             {
                                                 if (generation != generation_)
                                                     return;
                                                 answerText_ += delta;
                                                 SetAnswer(answerText_, true);
                                             });
                                     });
                CheckCancel(token);
                Post(
                    [this, answer = std::move(answer), question, transcript, generation]
                    {
                        if (generation != generation_)
                            return;
                        answerText_ = answer;
                        SetAnswer(answer);
                        history_.push_back({question, answer, transcript, Timestamp()});
                        if (history_.size() > 100)
                            history_.erase(history_.begin());
                        Persist();
                        screens_.clear();
                        desktopContext_ = {};
                        transcript_.clear();
                        answerStatus_.Text(L"回答完成");
                        FinishOperation(generation, L"回答完成 · 双击快捷键继续提问");
                    });
            }
            catch (const Cancelled&)
            {
                Post(
                    [this, generation]
                    {
                        if (generation == generation_)
                            FinishOperation(generation, L"已取消，未加入后续上下文");
                    });
            }
            catch (...)
            {
                auto message = ErrorText();
                Post(
                    [this, message, generation]
                    {
                        if (generation == generation_)
                        {
                            answerStatus_.Text(L"回答未完成，未加入后续上下文");
                            FinishOperation(generation, message);
                        }
                    });
            }
        });
}

void Application::FinishOperation(uint64_t generation, std::wstring text)
{
    if (generation != generation_)
        return;
    busy_ = listening_ = false;
    activity_.IsActive(false);
    settingsButton_.IsEnabled(true);
    send_.IsEnabled(true);
    send_.Visibility(ux::Visibility::Visible);
    conversationRoot_.FindName(L"VoiceButton").as<controls::Button>().IsEnabled(true);
    cancel_.Visibility(ux::Visibility::Collapsed);
    finishButton_.Visibility(ux::Visibility::Collapsed);
    questionEdit_.IsReadOnly(false);
    EditQuestion(editingQuestion_);
    Status(std::move(text));
    StartWakeWord();
}

void Application::Cancel()
{
    StopSpeech();
    ++generation_;
    if (operation_.joinable())
        operation_.request_stop();
    screens_.clear();
    desktopContext_ = {};
    transcript_.clear();
    FinishOperation(generation_, L"已取消，未加入后续上下文");
    answerStatus_.Text(L"已取消");
}

void Application::Dismiss()
{
    Cancel();
    HideWindow();
}

void Application::Persist()
{
    if (!settings_.SaveHistory || diagnostics_)
        return;
    try
    {
        SaveHistory(history_);
    }
    catch (...)
    {
        Status(L"回答已完成，但本地历史保存失败。");
    }
}

void Application::NewConversation()
{
    Cancel();
    history_.clear();
    if (!diagnostics_)
        ClearHistory();
    SetQuestion(L"");
    answerText_.clear();
    SetAnswer(L"");
    submittedQuestion_.Text(L"你的问题会显示在这里");
    EditQuestion(true);
    answerStatus_.Text(L"等待你的问题");
    Status(L"已开始新对话");
}

void Application::CopyAnswer()
{
    CopyText(answerPlainText_);
}

void Application::CopyText(std::wstring_view text)
{
    if (text.empty() || !OpenClipboard(panel_))
        return;
    size_t size = (text.size() + 1) * sizeof(wchar_t);
    auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory)
    {
        auto* p = GlobalLock(memory);
        if (p)
        {
            memcpy(p, text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(p)[text.size()] = L'\0';
            GlobalUnlock(memory);
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        }
        else
            GlobalFree(memory);
    }
    CloseClipboard();
}

void Application::StopSpeech()
{
    ++speechGeneration_;
    if (speechOperation_.joinable())
        speechOperation_.request_stop();
    if (speaking_)
    {
        speaking_ = false;
        conversationRoot_.FindName(L"SpeakLabel").as<controls::TextBlock>().Text(L"朗读");
        answerStatus_.Text(L"朗读已停止");
        StartWakeWord();
    }
}

void Application::ToggleSpeech()
{
    try
    {
        if (speaking_)
        {
            StopSpeech();
            return;
        }
        if (busy_ || answerText_.empty())
        {
            answerStatus_.Text(L"有回答后即可朗读");
            return;
        }
        if (Trim(settings_.TtsApiKey).empty())
        {
            OpenSettings();
            navigation_.SelectedItem(navigation_.MenuItems().GetAt(4));
            settingsError_.Message(L"请先填写 DashScope 语音合成密钥。");
            settingsError_.IsOpen(true);
            return;
        }
        auto text = std::wstring(answerBody_.SelectedText());
        if (text.empty())
        {
            text = answerPlainText_;
        }
        auto chunks = SpeechChunks(text);
        if (chunks.empty())
            return;
        auto s = settings_;
        auto generation = ++speechGeneration_;
        StopWakeWord();
        speaking_ = true;
        conversationRoot_.FindName(L"SpeakLabel").as<controls::TextBlock>().Text(L"停止");
        answerStatus_.Text(L"正在合成语音…");
        speechOperation_ = std::jthread(
            [this, s, generation, chunks = std::move(chunks)](std::stop_token token)
            {
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    for (auto const& chunk : chunks)
                    {
                        CheckCancel(token);
                        auto wav = SynthesizeRemote(s, chunk, token);
                        Post(
                            [this, generation]
                            {
                                if (generation == speechGeneration_)
                                    answerStatus_.Text(L"正在朗读…");
                            });
                        PlayWav(wav, token);
                    }
                    Post(
                        [this, generation]
                        {
                            if (generation == speechGeneration_)
                            {
                                StopSpeech();
                                answerStatus_.Text(L"朗读完成");
                            }
                        });
                }
                catch (const Cancelled&)
                {
                }
                catch (...)
                {
                    auto message = ErrorText();
                    Post(
                        [this, generation, message]
                        {
                            if (generation == speechGeneration_)
                            {
                                StopSpeech();
                                answerStatus_.Text(message);
                            }
                        });
                }
            });
    }
    catch (...)
    {
        StopSpeech();
        answerStatus_.Text(ErrorText());
    }
}

void Application::AddTray()
{
    tray_ = {sizeof(tray_)};
    tray_.hWnd = host_;
    tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_.uCallbackMessage = TrayMessage;
    tray_.hIcon = icon_;
    wcscpy_s(tray_.szTip, L"desktop assistant · 双击打开设置");
    Shell_NotifyIconW(NIM_ADD, &tray_);
    tray_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray_);
}

void Application::TrayMenu()
{
    HMENU menu = CreatePopupMenu();
    for (auto item : std::vector<std::pair<UINT, const wchar_t*>>{
             {TraySettings, L"设置"},
             {TrayActivate, L"截图并提问"},
             {TrayShow, L"显示悬浮窗"},
             {TrayNew, L"新对话"},
             {TrayPause, paused_ ? L"恢复采集与快捷键" : L"暂停采集与快捷键"},
             {TrayExit, L"退出"}})
        AppendMenuW(menu, MF_STRING, item.first, item.second);
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(host_);
    auto action =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, host_, nullptr);
    DestroyMenu(menu);
    PostMessageW(host_, WM_NULL, 0, 0);
    if (action)
        SendMessageW(host_, WM_COMMAND, action, 0);
}

void Application::SaveLayout()
{
    if (!positioned_ || diagnostics_)
        return;
    RECT bounds = conversationBounds_;
    if (!showingSettings_)
        GetWindowRect(panel_, &bounds);
    JsonObject document;
    document.SetNamedValue(L"ConversationLeft", J(static_cast<int>(bounds.left)));
    document.SetNamedValue(L"ConversationTop", J(static_cast<int>(bounds.top)));
    document.SetNamedValue(L"ConversationWidth", J(static_cast<int>(bounds.right - bounds.left)));
    document.SetNamedValue(L"ConversationHeight", J(static_cast<int>(bounds.bottom - bounds.top)));
    AtomicWrite(DataDirectory() / L"windows.json", Utf8(document.Stringify()));
}

void Application::RestoreLayout()
{
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(panel_, MONITOR_DEFAULTTOPRIMARY), &monitor);
    auto width = Scale(panel_, 720), height = Scale(panel_, 740);
    auto left = monitor.rcWork.right - width - Scale(panel_, 24);
    auto top = monitor.rcWork.top + Scale(panel_, 60);
    FitToMonitor(panel_, {left, top, left + width, top + height});
    if (!diagnostics_)
        try
        {
            auto path = DataDirectory() / L"windows.json";
            if (std::filesystem::exists(path))
            {
                auto document = JsonObject::Parse(Wide(ReadFile(path, 65536)));
                // Prefer the unified window, then migrate the former answer window bounds.
                std::wstring key =
                    document.HasKey(L"ConversationLeft") ? L"Conversation" : L"Answer";
                double x = document.GetNamedNumber(key + L"Left"),
                       y = document.GetNamedNumber(key + L"Top"),
                       w = document.GetNamedNumber(key + L"Width"),
                       h = document.GetNamedNumber(key + L"Height");
                if (std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) &&
                    std::abs(x) <= 131072 && std::abs(y) <= 131072 && w >= 420 && h >= 210 &&
                    w <= 32768 && h <= 32768)
                    FitToMonitor(panel_,
                                 {static_cast<LONG>(x),
                                  static_cast<LONG>(y),
                                  static_cast<LONG>(
                                      x + std::max(w, static_cast<double>(Scale(panel_, 520)))),
                                  static_cast<LONG>(
                                      y + std::max(h, static_cast<double>(Scale(panel_, 420))))});
            }
        }
        catch (...)
        {
        }
    GetWindowRect(panel_, &conversationBounds_);
}

LRESULT CALLBACK Application::Keyboard(int code, WPARAM w, LPARAM l)
{
    if (code >= 0 && active_)
    {
        const auto* input = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
        if (!(input->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)))
        {
            bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN,
                 up = w == WM_KEYUP || w == WM_SYSKEYUP;
            auto key = input->vkCode;
            if (down || up)
            {
                if (key < active_->keysDown_.size())
                    active_->keysDown_[key] = down;
                bool activation = key == active_->activationKey_;
                auto now = GetTickCount64();
                bool triggered = active_->detector_.Process(activation, down, now);
                if (activation && down)
                {
                    bool other = false;
                    for (size_t k = 0; k < active_->keysDown_.size(); ++k)
                        if (k != key && active_->keysDown_[k])
                            other = true;
                    for (auto modifier : {VK_LSHIFT,
                                          VK_RSHIFT,
                                          VK_LCONTROL,
                                          VK_RCONTROL,
                                          VK_LMENU,
                                          VK_RMENU,
                                          VK_LWIN,
                                          VK_RWIN})
                        if (static_cast<UINT>(modifier) != key &&
                            (GetAsyncKeyState(modifier) & 0x8000))
                            other = true;
                    if (other)
                        active_->detector_.Process(false, true, now);
                }
                if (triggered)
                    PostMessageW(active_->host_, ActivateMessage, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

void Application::Shutdown()
{
    if (exiting_)
        return;
    exiting_ = true;
    StopWakeWord();
    if (hook_)
    {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (operation_.joinable())
    {
        operation_.request_stop();
        operation_.join();
    }
    StopSpeech();
    if (speechOperation_.joinable())
        speechOperation_.join();
    desktop_.Stop();
    if (!diagnostics_)
    {
        try
        {
            SaveLayout();
        }
        catch (...)
        {
        }
        Shell_NotifyIconW(NIM_DELETE, &tray_);
    }
    if (window_)
        window_.Close();
    if (host_)
        DestroyWindow(host_);
    ux::Application::Current().Exit();
}

void Application::Action(int action)
{
    try
    {
        switch (action)
        {
        case Send:
            Start(false);
            break;
        case Finish:
            finish_ = true;
            break;
        case CancelButton:
            Cancel();
            break;
        case SettingsButton:
        case TraySettings:
            OpenSettings();
            break;
        case Copy:
            CopyAnswer();
            break;
        case HideAnswer:
            Dismiss();
            break;
        case Save:
            SaveSettingsForm();
            break;
        case CancelSettings:
            CloseSettings();
            break;
        case TrayActivate:
            Start(true);
            break;
        case TrayShow:
            ShowConversation();
            break;
        case TrayNew:
            NewConversation();
            break;
        case TrayPause:
            Cancel();
            paused_ = !paused_;
            ApplySettings();
            break;
        case TrayExit:
            Shutdown();
            break;
        }
    }
    catch (...)
    {
        auto text = ErrorText();
        if (action == Save)
        {
            settingsError_.Message(text);
            settingsError_.IsOpen(true);
        }
        else
        {
            if (busy_)
                Cancel();
            Notice(text);
        }
    }
}

LRESULT CALLBACK Application::Procedure(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app)
        try
        {
            return app->Message(h, m, w, l);
        }
        catch (...)
        {
            app->Notice(ErrorText());
            return 0;
        }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK
Application::WindowProcedure(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
    auto* app = reinterpret_cast<Application*>(data);
    if (m == WM_GETMINMAXINFO)
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        info->ptMinTrackSize = {Scale(h, app->showingSettings_ ? 640 : 520),
                                Scale(h, app->showingSettings_ ? 560 : 420)};
        return 0;
    }
    if (m == WM_NCHITTEST && !app->showingSettings_)
    {
        auto result = DefSubclassProc(h, m, w, l);
        if (result != HTCLIENT)
            return result;
        POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        ScreenToClient(h, &point);
        RECT bounds{};
        GetClientRect(h, &bounds);
        if (point.y >= Scale(h, 8) && point.y < Scale(h, 50) && point.x >= Scale(h, 16) &&
            point.x < bounds.right - Scale(h, 112))
            return HTCAPTION;
        return result;
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, WindowProcedure, id);
    return DefSubclassProc(h, m, w, l);
}

LRESULT Application::Message(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (taskbarCreated_ && m == taskbarCreated_ && !diagnostics_)
    {
        AddTray();
        return 0;
    }
    switch (m)
    {
    case ActivateMessage:
        Start(true);
        return 0;
    case TrayMessage:
        if (LOWORD(l) == WM_LBUTTONDBLCLK || LOWORD(l) == NIN_KEYSELECT)
            OpenSettings();
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU)
            TrayMenu();
        return 0;
    case WM_COMMAND:
        Action(LOWORD(w));
        return 0;
    case WM_ENDSESSION:
        if (w)
            Shutdown();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Application::Run(const std::filesystem::path& output,
                      const std::wstring& fixture,
                      std::function<void(int)> completed)
{
    if (diagnostics_)
        Diagnostics(output, fixture, std::move(completed));
}
} // namespace da
