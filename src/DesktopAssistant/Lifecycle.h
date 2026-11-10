#pragma once
#include "Core.h"

namespace da
{
// Requests a normal tray-menu exit only from the explicitly selected executable.
// This lets upgrades release files while preserving layout and cancelling work.
int ShutdownRunningApplication(const std::filesystem::path& executable);
} // namespace da
