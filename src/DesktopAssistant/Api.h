#pragma once
#include "Core.h"

namespace da
{
JsonObject AnswerRequest(const Settings& settings,
                         std::wstring_view question,
                         std::wstring_view transcript,
                         const std::vector<Screen>& screens,
                         const std::vector<Turn>& history);

class ResponseStream
{
    bool chat_, completed_{};
    std::string pending_, data_;
    std::wstring answer_;
    std::function<void(std::wstring)> delta_;
    void Event();

public:
    explicit ResponseStream(bool chat, std::function<void(std::wstring)> delta = {})
        : chat_(chat), delta_(std::move(delta))
    {
    }

    void Feed(std::string_view bytes);
    std::wstring Finish();
};

struct Multipart
{
    std::string contentType;
    Bytes body;
};

Multipart TranscriptionRequest(const Audio& audio, std::wstring_view model);
Settings SpeechSettings(const Settings& settings);
JsonObject AudioChatRequest(const Audio& audio, std::wstring_view model, bool dashScope);
JsonObject TtsRequest(const Settings& settings, std::wstring_view text);
std::vector<std::wstring> SpeechChunks(std::wstring_view text);
Bytes SynthesizeRemote(const Settings& settings, std::wstring_view text, std::stop_token token);
void PlayWav(const Bytes& wav, std::stop_token token);
std::string HttpPost(const Settings& settings,
                     std::wstring_view route,
                     std::string_view contentType,
                     std::span<const uint8_t> body,
                     std::stop_token token,
                     std::function<void(std::string_view)> onBytes = {});
std::wstring Answer(const Settings& settings,
                    std::wstring_view question,
                    std::wstring_view transcript,
                    const std::vector<Screen>& screens,
                    const std::vector<Turn>& history,
                    std::stop_token token,
                    std::function<void(std::wstring)> delta);
std::wstring TranscribeRemote(const Settings& settings,
                              const Audio& audio,
                              std::wstring_view model,
                              std::stop_token token);
} // namespace da
