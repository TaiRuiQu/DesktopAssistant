#include "Capture.h"
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wincodec.h>
#include <sapi.h>
#include <objidl.h>
#include <condition_variable>
#include <future>

namespace da
{
namespace
{
HRESULT EnumRecognizers(IEnumSpObjectTokens** result)
{
    ComPtr<ISpObjectTokenCategory> category;
    HRESULT hr =
        CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category));
    if (SUCCEEDED(hr))
        hr = category->SetId(SPCAT_RECOGNIZERS, FALSE);
    return SUCCEEDED(hr) ? category->EnumTokens(nullptr, nullptr, result) : hr;
}

ComPtr<ISpRecognizer> CreateRecognizer(std::wstring_view language)
{
    LANGID wanted = LANGIDFROMLCID(LocaleNameToLCID(std::wstring(language).c_str(), 0));
    if (!wanted)
        throw Error(L"本机语音识别语言无效。");
    ComPtr<IEnumSpObjectTokens> tokens;
    Check(EnumRecognizers(&tokens));
    ComPtr<ISpObjectToken> recognizerToken, current;
    while (tokens->Next(1, &current, nullptr) == S_OK)
    {
        ComPtr<ISpDataKey> attr;
        if (SUCCEEDED(current->OpenKey(L"Attributes", &attr)))
        {
            LPWSTR code{};
            if (SUCCEEDED(attr->GetStringValue(L"Language", &code)))
            {
                if (static_cast<LANGID>(wcstoul(code, nullptr, 16)) == wanted)
                    recognizerToken = current;
                CoTaskMemFree(code);
            }
        }
        current.Reset();
        if (recognizerToken)
            break;
    }
    if (!recognizerToken)
        throw Error(L"未安装 Windows 语音识别语言 " + std::wstring(language) +
                    L"。请安装对应语音组件或改用 GPT 识别。");
    ComPtr<ISpRecognizer> recognizer;
    Check(
        CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&recognizer)));
    Check(recognizer->SetRecognizer(recognizerToken.Get()));
    return recognizer;
}

struct Apartment
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    Apartment()
    {
        Check(hr);
    }

    ~Apartment()
    {
        CoUninitialize();
    }
};

struct Bitmap
{
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap{};
    HGDIOBJ old{};

    Bitmap(int w, int h)
    {
        bitmap = CreateCompatibleBitmap(screen, w, h);
        if (!bitmap || !dc)
            throw Error(L"无法分配截图缓冲。");
        old = SelectObject(dc, bitmap);
    }

    ~Bitmap()
    {
        if (old)
            SelectObject(dc, old);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        if (screen)
            ReleaseDC(nullptr, screen);
    }
};

Bytes Encode(HBITMAP bitmap, REFGUID format)
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmap> source;
    Check(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source));
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(format, nullptr, &encoder));
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr));
    Check(frame->Initialize(nullptr));
    Check(frame->WriteSource(source.Get(), nullptr));
    Check(frame->Commit());
    Check(encoder->Commit());
    STATSTG stat{};
    Check(stream->Stat(&stat, STATFLAG_NONAME));
    LARGE_INTEGER start{};
    Check(stream->Seek(start, STREAM_SEEK_SET, nullptr));
    Bytes bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG count{};
    Check(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &count));
    return bytes;
}

bool WaitStop(std::stop_token token, int ms)
{
    std::mutex m;
    std::condition_variable_any cv;
    std::unique_lock lock(m);
    cv.wait_for(lock,
                token,
                std::chrono::milliseconds(ms),
                []
                {
                    return false;
                });
    return token.stop_requested();
}

