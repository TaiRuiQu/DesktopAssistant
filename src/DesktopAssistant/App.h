#pragma once
#include "Api.h"
#include "Capture.h"
#include "WinUi.h"
#include <commctrl.h>
#include <shellapi.h>
#include <array>

namespace da
{
class Application
{
    struct Field
    {
        controls::Control control{nullptr};
        int page{};
        std::wstring key;
        std::vector<std::pair<std::wstring, std::wstring>> choices;
        std::function<std::wstring(const Settings&)> read;
        std::function<void(Settings&, std::wstring)> write;
        bool check{}, password{}, number{};
    };

    HINSTANCE instance_{};
    HWND host_{}, panel_{};
    HICON icon_{};
    HHOOK hook_{};
    UINT taskbarCreated_{};
    ux::Window window_{nullptr};
    ux::FrameworkElement conversationRoot_{nullptr}, settingsRoot_{nullptr};
    bool showingSettings_{}, editingQuestion_ = true;
    RECT conversationBounds_{};
    controls::TextBox questionEdit_{nullptr};
    controls::RichTextBlock answerBody_{nullptr};
    controls::TextBlock submittedQuestion_{nullptr};
    controls::TextBlock status_{nullptr}, answerStatus_{nullptr};
    controls::Button send_{nullptr}, finishButton_{nullptr}, cancel_{nullptr},
        settingsButton_{nullptr};
    controls::ProgressRing activity_{nullptr};
    controls::NavigationView navigation_{nullptr};
    controls::InfoBar settingsError_{nullptr};
    std::array<ux::FrameworkElement, 5> pages_{{nullptr, nullptr, nullptr, nullptr, nullptr}};
    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
    NOTIFYICONDATAW tray_{};
    Settings settings_;
    std::vector<Turn> history_;
    std::vector<Field> fields_;
    DesktopAudio desktop_;
    WakeWordListener wakeWord_;
    bool wakeListening_{};
    uint64_t wakeGeneration_{};
    std::jthread operation_;
    std::jthread speechOperation_;
    bool speaking_{};
    uint64_t speechGeneration_{};
    std::atomic<bool> finish_{false};
    DoubleTap detector_;
    UINT activationKey_ = VK_RSHIFT;
    bool paused_{}, busy_{}, listening_{}, exiting_{}, diagnostics_{}, positioned_{},
        suppressEdit_{};
    std::array<bool, 256> keysDown_{};
    uint64_t revision_{}, generation_{};
    std::vector<Screen> screens_;
    Audio desktopContext_;
    std::wstring transcript_, answerText_, displayedAnswer_, answerPlainText_;
    std::vector<std::wstring> answerBlocks_;
    static Application* active_;
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK Keyboard(int, WPARAM, LPARAM);
    LRESULT Message(HWND, UINT, WPARAM, LPARAM);
    void CreateMainWindow();
    void CreateSettings();
    void UpdateFields();
    void SelectPage(int);
    void Action(int);
    void AddText(int,
                 const wchar_t*,
                 const wchar_t*,
                 std::wstring Settings::*,
                 bool password = false,
                 bool multiline = false);
    void AddNumber(int,
                   const wchar_t*,
                   const wchar_t*,
                   std::function<double(const Settings&)>,
                   std::function<void(Settings&, double)>);
    void AddInteger(int page, const wchar_t* key, int Settings::*member);
    void AddChoice(int,
                   const wchar_t*,
                   const wchar_t*,
                   std::wstring Settings::*,
                   std::vector<std::pair<std::wstring, std::wstring>>);
    void AddCheck(int, const wchar_t*, const wchar_t*, bool Settings::*);
    void ApplySettings();
    void SaveSettingsForm();
    void OpenSettings();
    void CloseSettings();
    void StartWakeWord();
    void StopWakeWord();
    void ShowConversation();
    void ShowView(bool settings, bool activate = true);
    void EditQuestion(bool editing);
    void HideWindow();
    void Start(bool voice);
    void StartAnswer(std::wstring question, uint64_t generation);
    void FinishOperation(uint64_t generation, std::wstring status);
    void Cancel();
    void Dismiss();
    void TrayMenu();
    void AddTray();
    void Shutdown();
    void NewConversation();
    void CopyAnswer();
    void CopyText(std::wstring_view text);
    void ToggleSpeech();
    void StopSpeech();
    void Persist();
    void SaveLayout();
    void RestoreLayout();
    void Post(std::function<void()> action);
    void Status(std::wstring text);
    void Notice(std::wstring text);
    void SetQuestion(std::wstring text);
    void SetAnswer(std::wstring text, bool preserveSelection = false);
    std::wstring Transcribe(const Settings&, const Audio&, bool, std::stop_token);
    winrt::fire_and_forget Diagnostics(std::filesystem::path output,
                                       std::wstring fixture,
                                       std::function<void(int)> completed);

public:
    Application(HINSTANCE instance, bool diagnostics);
    ~Application();
    void Run(const std::filesystem::path& output = {},
             const std::wstring& fixture = {},
             std::function<void(int)> completed = {});
};
} // namespace da
