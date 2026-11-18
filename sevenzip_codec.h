#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sevenzip_codec {
constexpr size_t kMaxDecodedSize = 64u * 1024u * 1024u;

struct FileMetadata {
    bool hasLastWriteTime = false;
    uint64_t lastWriteTime = 0;
    bool hasAttributes = false;
    uint32_t attributes = 0;
};

bool ExtractSingleLzma2(const std::vector<uint8_t>& archive, std::string& memberName,
                        std::vector<uint8_t>& contents, FileMetadata* metadata = nullptr,
                        std::wstring* error = nullptr);
bool BuildSingleLzma2(const std::string& memberName, const std::vector<uint8_t>& contents,
                      std::vector<uint8_t>& archive, const FileMetadata* metadata = nullptr,
                      std::wstring* error = nullptr);
}
