#pragma once

#include <string>
#include <windows.h>

HWND FindTargetWindow(const std::wstring& targetTitle, bool matchExact);
