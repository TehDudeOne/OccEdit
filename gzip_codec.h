#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gzip_codec {
constexpr size_t kDefaultLimit = 64u * 1024u * 1024u;
bool Decompress(const std::vector<uint8_t>& input, std::vector<uint8_t>& output,
                size_t outputLimit = kDefaultLimit, std::wstring* error = nullptr);
std::vector<uint8_t> CompressStored(const std::vector<uint8_t>& input);
}