class Engine
{
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX* format_{};
    std::wstring id_;
    bool loopback_, started_{};
    double phase_{};
    float previous_{};
    bool hasPrevious_{};

public:
    explicit Engine(bool loopback) : loopback_(loopback)
    {
        Check(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)));
        Check(enumerator_->GetDefaultAudioEndpoint(
            loopback ? eRender : eCapture, eMultimedia, &device_));
        LPWSTR id{};
        Check(device_->GetId(&id));
        id_ = id;
        CoTaskMemFree(id);
        Check(device_->Activate(__uuidof(IAudioClient),
                                CLSCTX_ALL,
                                nullptr,
                                reinterpret_cast<void**>(client_.GetAddressOf())));
        Check(client_->GetMixFormat(&format_));
        try
        {
            Check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                      1000000,
                                      0,
                                      format_,
                                      nullptr));
            Check(client_->GetService(IID_PPV_ARGS(&capture_)));
            Check(client_->Start());
            started_ = true;
        }
        catch (...)
        {
            CoTaskMemFree(format_);
            format_ = nullptr;
            throw;
        }
    }

    ~Engine()
    {
        if (started_)
            client_->Stop();
        CoTaskMemFree(format_);
    }

    bool DefaultChanged()
    {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(
                loopback_ ? eRender : eCapture, eMultimedia, &device)))
            return true;
        LPWSTR id{};
        if (FAILED(device->GetId(&id)))
            return true;
        bool changed = id_ != id;
        CoTaskMemFree(id);
        return changed;
    }

    std::vector<int16_t> Read()
    {
        std::vector<int16_t> output;
        UINT32 count{};
        Check(capture_->GetNextPacketSize(&count));
        while (count)
        {
            BYTE* data{};
            UINT32 frames{};
            DWORD flags{};
            Check(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr));
            try
            {
                bool floating = format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                                (format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format_)->SubFormat ==
                                     KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                int bytes = format_->wBitsPerSample / 8;
                int channels = format_->nChannels;
                double step = format_->nSamplesPerSec / 16000.0;
                if ((floating && bytes != 4) ||
                    (!floating && bytes != 2 && bytes != 3 && bytes != 4) || !channels)
                    throw Error(L"默认音频设备格式不受支持。");
                for (UINT32 f = 0; f < frames; ++f)
                {
                    float sample = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        for (int ch = 0; ch < channels; ++ch)
                        {
                            const auto* p = data + f * format_->nBlockAlign + ch * bytes;
                            float v{};
                            if (floating)
                                memcpy(&v, p, 4);
                            else if (bytes == 2)
                            {
                                int16_t n{};
                                memcpy(&n, p, 2);
                                v = n / 32768.f;
                            }
                            else if (bytes == 3)
                            {
                                int32_t n = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
                                v = n / 2147483648.f;
                            }
                            else
                            {
                                int32_t n{};
                                memcpy(&n, p, 4);
                                v = n / 2147483648.f;
                            }
                            if (std::isfinite(v))
                                sample += v / channels;
                        }
                    if (!hasPrevious_)
                    {
                        previous_ = sample;
                        hasPrevious_ = true;
                    }
                    while (phase_ <= 1.0)
                    {
                        float v = previous_ + (sample - previous_) * static_cast<float>(phase_);
                        output.push_back(
                            static_cast<int16_t>(std::clamp(v, -1.f, .999969f) * 32768));
                        phase_ += step;
                    }
                    phase_ -= 1;
                    previous_ = sample;
                }
            }
            catch (...)
            {
                capture_->ReleaseBuffer(frames);
                throw;
            }
            Check(capture_->ReleaseBuffer(frames));
            Check(capture_->GetNextPacketSize(&count));
        }
        return output;
    }
};
} // namespace

