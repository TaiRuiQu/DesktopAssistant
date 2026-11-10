#include "Api.h"
#include <mmsystem.h>
#include <cstring>

namespace da
{
namespace
{
struct HttpHandle
{
    std::atomic<HINTERNET> value;

    explicit HttpHandle(HINTERNET handle) : value(handle)
    {
        if (!handle)
            throw Error(L"无法连接语音文件服务。");
    }

    void Close()
    {
        if (auto handle = value.exchange(nullptr))
            WinHttpCloseHandle(handle);
    }

    ~HttpHandle()
    {
        Close();
    }

    operator HINTERNET() const
    {
        return value.load();
    }
};

Bytes DownloadAudio(std::wstring url, std::stop_token token)
{
    CheckCancel(token);
    // DashScope can return signed OSS HTTP URLs; use their HTTPS equivalent.
    if (url.starts_with(L"http://") && !url.starts_with(L"http://127.0.0.1:") &&
        !url.starts_with(L"http://localhost:"))
        url.replace(0, 4, L"https");
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (url.find_first_of(L"\r\n\t#") != url.npos ||
        !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
        parts.dwUserNameLength || parts.dwPasswordLength)
        throw Error(L"语音服务返回了无效的音频地址。");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    if (parts.nScheme != INTERNET_SCHEME_HTTPS &&
        !(parts.nScheme == INTERNET_SCHEME_HTTP && (host == L"127.0.0.1" || host == L"localhost")))
        throw Error(L"语音文件地址必须使用 HTTPS。");
    auto path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HttpHandle session(WinHttpOpen(L"DesktopAssistant/1.0",
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS,
                                   0));
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
    HttpHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    HttpHandle request(
        WinHttpOpenRequest(connection,
                           L"GET",
                           path.c_str(),
                           nullptr,
                           WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                           parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    std::stop_callback cancel(token,
                              [&]
                              {
                                  request.Close();
                              });
    auto verify = [&](BOOL ok)
    {
        CheckCancel(token);
        if (!ok)
            throw Error(L"下载语音文件失败或超时。");
    };
    // Signed download URLs never receive an API Authorization header.
    verify(WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0));
    verify(WinHttpReceiveResponse(request, nullptr));
    DWORD status{}, size = sizeof(status);
    verify(WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX,
                               &status,
                               &size,
                               WINHTTP_NO_HEADER_INDEX));
    if (status != 200)
        throw Error(L"语音文件下载失败，请重新朗读。 (HTTP " + std::to_wstring(status) + L")");
    Bytes result;
    uint8_t buffer[16384];
    DWORD read{};
    auto start = Clock::now();
    for (;;)
    {
        verify(WinHttpReadData(request, buffer, sizeof(buffer), &read));
        if (!read)
            break;
        if (result.size() + read > 32 * 1024 * 1024 ||
            Clock::now() - start > std::chrono::minutes(3))
            throw Error(L"语音文件过大或下载超时。");
        result.insert(result.end(), buffer, buffer + read);
    }
    CheckCancel(token);
    return result;
}

Bytes PcmWav(Bytes pcm)
{
    if (pcm.size() >= 12 && !memcmp(pcm.data(), "RIFF", 4) && !memcmp(pcm.data() + 8, "WAVE", 4))
        return pcm;
    if (pcm.empty() || pcm.size() % 2)
        throw Error(L"语音服务返回的音频格式无效。");
    // Non-streaming Qwen-TTS audio.data is 24 kHz mono PCM16.
    auto header = Audio{{0}}.Wav();
    header.resize(44);
    auto write32 = [&](size_t offset, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    write32(4, static_cast<uint32_t>(pcm.size() + 36));
    write32(24, 24000);
    write32(28, 48000);
    write32(40, static_cast<uint32_t>(pcm.size()));
    header.insert(header.end(), pcm.begin(), pcm.end());
    return header;
}
} // namespace

std::vector<std::wstring> SpeechChunks(std::wstring_view text)
{
    std::vector<std::wstring> result;
    while (!text.empty())
    {
        size_t count = std::min<size_t>(500, text.size());
        if (count < text.size())
        {
            auto sentence = text.substr(0, count).find_last_of(L"。！？；.!?;\n");
            if (sentence != text.npos && sentence >= count / 2)
                count = sentence + 1;
            if (text[count - 1] >= 0xD800 && text[count - 1] <= 0xDBFF)
                --count;
        }
        auto chunk = Trim(std::wstring(text.substr(0, count)));
        if (!chunk.empty())
            result.push_back(std::move(chunk));
        text.remove_prefix(count);
    }
    return result;
}

JsonObject TtsRequest(const Settings& s, std::wstring_view text)
{
    if (text.empty() || text.size() > 600)
        throw Error(L"每段朗读文字需要在 1–600 字符之间。");
    JsonObject request, input;
    request.SetNamedValue(L"model", J(s.TtsModel));
    input.SetNamedValue(L"text", J(text));
    input.SetNamedValue(L"voice", J(s.TtsVoice));
    input.SetNamedValue(L"language_type", J(L"Auto"));
    request.SetNamedValue(L"input", input);
    return request;
}

Bytes SynthesizeRemote(const Settings& s, std::wstring_view text, std::stop_token token)
{
    CheckCancel(token);
    auto endpoint = s;
    endpoint.BaseUrl = s.TtsBaseUrl;
    endpoint.ApiKey = s.TtsApiKey;
    if (endpoint.ApiKey.empty() && ParseEndpoint(endpoint.BaseUrl).secure)
        throw Error(L"请在「语音合成」中填写 DashScope API Key。");
    auto body = Utf8(TtsRequest(s, text).Stringify());
    auto response = HttpPost(endpoint,
                             L"/services/aigc/multimodal-generation/generation",
                             "application/json",
                             {reinterpret_cast<const uint8_t*>(body.data()), body.size()},
                             token);
    try
    {
        auto document = JsonObject::Parse(Wide(response));
        if (!String(document, L"code").empty() ||
            document.GetNamedNumber(L"status_code", 200) != 200)
            throw Error(L"语音合成失败，请检查模型、音色、地域及额度。");
        auto audio = document.GetNamedObject(L"output").GetNamedObject(L"audio");
        auto url = String(audio, L"url");
        if (!url.empty())
            return DownloadAudio(url, token);
        auto data = String(audio, L"data");
        if (!data.empty())
            return PcmWav(Unbase64(Utf8(data)));
        throw Error(L"语音服务没有返回音频。");
    }
    catch (const winrt::hresult_error&)
    {
        throw Error(L"语音合成返回格式无效，请检查 DashScope 接口。");
    }
}

void PlayWav(const Bytes& wav, std::stop_token token)
{
    CheckCancel(token);
    if (wav.size() < 12 || memcmp(wav.data(), "RIFF", 4) || memcmp(wav.data() + 8, "WAVE", 4))
        throw Error(L"语音服务未返回有效的 WAV 音频。");
    WAVEFORMATEX format{};
    const uint8_t* samples = nullptr;
    uint32_t sampleBytes = 0;
    for (size_t offset = 12; offset + 8 <= wav.size();)
    {
        uint32_t size{};
        memcpy(&size, wav.data() + offset + 4, 4);
        if (size > wav.size() - offset - 8)
            throw Error(L"语音文件不完整。");
        if (!memcmp(wav.data() + offset, "fmt ", 4) && size >= 16)
            memcpy(&format, wav.data() + offset + 8, std::min<size_t>(size, sizeof(format)));
        if (!memcmp(wav.data() + offset, "data", 4))
        {
            samples = wav.data() + offset + 8;
            sampleBytes = size;
        }
        offset += 8 + static_cast<size_t>(size) + (size & 1);
    }
    if (!samples || !sampleBytes || format.wFormatTag != WAVE_FORMAT_PCM ||
        format.wBitsPerSample != 16 || format.nChannels < 1 || format.nChannels > 2 ||
        format.nBlockAlign != format.nChannels * 2 || sampleBytes % format.nBlockAlign ||
        format.nSamplesPerSec < 8000 || format.nSamplesPerSec > 192000 ||
        format.nAvgBytesPerSec != format.nSamplesPerSec * format.nBlockAlign)
        throw Error(L"朗读需要有效的 16 位 PCM WAV 音频。");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw Error(L"无法创建语音播放事件。");
    HWAVEOUT device{};
    if (waveOutOpen(
            &device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event), 0, CALLBACK_EVENT) !=
        MMSYSERR_NOERROR)
    {
        CloseHandle(event);
        throw Error(L"无法打开扬声器，请检查默认播放设备。");
    }
    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(const_cast<uint8_t*>(samples));
    header.dwBufferLength = sampleBytes;
    auto prepared = waveOutPrepareHeader(device, &header, sizeof(header));
    auto written = prepared == MMSYSERR_NOERROR ? waveOutWrite(device, &header, sizeof(header))
                                                : MMSYSERR_ERROR;
    if (written == MMSYSERR_NOERROR)
        while (!(header.dwFlags & WHDR_DONE) && !token.stop_requested())
            WaitForSingleObject(event, 25);
    waveOutReset(device);
    if (prepared == MMSYSERR_NOERROR)
        waveOutUnprepareHeader(device, &header, sizeof(header));
    waveOutClose(device);
    CloseHandle(event);
    CheckCancel(token);
    if (written != MMSYSERR_NOERROR)
        throw Error(L"播放语音失败，请检查播放设备。");
}
} // namespace da
