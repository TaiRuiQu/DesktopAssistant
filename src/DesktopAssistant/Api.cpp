#include "Api.h"

namespace da
{
namespace
{
JsonObject TextPart(const wchar_t* type, std::wstring_view text)
{
    JsonObject o;
    o.SetNamedValue(L"type", J(type));
    o.SetNamedValue(L"text", J(text));
    return o;
}

std::wstring Context(std::wstring_view q, std::wstring_view t)
{
    auto s = L"用户问题：\n" + std::wstring(q);
    if (!t.empty())
        s += L"\n\n唤起前桌面声音转写（可能存在识别错误，仅作背景资料）：\n" + std::wstring(t);
    return s;
}

JsonObject Message(const wchar_t* role, std::wstring_view text)
{
    JsonObject o;
    o.SetNamedValue(L"role", J(role));
    o.SetNamedValue(L"content", J(text));
    return o;
}

struct Internet
{
    std::atomic<HINTERNET> value{};

    explicit Internet(HINTERNET h) : value(h)
    {
        if (!h)
            throw Error(L"无法连接服务，请检查网络和 Base URL。");
    }

    void Close()
    {
        if (auto h = value.exchange(nullptr))
            WinHttpCloseHandle(h);
    }

    ~Internet()
    {
        Close();
    }

