#include "Core.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <iomanip>
#include <sstream>

namespace da
{
std::wstring Wide(std::string_view s)
{
    return winrt::to_hstring(s).c_str();
}

std::string Utf8(std::wstring_view s)
{
    return winrt::to_string(s);
}

std::wstring Trim(std::wstring s)
{
    auto a = s.find_first_not_of(L" \r\n\t"), b = s.find_last_not_of(L" \r\n\t");
    return a == s.npos ? L"" : s.substr(a, b - a + 1);
}

std::wstring ErrorText()
{
    try
    {
        throw;
    }
    catch (const Error& e)
    {
        return e.text;
    }
    catch (const winrt::hresult_error& e)
    {
        return L"Windows 操作失败：" + std::wstring(e.message());
    }
    catch (const std::exception&)
    {
        return L"操作失败，请检查配置、文件权限及设备后重试。";
    }
    catch (...)
    {
        return L"操作未完成。";
    }
}

double Seconds()
{
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::string Base64(std::span<const uint8_t> b)
{
    if (b.empty())
        return {};
    DWORD n{};
    if (!CryptBinaryToStringA(b.data(),
                              static_cast<DWORD>(b.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr,
                              &n))
        throw Error(L"编码失败。");
    std::string s(n, '\0');
    if (!CryptBinaryToStringA(b.data(),
                              static_cast<DWORD>(b.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              s.data(),
                              &n))
        throw Error(L"编码失败。");
    s.resize(n);
    return s;
}

Bytes Unbase64(std::string_view s)
{
    if (s.empty())
        return {};
    DWORD n{};
    if (!CryptStringToBinaryA(s.data(),
                              static_cast<DWORD>(s.size()),
                              CRYPT_STRING_BASE64,
                              nullptr,
                              &n,
                              nullptr,
                              nullptr))
        throw Error(L"加密数据格式无效。");
    Bytes b(n);
    if (!CryptStringToBinaryA(s.data(),
                              static_cast<DWORD>(s.size()),
                              CRYPT_STRING_BASE64,
                              b.data(),
                              &n,
                              nullptr,
                              nullptr))
        throw Error(L"加密数据格式无效。");
    return b;
}

std::wstring Protect(std::wstring_view text)
{
    if (text.empty())
        return {};
    auto input = Utf8(text);
    DATA_BLOB in{static_cast<DWORD>(input.size()), reinterpret_cast<BYTE*>(input.data())}, out{};
    if (!CryptProtectData(
            &in, L"DesktopAssistant", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw Error(L"Windows 加密失败。");
    auto value = Base64({out.pbData, out.cbData});
    LocalFree(out.pbData);
    return Wide(value);
}

std::wstring Unprotect(std::wstring_view text)
{
    if (text.empty())
        return {};
    auto input = Unbase64(Utf8(text));
    DATA_BLOB in{static_cast<DWORD>(input.size()), input.data()}, out{};
    if (!CryptUnprotectData(
            &in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw Error(L"无法解密本机数据，请确认当前 Windows 账户。原文件已保留。");
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return Wide(plain);
}

std::filesystem::path DataDirectory()
{
    PWSTR path{};
    Check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path));
    std::filesystem::path p(path);
    CoTaskMemFree(path);
    return p / L"DesktopAssistant";
}

std::string ReadFile(const std::filesystem::path& p, size_t limit)
{
    std::ifstream file(p, std::ios::binary);
    if (!file)
        throw Error(L"无法读取本地文件，原文件已保留。");
    auto size = std::filesystem::file_size(p);
    if (size > limit)
        throw Error(L"本地文件过大，原文件已保留。");
    std::string s(static_cast<size_t>(size), '\0');
    if (!file.read(s.data(), static_cast<std::streamsize>(s.size())))
        throw Error(L"读取本地文件失败。");
    if (s.starts_with("\xEF\xBB\xBF"))
        s.erase(0, 3);
    return s;
}

void AtomicWrite(const std::filesystem::path& p, std::string_view data)
{
    std::filesystem::create_directories(p.parent_path());
    GUID id{};
    Check(CoCreateGuid(&id));
    wchar_t name[48]{};
    StringFromGUID2(id, name, 48);
    auto tmp = p;
    tmp += name;
    tmp += L".tmp";
    try
    {
        {
            std::ofstream file(tmp, std::ios::binary);
            if (!file || !file.write(data.data(), static_cast<std::streamsize>(data.size())) ||
                !file.flush())
                throw Error(L"无法保存本地文件。");
        }
        if (!MoveFileExW(
                tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw Error(L"无法替换本地文件。");
    }
    catch (...)
    {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        throw;
    }
}

Endpoint ParseEndpoint(std::wstring url)
{
    url = Trim(std::move(url));
    if (url.find_first_of(L"\r\n\t") != url.npos || url.find(L'?') != url.npos ||
        url.find(L'#') != url.npos)
        throw Error(L"Base URL 不能包含查询参数、片段或控制字符。");
    URL_COMPONENTS c{sizeof(c)};
    c.dwHostNameLength = c.dwUrlPathLength = c.dwUserNameLength = c.dwPasswordLength =
        static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c) || c.dwUserNameLength || c.dwPasswordLength)
        throw Error(L"Base URL 必须是有效的 API 根地址。");
    Endpoint e{std::wstring(c.lpszHostName, c.dwHostNameLength),
               std::wstring(c.lpszUrlPath, c.dwUrlPathLength),
               c.nPort,
               c.nScheme == INTERNET_SCHEME_HTTPS};
    auto host = e.host;
    std::transform(host.begin(), host.end(), host.begin(), towlower);
    bool local = host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
    if (!e.secure && !(c.nScheme == INTERNET_SCHEME_HTTP && local))
        throw Error(L"远程 Base URL 必须使用 HTTPS；仅本机服务允许 HTTP。");
    while (!e.path.empty() && e.path.back() == L'/')
        e.path.pop_back();
    return e;
}

VoiceActivity::VoiceActivity(const Settings& settings, double start)
    : start_(start), lastVoice_(start), threshold_(settings.VoiceThreshold),
      silence_(settings.SilenceSeconds), maximum_(settings.MaxRecordingSeconds)
{
}

void VoiceActivity::Append(std::span<const int16_t> pcm, double now)
{
    for (size_t i = 0; i < pcm.size(); ++i)
    {
        double value = pcm[i] / 32768.;
        energy_ += value * value;
        if (++samples_ != 320)
            continue;
        double rms = std::sqrt(energy_ / samples_);
        double frameEnd = now - (pcm.size() - i - 1) / 16000.;
        if (rms >= threshold_ * (spoke_ ? .55 : 1.))
        {
            if (spoke_ || ++onsetFrames_ >= 6)
            {
                spoke_ = true;
                lastVoice_ = frameEnd;
            }
        }
        else
            onsetFrames_ = 0;
        energy_ = 0;
        samples_ = 0;
    }
}

bool VoiceActivity::Finished(double now) const
{
    return now - start_ >= maximum_ ||
           (spoke_ ? now - lastVoice_ >= silence_ : now - start_ >= 20.);
}

bool MatchesWakePhrase(std::wstring_view text, std::wstring_view phrase)
{
    auto normalize = [](std::wstring_view value)
    {
        std::wstring result;
        for (wchar_t ch : value)
        {
            WORD kind{};
            GetStringTypeW(CT_CTYPE1, &ch, 1, &kind);
            if (!(kind & (C1_SPACE | C1_PUNCT)))
                result += static_cast<wchar_t>(towlower(ch));
        }
        return result;
    };
    auto expected = normalize(phrase);
    return !expected.empty() && normalize(text) == expected;
}

void Validate(const Settings& s)
{
    ParseEndpoint(s.BaseUrl);
    if (!Trim(s.SpeechBaseUrl).empty())
        ParseEndpoint(s.SpeechBaseUrl);
    ParseEndpoint(s.TtsBaseUrl);
    if (Trim(s.AnswerModel).empty())
        throw Error(L"请填写回答模型。");
    if (s.ApiKey.find_first_of(L"\r\n") != s.ApiKey.npos ||
        s.SpeechApiKey.find_first_of(L"\r\n") != s.SpeechApiKey.npos ||
        s.TtsApiKey.find_first_of(L"\r\n") != s.TtsApiKey.npos)
        throw Error(L"API Key 不能包含换行。");
    if (s.SpeechApiMode != L"Transcriptions" && s.SpeechApiMode != L"ChatCompletions" &&
        s.SpeechApiMode != L"DashScope")
        throw Error(L"语音接口格式无效。");
    if (Trim(s.TtsModel).empty() || Trim(s.TtsVoice).empty())
        throw Error(L"请填写朗读模型和音色。");
    for (auto provider : {s.DesktopSpeechProvider, s.MicrophoneSpeechProvider})
        if (provider != L"Windows" && provider != L"OpenAI")
            throw Error(L"请选择 Windows 或云端语音识别。");
    if ((s.DesktopSpeechProvider == L"OpenAI" && Trim(s.DesktopTranscriptionModel).empty()) ||
        (s.MicrophoneSpeechProvider == L"OpenAI" && Trim(s.MicrophoneTranscriptionModel).empty()))
        throw Error(L"请填写语音转写模型。");
    if (s.ApiMode != L"Responses" && s.ApiMode != L"ChatCompletions")
        throw Error(L"接口格式无效。");
    if (s.AudioLookbackSeconds < 0 || s.AudioLookbackSeconds > 60 || s.HistoryTurns < 0 ||
        s.HistoryTurns > 100 || s.DoubleTapMilliseconds < 150 || s.DoubleTapMilliseconds > 1000 ||
        s.MaxRecordingSeconds < 5 || s.MaxRecordingSeconds > 120 ||
        s.ScreenshotMaxDimension < 640 || s.ScreenshotMaxDimension > 7680)
        throw Error(L"数值超出范围：回看 0–60 秒、历史 0–100 轮、双击 150–1000 毫秒、录音 5–120 "
                    L"秒、截图 640–7680 像素。");
    if (!std::isfinite(s.SilenceSeconds) || s.SilenceSeconds < .5 || s.SilenceSeconds > 15 ||
        !std::isfinite(s.VoiceThreshold) || s.VoiceThreshold < .001 || s.VoiceThreshold > .2)
        throw Error(L"静音等待应为 0.5–15 秒，音量阈值应为 0.001–0.2。");
    if (s.WakeWordEnabled && (Trim(s.WakePhrase).empty() || s.WakePhrase.size() > 64 ||
                              s.WakePhrase.find_first_of(L"\r\n\t") != s.WakePhrase.npos ||
                              !MatchesWakePhrase(s.WakePhrase, s.WakePhrase)))
        throw Error(L"请填写 1–64 字符的唤醒词，不要包含换行或仅使用标点。");
    if (Trim(s.SpeechLanguage).empty() || s.SystemPrompt.size() > 16000)
        throw Error(L"请填写语音语言，系统提示词最多 16000 字符。");
    const std::vector<std::wstring> keys{
        L"RightShift", L"LeftShift", L"RightControl", L"LeftControl", L"RightAlt", L"F8", L"F9"};
    if (std::find(keys.begin(), keys.end(), s.ActivationKey) == keys.end())
        throw Error(L"唤起键无效。");
}

JsonObject SettingsJson(const Settings& s)
{
    JsonObject o;
    o.SetNamedValue(L"BaseUrl", J(s.BaseUrl));
    o.SetNamedValue(L"ApiKey", J(s.ApiKey));
    o.SetNamedValue(L"SpeechBaseUrl", J(s.SpeechBaseUrl));
    o.SetNamedValue(L"SpeechApiKey", J(s.SpeechApiKey));
    o.SetNamedValue(L"SpeechApiMode", J(s.SpeechApiMode));
    o.SetNamedValue(L"TtsBaseUrl", J(s.TtsBaseUrl));
    o.SetNamedValue(L"TtsApiKey", J(s.TtsApiKey));
    o.SetNamedValue(L"TtsModel", J(s.TtsModel));
    o.SetNamedValue(L"TtsVoice", J(s.TtsVoice));
    o.SetNamedValue(L"AnswerModel", J(s.AnswerModel));
    o.SetNamedValue(L"SpeechLanguage", J(s.SpeechLanguage));
    o.SetNamedValue(L"DesktopSpeechProvider", J(s.DesktopSpeechProvider));
    o.SetNamedValue(L"MicrophoneSpeechProvider", J(s.MicrophoneSpeechProvider));
    o.SetNamedValue(L"DesktopTranscriptionModel", J(s.DesktopTranscriptionModel));
    o.SetNamedValue(L"MicrophoneTranscriptionModel", J(s.MicrophoneTranscriptionModel));
    o.SetNamedValue(L"ApiMode", J(s.ApiMode));
    o.SetNamedValue(L"ActivationKey", J(s.ActivationKey));
    o.SetNamedValue(L"WakePhrase", J(s.WakePhrase));
    o.SetNamedValue(L"WakeWordEnabled", J(s.WakeWordEnabled));
    o.SetNamedValue(L"VoiceDetectionVersion", J(2));
    o.SetNamedValue(L"SystemPrompt", J(s.SystemPrompt));
    o.SetNamedValue(L"AudioLookbackSeconds", J(s.AudioLookbackSeconds));
    o.SetNamedValue(L"HistoryTurns", J(s.HistoryTurns));
    o.SetNamedValue(L"DoubleTapMilliseconds", J(s.DoubleTapMilliseconds));
    o.SetNamedValue(L"MaxRecordingSeconds", J(s.MaxRecordingSeconds));
    o.SetNamedValue(L"ScreenshotMaxDimension", J(s.ScreenshotMaxDimension));
    o.SetNamedValue(L"SilenceSeconds", J(s.SilenceSeconds));
    o.SetNamedValue(L"VoiceThreshold", J(s.VoiceThreshold));
    o.SetNamedValue(L"CaptureDesktopAudio", J(s.CaptureDesktopAudio));
    o.SetNamedValue(L"SendScreenshots", J(s.SendScreenshots));
    o.SetNamedValue(L"DuckOtherAudio", J(s.DuckOtherAudio));
    o.SetNamedValue(L"SaveHistory", J(s.SaveHistory));
    o.SetNamedValue(L"StartMinimized", J(s.StartMinimized));
    return o;
}

namespace
{
int ReadInteger(const JsonObject& document, std::wstring_view name, int fallback)
{
    double value = document.GetNamedNumber(name, fallback);
    if (!std::isfinite(value) || value != std::trunc(value) || value < INT_MIN || value > INT_MAX)
        throw Error(L"整数设置无效。");
    return static_cast<int>(value);
}
} // namespace

Settings ReadSettings(const std::filesystem::path& dir)
{
    auto p = dir / L"settings.json";
    if (!std::filesystem::exists(p))
        return {};
    try
    {
        auto doc = JsonObject::Parse(Wide(ReadFile(p)));
        if (doc.GetNamedNumber(L"Version") != 1)
            throw Error(L"不支持的设置版本。");
        auto o = doc.GetNamedObject(L"Settings");
        Settings s;
        s.BaseUrl = String(o, L"BaseUrl", s.BaseUrl);
        s.ApiKey = String(o, L"ApiKey", s.ApiKey);
        s.SpeechBaseUrl = String(o, L"SpeechBaseUrl", s.SpeechBaseUrl);
        s.SpeechApiMode = String(o, L"SpeechApiMode", s.SpeechApiMode);
        s.TtsBaseUrl = String(o, L"TtsBaseUrl", s.TtsBaseUrl);
        s.TtsModel = String(o, L"TtsModel", s.TtsModel);
        s.TtsVoice = String(o, L"TtsVoice", s.TtsVoice);
        s.AnswerModel = String(o, L"AnswerModel", s.AnswerModel);
        s.SpeechLanguage = String(o, L"SpeechLanguage", s.SpeechLanguage);
        s.DesktopSpeechProvider = String(o, L"DesktopSpeechProvider", s.DesktopSpeechProvider);
        s.MicrophoneSpeechProvider =
            String(o, L"MicrophoneSpeechProvider", s.MicrophoneSpeechProvider);
        s.DesktopTranscriptionModel =
            String(o, L"DesktopTranscriptionModel", s.DesktopTranscriptionModel);
        s.MicrophoneTranscriptionModel =
            String(o, L"MicrophoneTranscriptionModel", s.MicrophoneTranscriptionModel);
        s.ApiMode = String(o, L"ApiMode", s.ApiMode);
        s.ActivationKey = String(o, L"ActivationKey", s.ActivationKey);
        s.WakePhrase = String(o, L"WakePhrase", s.WakePhrase);
        s.WakeWordEnabled = o.GetNamedBoolean(L"WakeWordEnabled", s.WakeWordEnabled);
        s.SystemPrompt = String(o, L"SystemPrompt", s.SystemPrompt);
        s.AudioLookbackSeconds = ReadInteger(o, L"AudioLookbackSeconds", s.AudioLookbackSeconds);
        s.HistoryTurns = ReadInteger(o, L"HistoryTurns", s.HistoryTurns);
        s.DoubleTapMilliseconds = ReadInteger(o, L"DoubleTapMilliseconds", s.DoubleTapMilliseconds);
        s.MaxRecordingSeconds = ReadInteger(o, L"MaxRecordingSeconds", s.MaxRecordingSeconds);
        s.ScreenshotMaxDimension =
            ReadInteger(o, L"ScreenshotMaxDimension", s.ScreenshotMaxDimension);
        s.SilenceSeconds = o.GetNamedNumber(L"SilenceSeconds", s.SilenceSeconds);
        // Upgrade the former default once; preserve custom values and later explicit choices.
        if (o.GetNamedNumber(L"VoiceDetectionVersion", 1) < 2 && s.SilenceSeconds == 1.5)
            s.SilenceSeconds = 4.0;
        s.VoiceThreshold = o.GetNamedNumber(L"VoiceThreshold", s.VoiceThreshold);
        s.CaptureDesktopAudio = o.GetNamedBoolean(L"CaptureDesktopAudio", s.CaptureDesktopAudio);
        s.SendScreenshots = o.GetNamedBoolean(L"SendScreenshots", s.SendScreenshots);
        s.DuckOtherAudio = o.GetNamedBoolean(L"DuckOtherAudio", s.DuckOtherAudio);
        s.SaveHistory = o.GetNamedBoolean(L"SaveHistory", s.SaveHistory);
        s.StartMinimized = o.GetNamedBoolean(L"StartMinimized", s.StartMinimized);
        s.ApiKey = Unprotect(String(doc, L"ProtectedApiKey"));
        s.SpeechApiKey = Unprotect(String(doc, L"ProtectedSpeechApiKey"));
        s.TtsApiKey = Unprotect(String(doc, L"ProtectedTtsApiKey"));
        Validate(s);
        return s;
    }
    catch (...)
    {
        throw Error(
            L"无法读取设置或解密 API Key。原设置文件已保留，请检查当前 Windows 账户及文件。");
    }
}

void SaveSettings(const Settings& s, const std::filesystem::path& dir)
{
    Validate(s);
    auto o = SettingsJson(s);
    o.SetNamedValue(L"ApiKey", J(L""));
    o.SetNamedValue(L"SpeechApiKey", J(L""));
    o.SetNamedValue(L"TtsApiKey", J(L""));
    JsonObject doc;
    doc.SetNamedValue(L"Version", J(1));
    doc.SetNamedValue(L"Settings", o);
    doc.SetNamedValue(L"ProtectedApiKey", J(Protect(s.ApiKey)));
    doc.SetNamedValue(L"ProtectedSpeechApiKey", J(Protect(s.SpeechApiKey)));
    doc.SetNamedValue(L"ProtectedTtsApiKey", J(Protect(s.TtsApiKey)));
    AtomicWrite(dir / L"settings.json", Utf8(doc.Stringify()));
}

std::vector<Turn> ReadHistory(const std::filesystem::path& dir)
{
    auto p = dir / L"history.dat";
    if (!std::filesystem::exists(p))
        return {};
    try
    {
        auto a = JsonArray::Parse(Unprotect(Wide(ReadFile(p))));
        std::vector<Turn> turns;
        for (uint32_t i = a.Size() > 100 ? a.Size() - 100 : 0; i < a.Size(); ++i)
        {
            auto o = a.GetObjectAt(i);
            turns.push_back({String(o, L"Question"),
                             String(o, L"Answer"),
                             String(o, L"DesktopTranscript"),
                             String(o, L"CreatedAt")});
        }
        return turns;
    }
    catch (...)
    {
        throw Error(L"无法读取本机对话记录，原文件已保留。你仍可开始新对话。");
    }
}

void SaveHistory(const std::vector<Turn>& turns, const std::filesystem::path& dir)
{
    JsonArray a;
    for (size_t i = turns.size() > 100 ? turns.size() - 100 : 0; i < turns.size(); ++i)
    {
        const auto& t = turns[i];
        JsonObject o;
        o.SetNamedValue(L"Question", J(t.question));
        o.SetNamedValue(L"Answer", J(t.answer));
        o.SetNamedValue(L"DesktopTranscript", J(t.transcript));
        o.SetNamedValue(L"CreatedAt", J(t.createdAt));
        a.Append(o);
    }
    AtomicWrite(dir / L"history.dat", Utf8(Protect(a.Stringify())));
}

void ClearHistory(const std::filesystem::path& dir)
{
    std::filesystem::remove(dir / L"history.dat");
}

std::wstring Timestamp()
{
    SYSTEMTIME s{};
    GetSystemTime(&s);
    wchar_t b[40]{};
    swprintf_s(b,
               L"%04u-%02u-%02uT%02u:%02u:%02uZ",
               s.wYear,
               s.wMonth,
               s.wDay,
               s.wHour,
               s.wMinute,
               s.wSecond);
    return b;
}

bool DoubleTap::Process(bool activation, bool down, uint64_t now)
{
    if (!activation)
    {
        if (down)
        {
            last_ = 0;
            interrupted_ = true;
        }
        return false;
    }
    if (down)
    {
        if (!pressed_)
        {
            pressed_ = true;
            pressedAt_ = now;
            interrupted_ = false;
        }
        return false;
    }
    if (!pressed_)
        return false;
    pressed_ = false;
    if (interrupted_ || now - pressedAt_ > static_cast<uint64_t>(interval))
    {
        last_ = 0;
        return false;
    }
    if (last_ && now - last_ <= static_cast<uint64_t>(interval))
    {
        last_ = 0;
        return true;
    }
    last_ = now;
    return false;
}

void Timeline::Reset(int seconds)
{
    std::lock_guard lock(mutex_);
    seconds_ = std::clamp(seconds, 0, 60);
    chunks_.clear();
}

void Timeline::Append(std::span<const int16_t> samples, double end)
{
    std::lock_guard lock(mutex_);
    if (!seconds_)
        return;
    if (samples.size() > static_cast<size_t>(seconds_) * 16000)
        samples = samples.last(static_cast<size_t>(seconds_) * 16000);
    chunks_.push_back({end, {samples.begin(), samples.end()}});
    while (!chunks_.empty() && chunks_.front().end < end - seconds_)
        chunks_.pop_front();
}

Audio Timeline::Snapshot(double now) const
{
    std::lock_guard lock(mutex_);
    Audio a;
    if (!seconds_ || chunks_.empty() || chunks_.back().end <= now - seconds_)
        return a;
    double start =
        std::max(now - seconds_, chunks_.front().end - chunks_.front().samples.size() / 16000.0);
    if (start >= now)
        return a;
    a.pcm.resize(static_cast<size_t>((now - start) * 16000), 0);
    for (const auto& c : chunks_)
    {
        auto offset = static_cast<int64_t>(
            std::llround((c.end - c.samples.size() / 16000.0 - start) * 16000));
        for (size_t i = 0; i < c.samples.size(); ++i)
        {
            auto n = offset + static_cast<int64_t>(i);
            if (n >= 0 && n < static_cast<int64_t>(a.pcm.size()))
                a.pcm[static_cast<size_t>(n)] = c.samples[i];
        }
    }
    return a;
}

Bytes Audio::Wav() const
{
    Bytes b(44 + pcm.size() * 2);
    auto put = [&](size_t pos, uint32_t n, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            b[pos + i] = static_cast<uint8_t>(n >> (i * 8));
    };
    memcpy(b.data(), "RIFF", 4);
    put(4, static_cast<uint32_t>(b.size() - 8), 4);
    memcpy(b.data() + 8, "WAVEfmt ", 8);
    put(16, 16, 4);
    put(20, 1, 2);
    put(22, 1, 2);
    put(24, 16000, 4);
    put(28, 32000, 4);
    put(32, 2, 2);
    put(34, 16, 2);
    memcpy(b.data() + 36, "data", 4);
    put(40, static_cast<uint32_t>(pcm.size() * 2), 4);
    memcpy(b.data() + 44, pcm.data(), pcm.size() * 2);
    return b;
}

bool Audio::Silent() const
{
    return std::none_of(pcm.begin(),
                        pcm.end(),
                        [](int16_t v)
                        {
                            return std::abs(static_cast<int>(v)) > 32;
                        });
}
} // namespace da