AudioDucking::AudioDucking(bool enabled, std::stop_token operationToken)
{
    if (!enabled || operationToken.stop_requested())
        return;
    std::promise<void> ready;
    auto initialized = ready.get_future();
    worker_ = std::jthread(
        [operationToken, ready = std::move(ready)](std::stop_token token) mutable
        {
            bool signaled = false;
            try
            {
                Apartment apartment;
                struct Session
                {
                    std::wstring id;
                    ComPtr<ISimpleAudioVolume> volume;
                    float original{}, reduced{};
                };
                struct Restore
                {
                    std::vector<Session> sessions;
                    ~Restore()
                    {
                        for (auto const& session : sessions)
                        {
                            float current{};
                            // Preserve deliberate volume changes made while recognition was active.
                            if (SUCCEEDED(session.volume->GetMasterVolume(&current)) &&
                                std::abs(current - session.reduced) < .0001f)
                                session.volume->SetMasterVolume(session.original, nullptr);
                        }
                    }
                } restore;
                ComPtr<IMMDeviceEnumerator> enumerator;
                Check(CoCreateInstance(
                    __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
                auto scan = [&]
                {
                    ComPtr<IMMDeviceCollection> devices;
                    if (FAILED(
                            enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
                        return;
                    UINT count{};
                    devices->GetCount(&count);
                    for (UINT d = 0;
                         d < count && !token.stop_requested() && !operationToken.stop_requested();
                         ++d)
                    {
                        ComPtr<IMMDevice> device;
                        ComPtr<IAudioSessionManager2> manager;
                        ComPtr<IAudioSessionEnumerator> sessions;
                        if (FAILED(devices->Item(d, &device)) ||
                            FAILED(device->Activate(
                                __uuidof(IAudioSessionManager2),
                                CLSCTX_ALL,
                                nullptr,
                                reinterpret_cast<void**>(manager.GetAddressOf()))) ||
                            FAILED(manager->GetSessionEnumerator(&sessions)))
                            continue;
                        int sessionCount{};
                        sessions->GetCount(&sessionCount);
                        for (int i = 0; i < sessionCount; ++i)
                        {
                            ComPtr<IAudioSessionControl> control;
                            ComPtr<IAudioSessionControl2> details;
                            ComPtr<ISimpleAudioVolume> volume;
                            DWORD process{};
                            if (FAILED(sessions->GetSession(i, &control)) ||
                                FAILED(control.As(&details)) ||
                                details->GetProcessId(&process) != S_OK || !process ||
                                process == GetCurrentProcessId() ||
                                details->IsSystemSoundsSession() == S_OK ||
                                FAILED(control.As(&volume)))
                                continue;
                            LPWSTR name{};
                            if (FAILED(details->GetSessionInstanceIdentifier(&name)))
                                continue;
                            std::wstring id = name;
                            CoTaskMemFree(name);
                            if (std::any_of(restore.sessions.begin(),
                                            restore.sessions.end(),
                                            [&](auto const& item)
                                            {
                                                return item.id == id;
                                            }))
                                continue;
                            float original{};
                            BOOL muted{};
                            if (FAILED(volume->GetMasterVolume(&original)) ||
                                FAILED(volume->GetMute(&muted)) || muted || original <= 0)
                                continue;
                            // Record before changing volume so every successful change can be
                            // restored.
                            restore.sessions.push_back(
                                {std::move(id), volume, original, original * .2f});
                            if (FAILED(volume->SetMasterVolume(original * .2f, nullptr)))
                                restore.sessions.pop_back();
                        }
                    }
                };
                scan();
                ready.set_value();
                signaled = true;
                while (!token.stop_requested() && !operationToken.stop_requested())
                {
                    for (int i = 0; i < 5 && !operationToken.stop_requested(); ++i)
                        if (WaitStop(token, 50))
                            break;
                    if (!token.stop_requested() && !operationToken.stop_requested())
                        scan();
                }
            }
            catch (...)
            {
                // An unavailable audio session must not prevent microphone recording.
            }
            if (!signaled)
                ready.set_value();
        });
    initialized.wait();
}

AudioDucking::~AudioDucking()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
}

std::vector<Screen> CaptureScreens(int maximum)
{
    std::vector<MONITORINFOEXW> monitors;
    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL
        {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info))
                reinterpret_cast<std::vector<MONITORINFOEXW>*>(data)->push_back(info);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    std::vector<Screen> result;
    for (const auto& m : monitors)
    {
        int w = m.rcMonitor.right - m.rcMonitor.left, h = m.rcMonitor.bottom - m.rcMonitor.top;
        double scale = std::min(1., maximum / static_cast<double>(std::max(w, h)));
        int ow = std::max(1, static_cast<int>(w * scale)),
            oh = std::max(1, static_cast<int>(h * scale));
        Bitmap original(w, h);
        if (!BitBlt(original.dc,
                    0,
                    0,
                    w,
                    h,
                    original.screen,
                    m.rcMonitor.left,
                    m.rcMonitor.top,
                    SRCCOPY | CAPTUREBLT))
            throw Error(L"屏幕截图失败。");
        Bitmap resized(ow, oh);
        SetStretchBltMode(resized.dc, HALFTONE);
        SetBrushOrgEx(resized.dc, 0, 0, nullptr);
        if (!StretchBlt(resized.dc, 0, 0, ow, oh, original.dc, 0, 0, w, h, SRCCOPY))
            throw Error(L"截图缩放失败。");
        result.push_back({m.szDevice, Encode(resized.bitmap, GUID_ContainerFormatJpeg), ow, oh});
    }
    if (result.empty())
        throw Error(L"没有可截图的显示器。");
    return result;
}

void SaveWindowPreview(HWND window, const std::filesystem::path& path)
{
    RECT r{};
    GetWindowRect(window, &r);
    Bitmap bitmap(r.right - r.left, r.bottom - r.top);
    if (!PrintWindow(window, bitmap.dc, 2))
        throw Error(L"窗口预览失败。");
    auto b = Encode(bitmap.bitmap, GUID_ContainerFormatPng);
    AtomicWrite(path, {reinterpret_cast<const char*>(b.data()), b.size()});
}

void DesktopAudio::Start(int seconds, std::function<void(std::wstring)> warning)
{
    Stop();
    timeline_.Reset(seconds);
    if (!seconds)
        return;
    worker_ = std::jthread(
        [this, warning = std::move(warning), seconds](std::stop_token token)
        {
            try
            {
                Apartment apartment;
                bool warned = false;
                while (!token.stop_requested())
                {
                    try
                    {
                        Engine engine(true);
                        timeline_.Reset(seconds);
                        auto checked = Seconds();
                        while (!token.stop_requested())
                        {
                            auto pcm = engine.Read();
                            if (!pcm.empty())
                                timeline_.Append(pcm, Seconds());
                            if (Seconds() - checked >= 1)
                            {
                                checked = Seconds();
                                if (engine.DefaultChanged())
                                    break;
                            }
                            if (WaitStop(token, 10))
                                break;
                        }
                        warned = false;
                    }
                    catch (...)
                    {
                        timeline_.Reset(seconds);
                        if (!warned && !token.stop_requested())
                        {
                            warning(L"桌面声音设备暂不可用，正在重试。截图和文字提问仍可使用。");
                            warned = true;
                        }
                        if (WaitStop(token, 2000))
                            break;
                    }
                }
            }
            catch (...)
            {
                if (!token.stop_requested())
                    warning(L"无法初始化桌面声音采集。");
            }
        });
}

void DesktopAudio::Stop()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
    timeline_.Reset(0);
}

Audio RecordMicrophone(const Settings& s, std::stop_token token, const std::atomic<bool>& finish)
{
    CheckCancel(token);
    Engine engine(false);
    Audio audio;
    VoiceActivity activity(s, Seconds());
    while (!token.stop_requested() && !finish.load())
    {
        auto pcm = engine.Read();
        auto now = Seconds();
        activity.Append(pcm, now);
        audio.pcm.insert(audio.pcm.end(), pcm.begin(), pcm.end());
        if (activity.Finished(now))
            break;
        if (WaitStop(token, 10))
            break;
    }
    CheckCancel(token);
    return audio;
}

std::vector<std::pair<std::wstring, std::wstring>> SpeechLanguages()
{
    std::vector<std::pair<std::wstring, std::wstring>> result;
    ComPtr<IEnumSpObjectTokens> tokens;
    Check(EnumRecognizers(&tokens));
    ComPtr<ISpObjectToken> token;
    while (tokens->Next(1, &token, nullptr) == S_OK)
    {
        ComPtr<ISpDataKey> attributes;
        if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)))
        {
            LPWSTR codes{};
            if (SUCCEEDED(attributes->GetStringValue(L"Language", &codes)))
            {
                LANGID id = static_cast<LANGID>(wcstoul(codes, nullptr, 16));
                wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
                LCIDToLocaleName(MAKELCID(id, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0);
                CoTaskMemFree(codes);
                LPWSTR description{};
                token->GetStringValue(nullptr, &description);
                if (*locale)
                    result.emplace_back(locale, description ? description : locale);
                CoTaskMemFree(description);
            }
        }
        token.Reset();
    }
    return result;
}

