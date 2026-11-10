#pragma once
#include "Core.h"

namespace da
{
std::vector<Screen> CaptureScreens(int maxDimension);
void SaveWindowPreview(HWND window, const std::filesystem::path& path);

// Scoped session attenuation. Its worker restores volumes before destruction returns.
class AudioDucking
{
    std::jthread worker_;

public:
    explicit AudioDucking(bool enabled, std::stop_token token = {});
    ~AudioDucking();
};

// The audio engine owns its COM objects on the worker thread. Stop joins before
// any buffer or UI callback can be destroyed.
class DesktopAudio
{
    Timeline timeline_;
    std::jthread worker_;

public:
    void Start(int seconds, std::function<void(std::wstring)> warning);
    void Stop();

    Audio Snapshot() const
    {
        return timeline_.Snapshot(Seconds());
    }

    ~DesktopAudio()
    {
        Stop();
    }
};

Audio RecordMicrophone(const Settings& settings,
                       std::stop_token token,
                       const std::atomic<bool>& finish);
std::vector<std::pair<std::wstring, std::wstring>> SpeechLanguages();
std::wstring TranscribeLocal(const Audio& audio, std::wstring_view language, std::stop_token token);

// Local SAPI command grammar uses the recognizer's VAD and never uploads standby audio.
class WakeWordListener
{
    std::jthread worker_;

public:
    void Start(std::wstring phrase,
               std::wstring language,
               std::function<void()> recognized,
               std::function<void(std::wstring)> warning,
               std::function<void()> ready = {});
    void Stop();

    ~WakeWordListener()
    {
        Stop();
    }
};
} // namespace da
