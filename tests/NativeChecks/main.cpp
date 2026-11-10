#include "Api.h"
#include "Capture.h"
#include "Markdown.h"
#include "DuckingChecks.h"
#include <iostream>
#include <future>
using namespace da;
int checks{};

void Expect(bool value, const char* description)
{
    if (!value)
        throw Error(Wide(description));
    ++checks;
    std::cout << "PASS: " << description << "\n";
}

template <class F> void Reject(F f, const char* description)
{
    bool rejected = false;
    try
    {
        f();
    }
    catch (...)
    {
        rejected = true;
    }
    Expect(rejected, description);
}

int wmain(int argc, wchar_t** argv)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto temp = std::filesystem::temp_directory_path() /
                (L"DesktopAssistant-NativeChecks-" + std::to_wstring(GetCurrentProcessId()));
    try
    {
        if (argc > 2 && std::wstring(argv[1]) == L"--ducking-session")
            return RunDuckingFixture(argv[2]);
        if (argc > 2 && std::wstring(argv[1]) == L"--migration-fixture")
        {
            std::filesystem::path directory = argv[2];
            auto legacy = ReadSettings(directory);
            auto history = ReadHistory(directory);
            Expect(legacy.ApiKey == L"legacy-fixture-key" && legacy.AnswerModel == L"legacy-model",
                   "read original .NET settings and DPAPI key");
            Expect(legacy.SpeechBaseUrl.empty() && SpeechSettings(legacy).ApiKey == legacy.ApiKey &&
                       legacy.SpeechApiMode == L"Transcriptions" && legacy.TtsApiKey.empty(),
                   "legacy configuration keeps shared speech credentials and default TTS");
            Expect(history.size() == 1 && history[0].question == L"legacy question" &&
                       history[0].transcript == L"legacy desktop",
                   "read original .NET encrypted history");
            legacy.ApiKey = L"native-fixture-key";
            SaveSettings(legacy, directory);
            history.push_back(
                {L"native question", L"native answer", L"native desktop", Timestamp()});
            SaveHistory(history, directory);
            return 0;
        }
        Settings s;
        Validate(s);
        Expect(s.SilenceSeconds == 4. && !s.WakeWordEnabled && s.WakePhrase == L"小弟小弟",
               "longer pause default and opt-in local wake phrase");
        Expect(MatchesWakePhrase(L" 小弟，小弟。", L"小弟小弟") &&
                   MatchesWakePhrase(L"HEY Assistant!", L"hey assistant") &&
                   !MatchesWakePhrase(L"小弟", L"小弟小弟") &&
                   !MatchesWakePhrase(L"小弟小弟今天天气", L"小弟小弟") &&
                   !MatchesWakePhrase(L"。", L"。"),
               "wake matching ignores punctuation and spacing but requires the whole phrase");
        auto wakeSettings = s;
        wakeSettings.WakeWordEnabled = true;
        for (auto phrase : {L"", L"  ", L"。", L"小弟\n小弟"})
        {
            wakeSettings.WakePhrase = phrase;
            Reject(
                [&]
                {
                    Validate(wakeSettings);
                },
                "reject invalid enabled wake phrase");
        }
        VoiceActivity waiting(s, 0.);
        std::vector<int16_t> click(320, 2000), voice(1920, 1000), softVoice(1920, 350);
        waiting.Append(click, .02);
        Expect(!waiting.Spoke() && !waiting.Finished(19.9) && waiting.Finished(20.),
               "brief noise does not start silence countdown and first speech has 20 seconds");
        VoiceActivity activity(s, 0.);
        activity.Append(voice, .12);
        Expect(activity.Spoke() && !activity.Finished(3.9),
               "sustained voice tolerates a thinking pause longer than the former default");
        activity.Append(softVoice, 3.9);
        Expect(!activity.Finished(7.8) && activity.Finished(8.),
               "soft continuation resets the full silence window");
        VoiceActivity partitioned(s, 0.);
        for (int i = 0; i < 12; ++i)
            partitioned.Append(std::span<const int16_t>(voice).subspan(i * 160, 160),
                               (i + 1) * .01);
        Expect(partitioned.Spoke() && partitioned.Finished(4.2),
               "VAD handles audio packets smaller than a detection frame");
        auto longWait = s;
        longWait.SilenceSeconds = 15.;
        longWait.MaxRecordingSeconds = 5;
        Validate(longWait);
        VoiceActivity boundedRecording(longWait, 0.);
        boundedRecording.Append(voice, .12);
        Expect(!boundedRecording.Finished(4.9) && boundedRecording.Finished(5.),
               "recording maximum remains effective with a long silence setting");
        auto markdown =
            ParseMarkdown(L"## 中文字体\n\n| 名称 | 备注 |\n| :--- | ---: |\n| **微软雅黑** | 正文 "
                          L"|\n\n普通文字 **局部加粗** 和 `**原样代码**`。\n\n1. 第一项\n   - "
                          L"嵌套列表\n\n> 引用\n\n```cpp\n中文 **literal**\n```\n\n- [x] "
                          L"已完成\n\n[文档](https://example.com) 和 ~~删除线~~");
        Expect(markdown.children[0].kind == MarkdownKind::Heading &&
                   markdown.children[0].value == 2,
               "CommonMark Chinese heading parsed independently");
        Expect(markdown.children[1].kind == MarkdownKind::Table &&
                   markdown.children[1].children[1].children[0].children.size() == 2,
               "GFM table creates header, rows and real columns");
        Expect(markdown.children[2].kind == MarkdownKind::Paragraph &&
                   markdown.children[2].children[0].kind == MarkdownKind::Text,
               "body paragraph does not inherit heading or bold markup");
        auto plainMarkdown = MarkdownText(markdown);
        Expect(plainMarkdown.find(L"微软雅黑") != plainMarkdown.npos &&
                   plainMarkdown.find(L"**原样代码**") != plainMarkdown.npos &&
                   plainMarkdown.find(L"| :---") == plainMarkdown.npos,
               "plain text keeps table and literal code while removing layout markup");
        auto nested = ParseMarkdown(L"1. 第一项\n   - 子项\n2. 第二项");
        Expect(nested.children.size() == 1 && nested.children[0].ordered &&
                   nested.children[0].children[0].children[1].kind == MarkdownKind::List,
               "ordered and nested lists preserve structure");
        auto escaped = ParseMarkdown(L"转义 \\*普通\\* &amp; &#x4E2D;\n\n未闭合 **强调");
        Expect(MarkdownText(escaped).find(L"*普通* & 中") != std::wstring::npos,
               "escaped Markdown and numeric entities preserve Chinese text");
        for (size_t length = 0; length <= 100; ++length)
        {
            auto partial = std::wstring_view(
                L"## 标题\n\n| A | B |\n| --- | --- |\n| **中文** | `代码` |\n\n```cpp\ncode\n```");
            ParseMarkdown(partial.substr(0, std::min(length, partial.size())));
        }
        Expect(true, "partial streaming tables, emphasis and fences parse safely");
        auto local = s;
        local.BaseUrl = L"http://127.0.0.1:4000/v1/";
        Validate(local);
        Expect(ParseEndpoint(local.BaseUrl).path == L"/v1", "local endpoint and base path");
        for (auto url : {L"http://example.com/v1",
                         L"https://user:pass@example.com/v1",
                         L"https://example.com/v1?key=a",
                         L"https://example.com/v1#part"})
        {
            auto invalid = s;
            invalid.BaseUrl = url;
            Reject(
                [&]
                {
                    Validate(invalid);
                },
                "reject unsafe endpoint");
        }
        auto invalid = s;
        invalid.SilenceSeconds = NAN;
        Reject(
            [&]
            {
                Validate(invalid);
            },
            "reject non-finite setting");
        s.ApiKey = L"native-check-secret";
        s.SpeechApiKey = L"speech-check-secret";
        s.TtsApiKey = L"tts-check-secret";
        s.DuckOtherAudio = false;
        s.WakeWordEnabled = true;
        s.WakePhrase = L"你好助手";
        SaveSettings(s, temp);
        auto loaded = ReadSettings(temp);
        Expect(loaded.ApiKey == s.ApiKey && loaded.SilenceSeconds == s.SilenceSeconds,
               "settings and DPAPI round trip");
        Expect(loaded.SpeechApiKey == s.SpeechApiKey && loaded.TtsApiKey == s.TtsApiKey,
               "ASR and TTS keys independently encrypted and restored");
        Expect(!loaded.DuckOtherAudio && Settings{}.DuckOtherAudio,
               "audio ducking defaults on and preference is persisted");
        Expect(loaded.WakeWordEnabled && loaded.WakePhrase == L"你好助手",
               "wake switch and custom phrase survive settings round trip");
        auto oldSettings = JsonObject::Parse(Wide(ReadFile(temp / L"settings.json")));
        auto oldValues = oldSettings.GetNamedObject(L"Settings");
        oldValues.Remove(L"VoiceDetectionVersion");
        oldValues.Remove(L"WakeWordEnabled");
        oldValues.Remove(L"WakePhrase");
        oldValues.SetNamedValue(L"SilenceSeconds", J(1.5));
        AtomicWrite(temp / L"settings.json", Utf8(oldSettings.Stringify()));
        auto upgraded = ReadSettings(temp);
        Expect(upgraded.SilenceSeconds == 4. && !upgraded.WakeWordEnabled,
               "legacy default pause upgrades without enabling background microphone");
        oldValues.SetNamedValue(L"SilenceSeconds", J(2.7));
        AtomicWrite(temp / L"settings.json", Utf8(oldSettings.Stringify()));
        Expect(ReadSettings(temp).SilenceSeconds == 2.7,
               "custom legacy silence preference is preserved");
        s.SilenceSeconds = 1.5;
        SaveSettings(s, temp);
        Expect(ReadSettings(temp).SilenceSeconds == 1.5,
               "explicit short pause in new settings is preserved after restart");
        s.SilenceSeconds = 4.;
        SaveSettings(s, temp);
        s.DuckOtherAudio = true;
        Expect(ReadFile(temp / L"settings.json").find("native-check-secret") == std::string::npos,
               "key absent from settings plaintext");
        auto persisted = ReadFile(temp / L"settings.json");
        Expect(persisted.find("speech-check-secret") == persisted.npos &&
                   persisted.find("tts-check-secret") == persisted.npos,
               "ASR and TTS secrets absent from settings plaintext");
        s.SpeechApiKey.clear();
        s.TtsApiKey.clear();
        auto isolated = s;
        isolated.SpeechBaseUrl = L"https://openrouter.ai/api/v1";
        Expect(SpeechSettings(isolated).BaseUrl == isolated.SpeechBaseUrl &&
                   SpeechSettings(isolated).ApiKey.empty(),
               "independent ASR endpoint never inherits answer key");
        isolated.SpeechApiKey = L"asr-only-key";
        isolated.SpeechApiMode = L"DashScope";
        Expect(SpeechSettings(isolated).ApiKey == L"asr-only-key" &&
                   s.ApiKey == L"native-check-secret",
               "ASR credential routing leaves answer settings unchanged");
        isolated.SpeechBaseUrl = L"http://remote.example/v1";
        Reject(
            [&]
            {
                Validate(isolated);
            },
            "ASR rejects insecure remote endpoints");
        isolated = s;
        isolated.TtsApiKey = L"bad\r\nheader";
        Reject(
            [&]
            {
                Validate(isolated);
            },
            "TTS rejects authorization header injection");
        auto longSpeech = std::wstring(499, L'问') + L"\xD83D\xDE00" + std::wstring(900, L'答');
        auto chunks = SpeechChunks(longSpeech);
        std::wstring joined;
        bool bounded = true;
        for (auto const& chunk : chunks)
        {
            bounded = bounded && chunk.size() <= 500 &&
                      !(chunk.back() >= 0xD800 && chunk.back() <= 0xDBFF);
            joined += chunk;
        }
        Expect(bounded && joined == longSpeech,
               "long TTS text is chunked without truncation or split Unicode pairs");
        auto synthesis = TtsRequest(s, L"你好");
        Expect(String(synthesis, L"model") == L"qwen3-tts-flash" &&
                   String(synthesis.GetNamedObject(L"input"), L"voice") == L"Cherry",
               "DashScope TTS request uses input text and voice");
        Reject(
            [&]
            {
                TtsRequest(s, std::wstring(601, L'x'));
            },
            "TTS request enforces segment limit");
        Reject(
            [&]
            {
                PlayWav({1, 2, 3}, {});
            },
            "malformed audio is rejected before opening device");
        std::vector<Turn> history;
        for (int i = 0; i < 105; ++i)
            history.push_back({L"q" + std::to_wstring(i), L"answer", L"desktop", Timestamp()});
        SaveHistory(history, temp);
        auto turns = ReadHistory(temp);
        Expect(turns.size() == 100 && turns.front().question == L"q5",
               "encrypted history retains newest 100 turns");
        Expect(ReadFile(temp / L"history.dat").find("answer") == std::string::npos,
               "history encrypted at rest");
        ClearHistory(temp);
        Expect(ReadHistory(temp).empty(), "new conversation clears persisted history");
        AtomicWrite(temp / L"settings.json", "corrupt");
        Reject(
            [&]
            {
                ReadSettings(temp);
            },
            "corrupt settings rejected");
        Expect(ReadFile(temp / L"settings.json") == "corrupt", "corrupt settings preserved");
        s.HistoryTurns = 1;
        std::vector<Screen> screens{{L"DISPLAY1", {1, 2, 3}, 100, 80}};
        auto request = AnswerRequest(s, L"question", L"current desktop", screens, history);
        auto input = request.GetNamedArray(L"input");
        Expect(input.Size() == 3, "history limit counts complete turns");
        Expect(String(input.GetObjectAt(0), L"content").find(L"desktop") != std::wstring::npos,
               "old desktop transcript in context");
        auto content = input.GetObjectAt(2).GetNamedArray(L"content");
        Expect(String(content.GetObjectAt(2), L"image_url") == L"data:image/jpeg;base64,AQID",
               "screenshots included as data images");
        Expect(!request.GetNamedBoolean(L"store") && request.GetNamedBoolean(L"stream"),
               "store false and streaming enabled");
        auto textOnly = s;
        textOnly.SendScreenshots = false;
        auto textRequest = AnswerRequest(textOnly, L"question", L"", screens, {});
        Expect(
            textRequest.GetNamedArray(L"input").GetObjectAt(0).GetNamedArray(L"content").Size() ==
                1,
            "text-only answer provider receives no screenshot image parts");
        std::wstring deltas;
        ResponseStream response(false,
                                [&](auto delta)
                                {
                                    deltas += delta;
                                });
        std::string stream = "event: response.output_text.delta\r\ndata: "
                             "{\"type\":\"response.output_text.delta\",\"delta\":\"你好\"}"
                             "\r\n\r\ndata: {\"type\":\"response.completed\"}\n\n";
        for (char c : stream)
            response.Feed({&c, 1});
        Expect(response.Finish() == L"你好" && deltas == L"你好",
               "UTF-8 SSE across arbitrary byte boundaries");
        ResponseStream final(false);
        final.Feed("data: "
                   "{\"type\":\"response.completed\",\"response\":{\"output\":[{\"content\":[{"
                   "\"type\":\"output_text\",\"text\":\"fallback\"}]}]}}\n\n");
        Expect(final.Finish() == L"fallback", "completed response supplies final text");
        ResponseStream broken(false);
        broken.Feed("data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n");
        Reject(
            [&]
            {
                broken.Finish();
            },
            "truncated stream never completes history");
        ResponseStream incomplete(false);
        Reject(
            [&]
            {
                incomplete.Feed("data: {\"type\":\"response.incomplete\"}\n\n");
            },
            "incomplete response rejected");
        s.ApiMode = L"ChatCompletions";
        s.HistoryTurns = 0;
        auto chat = AnswerRequest(s, L"question", L"", screens, history);
        Expect(chat.GetNamedArray(L"messages").Size() == 2,
               "zero history retains system and current chat messages");
        ResponseStream cs(true);
        cs.Feed("data: "
                "{\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":null}]}\n\ndata: "
                "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n");
        Expect(cs.Finish() == L"ok", "Chat Completions streaming");
        ResponseStream length(true);
        Reject(
            [&]
            {
                length.Feed(
                    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n");
            },
            "chat length cutoff rejected");
        Audio audio;
        audio.pcm.resize(16000, 1234);
        auto wav = audio.Wav();
        Expect(wav.size() == 32044 && memcmp(wav.data(), "RIFF", 4) == 0,
               "16k mono PCM WAV in memory");
        auto multi = TranscriptionRequest(audio, L"gpt-transcribe");
        auto bytes = std::string(multi.body.begin(), multi.body.end());
        Expect(bytes.find("gpt-transcribe") != bytes.npos &&
                   bytes.find("audio.wav") != bytes.npos && bytes.find("RIFF") != bytes.npos,
               "multipart includes model and original WAV");
        Audio silence;
        silence.pcm.resize(16000);
        Expect(silence.Silent(), "silence does not need upload");
        Timeline timeline;
        timeline.Reset(5);
        std::vector<int16_t> sound(16000, 100);
        timeline.Append(sound, 10);
        Expect(!timeline.Snapshot(10).Silent(), "timeline snapshots recent PCM");
        Expect(timeline.Snapshot(16).pcm.empty(), "silence expires old audio");
        for (int i = 0; i < 70; ++i)
            timeline.Append(sound, 20 + i);
        Expect(timeline.Snapshot(89).pcm.size() <= 80000, "timeline memory bounded by lookback");
        timeline.Reset(0);
        Expect(timeline.Snapshot(90).pcm.empty(), "disabled capture has no audio");
        DoubleTap tap;
        tap.Process(true, true, 100);
        tap.Process(true, false, 110);
        tap.Process(true, true, 200);
        Expect(tap.Process(true, false, 210), "double tap triggers on release");
        tap.Process(true, true, 1000);
        tap.Process(false, true, 1010);
        Expect(!tap.Process(true, false, 1020), "modifier chord does not trigger");
        tap.Process(true, true, 2000);
        Expect(!tap.Process(true, false, 3000), "long hold does not trigger");
        std::stop_source cancel;
        cancel.request_stop();
        Reject(
            [&]
            {
                HttpPost(local, L"/responses", "application/json", {}, cancel.get_token());
            },
            "pre-cancelled network request stops before connect");
        auto languages = SpeechLanguages();
        std::cout << "Installed SAPI recognizers: " << languages.size() << "\n";
        if (!languages.empty())
        {
            Audio tone;
            tone.pcm.resize(3200);
            for (size_t i = 0; i < tone.pcm.size(); ++i)
                tone.pcm[i] = static_cast<int16_t>(std::sin(i * 0.157) * 3000);
            auto text = TranscribeLocal(tone, languages[0].first, {});
            Expect(text.empty(), "real SAPI recognizes finite in-memory non-speech PCM");
        }
        Reject(
            [&]
            {
                TranscribeLocal(audio, L"invalid-language", {});
            },
            "missing speech language never silently changes recognizer");
        if (argc > 2 && std::wstring(argv[1]) == L"--http-tests")
        {
            auto http = s;
            http.BaseUrl = argv[2];
            http.ApiKey = L"fixture-key";
            http.ApiMode = L"Responses";
            Expect(Answer(http, L"http-ok", L"desktop", screens, {}, {}, {}) == L"fixture answer",
                   "WinHTTP sends Responses request and reads SSE");
            http.ApiMode = L"ChatCompletions";
            Expect(Answer(http, L"http-ok", L"", screens, {}, {}, {}) == L"fixture answer",
                   "WinHTTP Chat Completions transport");
            Expect(TranscribeRemote(http, audio, L"fixture-model", {}) == L"fixture transcript",
                   "WinHTTP multipart transcription");
            auto root = std::wstring(argv[2]);
            root.erase(root.rfind(L"/v1"));
            http.SpeechBaseUrl = root + L"/speech/v1";
            http.SpeechApiKey = L"speech-fixture-key";
            Expect(TranscribeRemote(http, audio, L"fixture-model", {}) == L"fixture transcript",
                   "ASR uses its own API endpoint and bearer key");
            http.SpeechApiMode = L"ChatCompletions";
            Expect(TranscribeRemote(http, audio, L"fixture-model", {}) == L"audio chat transcript",
                   "OpenRouter audio chat transcription request and response");
            http.SpeechApiMode = L"DashScope";
            Expect(TranscribeRemote(http, audio, L"fixture-model", {}) == L"dashscope transcript",
                   "DashScope ASR sends audio Data URI with ASR options");
            http.TtsBaseUrl = root + L"/tts/v1";
            http.TtsApiKey = L"tts-fixture-key";
            auto synthesized = SynthesizeRemote(http, L"inline-audio", {});
            Expect(synthesized.size() > 44 &&
                       std::string(synthesized.begin(), synthesized.begin() + 4) == "RIFF",
                   "DashScope TTS synthesizes inline audio with independent credentials");
            synthesized = SynthesizeRemote(http, L"inline-pcm", {});
            Expect(synthesized.size() == 524 && synthesized[24] == 0xc0 && synthesized[25] == 0x5d,
                   "DashScope PCM audio is wrapped as a 24 kHz WAV for playback");
            synthesized = SynthesizeRemote(http, L"download-audio", {});
            Expect(synthesized.size() > 44,
                   "TTS downloads signed WAV URL without API authorization");
            Reject(
                [&]
                {
                    SynthesizeRemote(http, L"invalid-audio-url", {});
                },
                "TTS rejects unsafe signed audio URLs");
            std::stop_source ttsCancel;
            std::jthread ttsStopper(
                [&]
                {
                    Sleep(200);
                    ttsCancel.request_stop();
                });
            bool ttsCancelled = false;
            auto ttsStart = Clock::now();
            try
            {
                SynthesizeRemote(http, L"http-blocked-tts", ttsCancel.get_token());
            }
            catch (const Cancelled&)
            {
                ttsCancelled = true;
            }
            Expect(ttsCancelled && Clock::now() - ttsStart < std::chrono::seconds(2),
                   "stopping TTS interrupts synthesis network reads");
            http.ApiMode = L"Responses";
            bool redacted = false;
            try
            {
                Answer(http, L"http-error", L"", {}, {}, {}, {});
            }
            catch (const Error& e)
            {
                redacted = e.text.find(L"fixture-key") == std::wstring::npos &&
                           e.text.find(L"401") != std::wstring::npos;
            }
            Expect(redacted, "HTTP service error body and key redacted");
            Reject(
                [&]
                {
                    Answer(http, L"http-truncated", L"", {}, {}, {}, {});
                },
                "real HTTP truncated stream rejected");
            Reject(
                [&]
                {
                    Answer(http, L"http-redirect", L"", {}, {}, {}, {});
                },
                "HTTP redirects never forward authorization");
            std::stop_source source;
            std::jthread stopper(
                [&]
                {
                    Sleep(200);
                    source.request_stop();
                });
            auto start = Clock::now();
            bool cancelled = false;
            try
            {
                Answer(http, L"http-blocked", L"", {}, {}, source.get_token(), {});
            }
            catch (const Cancelled&)
            {
                cancelled = true;
            }
            Expect(cancelled && Clock::now() - start < std::chrono::seconds(2),
                   "cancellation interrupts blocked WinHTTP reads");
        }
        if (argc > 1 && std::wstring(argv[1]) == L"--hardware")
        {
            CheckAudioDucking(std::filesystem::absolute(argv[0]), temp / L"ducking", Expect);
            Audio playback;
            playback.pcm.resize(16000);
            std::stop_source playbackStop;
            std::jthread playbackStopper(
                [&]
                {
                    Sleep(100);
                    playbackStop.request_stop();
                });
            auto playbackStart = Clock::now();
            bool stopped = false;
            try
            {
                PlayWav(playback.Wav(), playbackStop.get_token());
            }
            catch (const Cancelled&)
            {
                stopped = true;
            }
            Expect(stopped && Clock::now() - playbackStart < std::chrono::milliseconds(700),
                   "real WAV playback cancellation releases default speaker promptly");
            auto captures = CaptureScreens(640);
            Expect(!captures.empty() && captures[0].width <= 640 && captures[0].height <= 640 &&
                       !captures[0].jpeg.empty(),
                   "real monitor capture and WIC JPEG");
            DesktopAudio desktop;
            desktop.Start(1,
                          [](auto)
                          {
                          });
            Sleep(400);
            desktop.Snapshot();
            desktop.Stop();
            Expect(true, "WASAPI loopback start/snapshot/stop");
            std::atomic<bool> finish = true;
            auto mic = RecordMicrophone(s, {}, finish);
            Expect(mic.pcm.empty(), "microphone manual finish releases device");
            std::promise<std::wstring> initialized;
            auto initializedResult = initialized.get_future();
            std::atomic<bool> reported = false;
            auto report = [&](std::wstring error)
            {
                if (!reported.exchange(true))
                    initialized.set_value(std::move(error));
            };
            WakeWordListener wake;
            wake.Start(
                L"小弟小弟",
                L"zh-CN",
                []
                {
                },
                report,
                [&]
                {
                    report(L"");
                });
            Expect(initializedResult.wait_for(std::chrono::seconds(10)) ==
                       std::future_status::ready,
                   "local wake listener initializes or reports an actionable error promptly");
            auto wakeError = initializedResult.get();
            if (!wakeError.empty())
                throw Error(wakeError);
            Expect(true, "real SAPI microphone and Chinese custom wake grammar initialize");
            auto stopStart = Clock::now();
            wake.Stop();
            Expect(Clock::now() - stopStart < std::chrono::seconds(2),
                   "standby wake listener cancellation releases microphone promptly");
        }
        std::filesystem::remove_all(temp);
        std::cout << "Completed " << checks << " native checks. No live API calls.\n";
        return 0;
    }
    catch (...)
    {
        std::wcerr << L"FAIL: " << ErrorText() << L"\n";
        return 1;
    }
}