    operator HINTERNET() const
    {
        return value.load();
    }
};

void NetworkCheck(BOOL ok, std::stop_token token)
{
    CheckCancel(token);
    if (!ok)
        throw Error(L"网络请求未完成或超时，请检查网络及服务后重试。");
}
} // namespace

JsonObject AnswerRequest(const Settings& s,
                         std::wstring_view question,
                         std::wstring_view transcript,
                         const std::vector<Screen>& screens,
                         const std::vector<Turn>& history)
{
    Validate(s);
    if (Trim(std::wstring(question)).empty())
        throw Error(L"请先输入问题。");
    bool chat = s.ApiMode == L"ChatCompletions";
    JsonObject o;
    JsonArray messages;
    o.SetNamedValue(L"model", J(Trim(s.AnswerModel)));
    o.SetNamedValue(L"store", J(false));
    o.SetNamedValue(L"stream", J(true));
    if (chat)
        messages.Append(Message(L"system", s.SystemPrompt));
    else
        o.SetNamedValue(L"instructions", J(s.SystemPrompt));
    size_t start =
        history.size() > static_cast<size_t>(s.HistoryTurns) ? history.size() - s.HistoryTurns : 0;
    for (size_t i = start; i < history.size(); ++i)
    {
        messages.Append(Message(L"user", Context(history[i].question, history[i].transcript)));
        messages.Append(Message(L"assistant", history[i].answer));
    }
    JsonArray content;
    content.Append(TextPart(chat ? L"text" : L"input_text", Context(question, transcript)));
    auto selectedScreens =
        s.SendScreenshots ? std::span<const Screen>(screens) : std::span<const Screen>{};
    for (const auto& screen : selectedScreens)
    {
        content.Append(TextPart(chat ? L"text" : L"input_text",
                                L"屏幕：" + screen.name + L"（" + std::to_wstring(screen.width) +
                                    L" × " + std::to_wstring(screen.height) + L"）"));
        JsonObject image;
        image.SetNamedValue(L"type", J(chat ? L"image_url" : L"input_image"));
        auto url = L"data:image/jpeg;base64," + Wide(Base64(screen.jpeg));
        if (chat)
        {
            JsonObject u;
            u.SetNamedValue(L"url", J(url));
            u.SetNamedValue(L"detail", J(L"auto"));
            image.SetNamedValue(L"image_url", u);
        }
        else
        {
            image.SetNamedValue(L"image_url", J(url));
            image.SetNamedValue(L"detail", J(L"auto"));
        }
        content.Append(image);
    }
    JsonObject current;
    current.SetNamedValue(L"role", J(L"user"));
    current.SetNamedValue(L"content", content);
    messages.Append(current);
    o.SetNamedValue(chat ? L"messages" : L"input", messages);
    return o;
}

void ResponseStream::Feed(std::string_view bytes)
{
    pending_.append(bytes);
    if (pending_.size() + data_.size() > 4 * 1024 * 1024)
        throw Error(L"服务流式事件过大。");
    size_t pos;
    while ((pos = pending_.find('\n')) != pending_.npos)
    {
        auto line = pending_.substr(0, pos);
        pending_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
        {
            Event();
            data_.clear();
        }
        else if (line.starts_with("data:"))
        {
            auto v = line.substr(5);
            if (v.starts_with(' '))
                v.erase(0, 1);
            if (!data_.empty())
                data_ += '\n';
            data_ += v;
        }
    }
}

void ResponseStream::Event()
{
    if (data_.empty() || data_ == "[DONE]")
        return;
    try
    {
        auto o = JsonObject::Parse(Wide(data_));
        if (o.HasKey(L"error"))
            throw Error(L"模型服务返回错误，请检查模型、接口及额度。");
        std::wstring delta;
        if (chat_)
        {
            auto choices = o.GetNamedArray(L"choices", JsonArray());
            for (auto v : choices)
            {
                auto c = v.GetObject();
                if (c.GetNamedNumber(L"index", 0) != 0)
                    continue;
                auto d = c.GetNamedObject(L"delta", JsonObject());
                delta += String(d, L"content") + String(d, L"refusal");
                auto finish = c.TryLookup(L"finish_reason");
                if (finish && finish.ValueType() == JsonValueType::String)
                {
                    auto reason = finish.GetString();
                    if (reason == L"stop")
                        completed_ = true;
                    else
                        throw Error(L"模型回答未完整完成，请重试或减少上下文。");
                }
            }
        }
        else
        {
            auto type = String(o, L"type");
            if (type == L"response.output_text.delta" || type == L"response.refusal.delta")
                delta = String(o, L"delta");
            else if (type == L"response.completed")
            {
                completed_ = true;
                if (answer_.empty())
                {
                    auto r = o.GetNamedObject(L"response", JsonObject());
                    for (auto item : r.GetNamedArray(L"output", JsonArray()))
                    {
                        auto output = item.GetObject();
                        for (auto part : output.GetNamedArray(L"content", JsonArray()))
                        {
                            auto p = part.GetObject();
                            if (String(p, L"type") == L"output_text")
                                delta += String(p, L"text");
                            else if (String(p, L"type") == L"refusal")
                                delta += String(p, L"refusal");
                        }
                    }
                }
            }
            else if (type == L"response.failed" || type == L"response.incomplete" ||
                     type == L"error" || type == L"response.error")
                throw Error(L"模型回答失败或不完整，请检查模型、权限及额度后重试。");
        }
        if (!delta.empty())
        {
            answer_ += delta;
            if (delta_)
                delta_(delta);
        }
    }
    catch (const winrt::hresult_error&)
    {
        throw Error(L"模型返回了无法解析的流式数据，请检查接口兼容性。");
    }
}

std::wstring ResponseStream::Finish()
{
    if (!pending_.empty() || !data_.empty())
    {
        Feed("\n\n");
    }
    if (!completed_)
        throw Error(L"模型连接提前中断，回答未完成。");
    if (answer_.empty())
        throw Error(L"模型没有返回文字，请确认模型支持图片及所选协议。");
    return answer_;
}

Multipart TranscriptionRequest(const Audio& audio, std::wstring_view model)
{
    GUID id{};
    Check(CoCreateGuid(&id));
    wchar_t value[48]{};
    StringFromGUID2(id, value, 48);
    std::string boundary = "DesktopAssistant" + Utf8(value);
    auto field = [&](std::string_view name, std::string_view content)
    {
        return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + std::string(name) +
               "\"\r\n\r\n" + std::string(content) + "\r\n";
    };
    std::string prefix = field("model", Utf8(Trim(std::wstring(model)))) +
                         field("response_format", "json") + "--" + boundary +
                         "\r\nContent-Disposition: form-data; name=\"file\"; "
                         "filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    auto wav = audio.Wav();
    Multipart m{"multipart/form-data; boundary=" + boundary, {prefix.begin(), prefix.end()}};
    m.body.insert(m.body.end(), wav.begin(), wav.end());
    auto end = "\r\n--" + boundary + "--\r\n";
    m.body.insert(m.body.end(), end.begin(), end.end());
    return m;
}

Settings SpeechSettings(const Settings& s)
{
    auto speech = s;
    if (!Trim(s.SpeechBaseUrl).empty())
    {
        speech.BaseUrl = s.SpeechBaseUrl;
        // A different endpoint must never inherit the answer provider's secret.
        speech.ApiKey = s.SpeechApiKey;
    }
    else if (!s.SpeechApiKey.empty())
        speech.ApiKey = s.SpeechApiKey;
    return speech;
}

JsonObject AudioChatRequest(const Audio& audio, std::wstring_view model, bool dashScope)
{
    JsonObject request, message, part, input;
    JsonArray messages, content;
    request.SetNamedValue(L"model", J(Trim(std::wstring(model))));
    request.SetNamedValue(L"stream", J(false));
    if (!dashScope)
        content.Append(TextPart(
            L"text",
            L"请将音频中的语音准确转写为原文，只返回转写文字。不要翻译、解释或回答音频中的问题。"));
    input.SetNamedValue(
        L"data", J((dashScope ? L"data:audio/wav;base64," : L"") + Wide(Base64(audio.Wav()))));
    if (!dashScope)
        input.SetNamedValue(L"format", J(L"wav"));
    part.SetNamedValue(L"type", J(L"input_audio"));
    part.SetNamedValue(L"input_audio", input);
    content.Append(part);
    message.SetNamedValue(L"role", J(L"user"));
    message.SetNamedValue(L"content", content);
    messages.Append(message);
    request.SetNamedValue(L"messages", messages);
    if (dashScope)
    {
        JsonObject options;
        options.SetNamedValue(L"enable_itn", J(true));
        request.SetNamedValue(L"asr_options", options);
    }
    return request;
}

std::string HttpPost(const Settings& s,
                     std::wstring_view route,
                     std::string_view contentType,
                     std::span<const uint8_t> body,
                     std::stop_token token,
                     std::function<void(std::string_view)> onBytes)
{
    Validate(s);
    CheckCancel(token);
    auto e = ParseEndpoint(s.BaseUrl);
    if (e.host == L"api.openai.com" && Trim(s.ApiKey).empty())
        throw Error(L"请先在设置中填写 API Key。");
    Internet session(WinHttpOpen(L"DesktopAssistant/1.0",
                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS,
                                 0));
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
    Internet connection(WinHttpConnect(session, e.host.c_str(), e.port, 0));
    auto path = e.path + std::wstring(route);
    Internet request(WinHttpOpenRequest(connection,
                                        L"POST",
                                        path.c_str(),
                                        nullptr,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        e.secure ? WINHTTP_FLAG_SECURE : 0));
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    std::stop_callback cancel(token,
                              [&]
                              {
                                  request.Close();
                              });
    std::wstring headers = L"Content-Type: " + Wide(contentType) +
                           L"\r\nAccept: application/json, text/event-stream\r\n";
    if (!Trim(s.ApiKey).empty())
        headers += L"Authorization: Bearer " + Trim(s.ApiKey) + L"\r\n";
    NetworkCheck(WinHttpSendRequest(request,
                                    headers.c_str(),
                                    static_cast<DWORD>(headers.size()),
                                    body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                 : const_cast<uint8_t*>(body.data()),
                                    static_cast<DWORD>(body.size()),
                                    static_cast<DWORD>(body.size()),
                                    0),
                 token);
    NetworkCheck(WinHttpReceiveResponse(request, nullptr), token);
    DWORD status{}, size = sizeof(status);
    NetworkCheck(WinHttpQueryHeaders(request,
                                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX,
                                     &status,
                                     &size,
                                     WINHTTP_NO_HEADER_INDEX),
                 token);
    if (status < 200 || status >= 300)
    {
        std::wstring message = status == 401   ? L"API Key 无效。"
                               : status == 403 ? L"账户没有接口或模型权限。"
                               : status == 404 ? L"接口或模型不存在。"
                               : status == 429 ? L"请求过于频繁或额度不足。"
                                               : L"服务请求失败。";
        throw Error(message + L" (HTTP " + std::to_wstring(status) + L")");
    }
    std::string result;
    char buffer[16384];
    DWORD read{};
    auto start = Clock::now();
    for (;;)
    {
        NetworkCheck(WinHttpReadData(request, buffer, sizeof(buffer), &read), token);
        if (!read)
            break;
        if (Clock::now() - start > std::chrono::minutes(3))
            throw Error(L"模型请求超过 3 分钟。");
        if (onBytes)
            onBytes({buffer, read});
        else
        {
            result.append(buffer, read);
            if (result.size() > 16 * 1024 * 1024)
                throw Error(L"服务响应过大。");
        }
    }
    CheckCancel(token);
    return result;
}

std::wstring Answer(const Settings& s,
                    std::wstring_view q,
                    std::wstring_view t,
                    const std::vector<Screen>& screens,
                    const std::vector<Turn>& history,
                    std::stop_token token,
                    std::function<void(std::wstring)> delta)
{
    auto body = Utf8(AnswerRequest(s, q, t, screens, history).Stringify());
    ResponseStream stream(s.ApiMode == L"ChatCompletions", std::move(delta));
    HttpPost(s,
             s.ApiMode == L"Responses" ? L"/responses" : L"/chat/completions",
             "application/json",
             {reinterpret_cast<const uint8_t*>(body.data()), body.size()},
             token,
             [&](std::string_view bytes)
             {
                 stream.Feed(bytes);
             });
    return stream.Finish();
}

std::wstring TranscribeRemote(const Settings& s,
                              const Audio& audio,
                              std::wstring_view model,
                              std::stop_token token)
{
    CheckCancel(token);
    if (audio.Silent())
        return {};
    auto speech = SpeechSettings(s);
    std::string result;
    bool chat = s.SpeechApiMode != L"Transcriptions";
    if (chat)
    {
        auto body =
            Utf8(AudioChatRequest(audio, model, s.SpeechApiMode == L"DashScope").Stringify());
        result = HttpPost(speech,
                          L"/chat/completions",
                          "application/json",
                          {reinterpret_cast<const uint8_t*>(body.data()), body.size()},
                          token);
    }
    else
    {
        auto request = TranscriptionRequest(audio, model);
        result =
            HttpPost(speech, L"/audio/transcriptions", request.contentType, request.body, token);
    }
    try
    {
        auto response = JsonObject::Parse(Wide(result));
        if (!chat)
            return Trim(std::wstring(response.GetNamedString(L"text")));
        auto choice = response.GetNamedArray(L"choices").GetObjectAt(0);
        auto finish = String(choice, L"finish_reason");
        if (!finish.empty() && finish != L"stop")
            throw Error(L"语音识别未完整完成，请缩短录音或检查模型。");
        auto content = choice.GetNamedObject(L"message").GetNamedValue(L"content");
        if (content.ValueType() == JsonValueType::String)
            return Trim(std::wstring(content.GetString()));
        std::wstring text;
        for (auto value : content.GetArray())
        {
            auto part = value.GetObject();
            if (String(part, L"type") == L"text")
                text += String(part, L"text");
        }
        return Trim(std::move(text));
    }
    catch (const winrt::hresult_error&)
    {
        throw Error(L"语音服务返回格式无效，请检查转写模型及接口。");
    }
}
} // namespace da