void WakeWordListener::Stop()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
}

void WakeWordListener::Start(std::wstring phrase,
                             std::wstring language,
                             std::function<void()> recognized,
                             std::function<void(std::wstring)> warning,
                             std::function<void()> ready)
{
    Stop();
    worker_ = std::jthread(
        [phrase = std::move(phrase),
         language = std::move(language),
         recognized = std::move(recognized),
         warning = std::move(warning),
         ready = std::move(ready)](std::stop_token token)
        {
            try
            {
                Apartment apartment;
                bool warned = false;
                while (!token.stop_requested())
                {
                    try
                    {
                        auto recognizer = CreateRecognizer(language);
                        ComPtr<ISpObjectTokenCategory> category;
                        Check(CoCreateInstance(CLSID_SpObjectTokenCategory,
                                               nullptr,
                                               CLSCTX_ALL,
                                               IID_PPV_ARGS(&category)));
                        Check(category->SetId(SPCAT_AUDIOIN, FALSE));
                        LPWSTR id{};
                        Check(category->GetDefaultTokenId(&id));
                        std::wstring inputId(id);
                        CoTaskMemFree(id);
                        ComPtr<ISpObjectToken> input;
                        Check(CoCreateInstance(
                            CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&input)));
                        Check(input->SetId(nullptr, inputId.c_str(), FALSE));
                        Check(recognizer->SetInput(input.Get(), TRUE));
                        ComPtr<ISpRecoContext> context;
                        Check(recognizer->CreateRecoContext(&context));
                        Check(context->SetNotifyWin32Event());
                        auto interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM);
                        Check(context->SetInterest(interest, interest));
                        ComPtr<ISpRecoGrammar> grammar;
                        Check(context->CreateGrammar(2, &grammar));
                        auto locale = LocaleNameToLCID(language.c_str(), 0);
                        Check(grammar->ResetGrammar(LANGIDFROMLCID(locale)));
                        SPSTATEHANDLE rule{};
                        Check(grammar->GetRule(
                            L"Wake", 1, SPRAF_TopLevel | SPRAF_Dynamic, TRUE, &rule));
                        Check(grammar->AddWordTransition(
                            rule, nullptr, phrase.c_str(), L" ", SPWT_LEXICAL, 1.f, nullptr));
                        Check(grammar->Commit(0));
                        Check(grammar->SetRuleState(L"Wake", nullptr, SPRS_ACTIVE));
                        Check(recognizer->SetRecoState(SPRST_ACTIVE_ALWAYS));
                        if (ready && !token.stop_requested())
                            ready();
                        while (!token.stop_requested())
                        {
                            WaitForSingleObject(context->GetNotifyEventHandle(), 50);
                            struct Event
                            {
                                SPEVENT value{};
                                ~Event()
                                {
                                    if (value.elParamType == SPET_LPARAM_IS_OBJECT ||
                                        value.elParamType == SPET_LPARAM_IS_TOKEN)
                                        reinterpret_cast<IUnknown*>(value.lParam)->Release();
                                    else if (value.elParamType == SPET_LPARAM_IS_POINTER ||
                                             value.elParamType == SPET_LPARAM_IS_STRING)
                                        CoTaskMemFree(reinterpret_cast<void*>(value.lParam));
                                }
                            } event;
                            ULONG fetched{};
                            while (context->GetEvents(1, &event.value, &fetched) == S_OK && fetched)
                            {
                                if (event.value.eEventId == SPEI_END_SR_STREAM)
                                    throw Error(L"麦克风连接已中断，正在重试。");
                                if (event.value.eEventId == SPEI_RECOGNITION)
                                {
                                    auto result =
                                        reinterpret_cast<ISpRecoResult*>(event.value.lParam);
                                    SPPHRASE* details{};
                                    LPWSTR text{};
                                    bool confident = false;
                                    if (SUCCEEDED(result->GetPhrase(&details)))
                                    {
                                        confident =
                                            details->Rule.Confidence >= SP_NORMAL_CONFIDENCE;
                                        CoTaskMemFree(details);
                                    }
                                    bool matched = false;
                                    if (SUCCEEDED(result->GetText(static_cast<ULONG>(-1),
                                                                  static_cast<ULONG>(-1),
                                                                  TRUE,
                                                                  &text,
                                                                  nullptr)))
                                    {
                                        matched = MatchesWakePhrase(text, phrase);
                                        CoTaskMemFree(text);
                                    }
                                    if (confident && matched && !token.stop_requested())
                                    {
                                        // Release the microphone before handing control to
                                        // recording.
                                        Check(recognizer->SetRecoState(SPRST_INACTIVE_WITH_PURGE));
                                        recognized();
                                        return;
                                    }
                                }
                                // Process one event per scope so every COM payload is released.
                                break;
                            }
                        }
                        recognizer->SetRecoState(SPRST_INACTIVE_WITH_PURGE);
                    }
                    catch (...)
                    {
                        if (!token.stop_requested() && !warned)
                        {
                            warning(L"语音唤醒暂不可用：" + ErrorText() +
                                    L" 请检查麦克风与 Windows 语音组件；仍可双击快捷键。");
                            warned = true;
                        }
                        if (WaitStop(token, 5000))
                            break;
                    }
                }
            }
            catch (...)
            {
                if (!token.stop_requested())
                    warning(L"无法初始化本机语音唤醒：" + ErrorText());
            }
        });
}

