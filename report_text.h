#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

bool FindEditableText(const std::vector<uint8_t>& body, size_t& start, size_t& end, std::wstring& error);
bool BuildEditedBody(const std::vector<uint8_t>& original, HWND richEdit,
                     std::vector<uint8_t>& result, std::wstring& error);
