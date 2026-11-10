#include "DuckingChecks.h"
#include <audioclient.h>
#include <mmdeviceapi.h>

namespace
{
using namespace da;

struct SilentSession
{
    ComPtr<IAudioClient> client;
    ComPtr<ISimpleAudioVolume> volume;

    SilentSession()
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        Check(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
        Check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device));
        Check(device->Activate(__uuidof(IAudioClient),
                               CLSCTX_ALL,
                               nullptr,
                               reinterpret_cast<void**>(client.GetAddressOf())));
        WAVEFORMATEX* format{};
        Check(client->GetMixFormat(&format));
        GUID session{};
        Check(CoCreateGuid(&session));
        auto result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, &session);
        CoTaskMemFree(format);
        Check(result);
        Check(client->GetService(IID_PPV_ARGS(&volume)));
        Check(volume->SetMasterVolume(.65f, nullptr));
        Check(client->Start());
    }

    ~SilentSession()
    {
        if (client)
            client->Stop();
    }
};

struct Fixture
{
    std::filesystem::path directory;
    PROCESS_INFORMATION process{};

    Fixture(const std::filesystem::path& executable, std::filesystem::path path)
        : directory(std::move(path))
    {
        std::filesystem::create_directories(directory);
        auto command =
            L"\"" + executable.wstring() + L"\" --ducking-session \"" + directory.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        if (!CreateProcessW(executable.c_str(),
                            command.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            CREATE_NO_WINDOW,
                            nullptr,
                            nullptr,
                            &startup,
                            &process))
            throw Error(L"无法启动音量测试会话。");
    }

    ~Fixture()
    {
        try
        {
            AtomicWrite(directory / L"stop", "stop");
        }
        catch (...)
        {
        }
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }

    bool WaitVolume(float expected)
    {
        auto deadline = Seconds() + 3;
        while (Seconds() < deadline)
        {
            try
            {
                auto value = std::stof(ReadFile(directory / L"volume"));
                if (std::abs(value - expected) < .001f)
                    return true;
            }
            catch (...)
            {
            }
            Sleep(20);
        }
        return false;
    }
};
} // namespace

int RunDuckingFixture(const std::filesystem::path& directory)
{
    SilentSession session;
    auto deadline = Seconds() + 20;
    while (!std::filesystem::exists(directory / L"stop") && Seconds() < deadline)
    {
        if (std::filesystem::exists(directory / L"manual"))
        {
            session.volume->SetMasterVolume(.42f, nullptr);
            std::filesystem::remove(directory / L"manual");
        }
        float value{};
        da::Check(session.volume->GetMasterVolume(&value));
        da::AtomicWrite(directory / L"volume", std::to_string(value));
        Sleep(20);
    }
    return 0;
}

void CheckAudioDucking(const std::filesystem::path& executable,
                       const std::filesystem::path& directory,
                       std::function<void(bool, const char*)> expect)
{
    Fixture fixture(executable, directory / L"first");
    expect(fixture.WaitVolume(.65f), "separate-process audio session starts at original volume");
    SilentSession own;
    {
        AudioDucking disabled(false);
        expect(fixture.WaitVolume(.65f),
               "disabled automatic ducking leaves other applications unchanged");
    }
    std::stop_source cancel;
    {
        AudioDucking ducking(true, cancel.get_token());
        expect(fixture.WaitVolume(.13f), "voice recognition ducks another process to 20 percent");
        float ownVolume{};
        Check(own.volume->GetMasterVolume(&ownVolume));
        expect(std::abs(ownVolume - .65f) < .001f,
               "assistant's own audio session is excluded from ducking");
        Fixture later(executable, directory / L"later");
        expect(later.WaitVolume(.13f),
               "audio sessions opened during recognition are also attenuated");
        cancel.request_stop();
        expect(fixture.WaitVolume(.65f) && later.WaitVolume(.65f),
               "cancelling recognition restores all original application volumes");
    }
    {
        AudioDucking ducking(true);
        expect(fixture.WaitVolume(.13f), "next recognition attenuates the restored audio session");
        AtomicWrite(fixture.directory / L"manual", "change");
        expect(fixture.WaitVolume(.42f), "manual volume adjustment is observed during recognition");
    }
    expect(fixture.WaitVolume(.42f),
           "recognition cleanup preserves manual application volume changes");
}