std::wstring TranscribeLocal(const Audio& audio, std::wstring_view language, std::stop_token token)
{
    CheckCancel(token);
    if (audio.Silent())
        return {};
    auto recognizer = CreateRecognizer(language);
    ComPtr<IStream> memory;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &memory));
    ULONG written{};
    Check(memory->Write(
        audio.pcm.data(), static_cast<ULONG>(audio.pcm.size() * sizeof(int16_t)), &written));
    LARGE_INTEGER offset{};
    Check(memory->Seek(offset, STREAM_SEEK_SET, nullptr));
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
    ComPtr<ISpStream> stream;
    Check(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream)));
    Check(stream->SetBaseStream(memory.Get(), SPDFID_WaveFormatEx, &format));
    Check(recognizer->SetInput(stream.Get(), TRUE));
    ComPtr<ISpRecoContext> context;
    Check(recognizer->CreateRecoContext(&context));
    Check(context->SetNotifyWin32Event());
    Check(context->SetInterest(SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM),
                               SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM)));
    ComPtr<ISpRecoGrammar> grammar;
    Check(context->CreateGrammar(1, &grammar));
    Check(grammar->LoadDictation(nullptr, SPLO_STATIC));
    Check(grammar->SetDictationState(SPRS_ACTIVE));
    Check(recognizer->SetRecoState(SPRST_ACTIVE_ALWAYS));
    std::wstring result;
    auto deadline = Seconds() + std::clamp(audio.pcm.size() / 16000. * 3 + 10, 15., 180.);
    bool ended = false;
    while (!ended)
    {
        CheckCancel(token);
        if (Seconds() > deadline)
            throw Error(L"Windows 本机语音识别超时。");
        WaitForSingleObject(context->GetNotifyEventHandle(), 50);
        SPEVENT event{};
        ULONG fetched{};
        while (context->GetEvents(1, &event, &fetched) == S_OK && fetched)
        {
            if (event.eEventId == SPEI_END_SR_STREAM)
                ended = true;
            if (event.eEventId == SPEI_RECOGNITION)
            {
                auto* recognition = reinterpret_cast<ISpRecoResult*>(event.lParam);
                LPWSTR text{};
                if (SUCCEEDED(recognition->GetText(
                        static_cast<ULONG>(-1), static_cast<ULONG>(-1), TRUE, &text, nullptr)))
                {
                    if (!result.empty())
                        result += L" ";
                    result += text;
                    CoTaskMemFree(text);
                }
            }
            if (event.elParamType == SPET_LPARAM_IS_OBJECT ||
                event.elParamType == SPET_LPARAM_IS_TOKEN)
                reinterpret_cast<IUnknown*>(event.lParam)->Release();
            else if (event.elParamType == SPET_LPARAM_IS_POINTER ||
                     event.elParamType == SPET_LPARAM_IS_STRING)
                CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
            event = {};
        }
    }
    CheckCancel(token);
    return Trim(result);
}
} // namespace da
