#pragma once

#include <string>

namespace diagnostics {
// A one-line-per-event UTF-8 diagnostic log beside FrameGeneration.exe.
void start();
void write(const std::string &message);
void fileVersion(const wchar_t *filename);
}
