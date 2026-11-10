#pragma once
#include "Capture.h"

int RunDuckingFixture(const std::filesystem::path& directory);
void CheckAudioDucking(const std::filesystem::path& executable,
                       const std::filesystem::path& directory,
                       std::function<void(bool, const char*)> expect);
