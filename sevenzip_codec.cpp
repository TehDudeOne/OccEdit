#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
extern "C" {
#include "lzma_sdk/7z.h"
#include "lzma_sdk/7zAlloc.h"
#include "lzma_sdk/7zCrc.h"
}
#include "sevenzip_codec.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr uint8_t kSignature[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
constexpr size_t kMaxArchiveSize = 128u * 1024u * 1024u;
const ISzAlloc kAlloc = {SzAlloc, SzFree};
const ISzAlloc kAllocTemp = {SzAllocTemp, SzFreeTemp};

struct MemoryStream {
    ILookInStream iface{};
    const Byte* data = nullptr;
    size_t size = 0;
    size_t position = 0;
};

SRes memoryLook(ILookInStreamPtr iface, const void** buffer, size_t* size) {
    auto* stream = reinterpret_cast<MemoryStream*>(const_cast<ILookInStream*>(iface));
    const size_t available = stream->size - stream->position;
    *size = std::min(*size, available);
    *buffer = stream->data + stream->position;
    return SZ_OK;
}
SRes memorySkip(ILookInStreamPtr iface, size_t amount) {
    auto* stream = reinterpret_cast<MemoryStream*>(const_cast<ILookInStream*>(iface));
    if (amount > stream->size - stream->position) return SZ_ERROR_INPUT_EOF;
    stream->position += amount;
    return SZ_OK;
}
SRes memoryRead(ILookInStreamPtr iface, void* buffer, size_t* size) {
    auto* stream = reinterpret_cast<MemoryStream*>(const_cast<ILookInStream*>(iface));
    const size_t amount = std::min(*size, stream->size - stream->position);
    if (amount) std::memcpy(buffer, stream->data + stream->position, amount);
    stream->position += amount;
    *size = amount;
    return SZ_OK;
}
SRes memorySeek(ILookInStreamPtr iface, Int64* position, ESzSeek origin) {
    auto* stream = reinterpret_cast<MemoryStream*>(const_cast<ILookInStream*>(iface));
    Int64 base = 0;
    if (origin == SZ_SEEK_CUR) base = static_cast<Int64>(stream->position);
    else if (origin == SZ_SEEK_END) base = static_cast<Int64>(stream->size);
    else if (origin != SZ_SEEK_SET) return SZ_ERROR_PARAM;
    if ((*position < 0 && base < -*position) || (*position > 0 && base > INT64_MAX - *position))
        return SZ_ERROR_PARAM;
    const Int64 target = base + *position;
    if (target < 0 || static_cast<UInt64>(target) > stream->size) return SZ_ERROR_INPUT_EOF;
    stream->position = static_cast<size_t>(target);
    *position = target;
    return SZ_OK;
}

void appendByte(std::vector<uint8_t>& out, uint8_t value) { out.push_back(value); }
void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned extra = 0; extra <= 7; ++extra) {
        const unsigned payloadBits = 7 + extra * 8;
        if (payloadBits < 64 && value >= (uint64_t{1} << payloadBits)) continue;
        if (extra == 0) { appendByte(out, static_cast<uint8_t>(value)); return; }
        const uint8_t prefix = static_cast<uint8_t>(0xFFu << (8 - extra));
        const uint8_t first = static_cast<uint8_t>(prefix | (value >> (extra * 8)));
        appendByte(out, first);
        for (unsigned i = 0; i < extra; ++i) appendByte(out, static_cast<uint8_t>(value >> (i * 8)));
        return;
    }
    appendByte(out, 0xFF);
    for (unsigned i = 0; i < 8; ++i) appendByte(out, static_cast<uint8_t>(value >> (i * 8)));
}

bool safeMemberName(const std::string& name) {
    return !name.empty() && name.size() <= 240 && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string::npos;
}

std::vector<uint8_t> makeRawLzma2(const std::vector<uint8_t>& contents) {
    std::vector<uint8_t> out;
    out.reserve(contents.size() + contents.size() / 65536 * 3 + 4);
    size_t offset = 0;
    bool first = true;
    while (offset < contents.size()) {
        const size_t count = std::min<size_t>(65536, contents.size() - offset);
        appendByte(out, first ? 0x01 : 0x02);
        const uint16_t encodedSize = static_cast<uint16_t>(count - 1);
        appendByte(out, static_cast<uint8_t>(encodedSize >> 8));
        appendByte(out, static_cast<uint8_t>(encodedSize));
        out.insert(out.end(), contents.begin() + offset, contents.begin() + offset + count);
        offset += count;
        first = false;
    }
    appendByte(out, 0x00);
    return out;
}

std::vector<uint8_t> makeHeader(const std::string& name, size_t unpackSize,
                                size_t packSize, uint32_t dataCrc,
                                const sevenzip_codec::FileMetadata* metadata) {
    std::vector<uint8_t> h;
    appendByte(h, 0x01); // Header
    appendByte(h, 0x04); // MainStreamsInfo
    appendByte(h, 0x06); appendU64(h, 0); appendU64(h, 1);
    appendByte(h, 0x09); appendU64(h, packSize); appendByte(h, 0x00);
    appendByte(h, 0x07); appendByte(h, 0x0B); appendU64(h, 1); appendByte(h, 0x00);
    appendU64(h, 1); appendByte(h, 0x21); appendByte(h, 0x21);
    appendU64(h, 1); appendByte(h, 0x0F);
    appendByte(h, 0x0C); appendU64(h, unpackSize);
    appendByte(h, 0x0A); appendByte(h, 0x01); appendU32(h, dataCrc);
    appendByte(h, 0x00); // End UnpackInfo.
    appendByte(h, 0x08); appendByte(h, 0x00); // One substream per folder, default size and folder CRC.
    appendByte(h, 0x00); // End MainStreamsInfo.
    appendByte(h, 0x05); appendU64(h, 1);
    std::vector<uint8_t> nameData;
    nameData.reserve(name.size() * 2 + 3);
    nameData.push_back(0x00);
    for (unsigned char ch : name) { nameData.push_back(ch); nameData.push_back(0); }
    nameData.push_back(0); nameData.push_back(0);
    appendByte(h, 0x11); appendU64(h, nameData.size());
    h.insert(h.end(), nameData.begin(), nameData.end());
    if (metadata && metadata->hasLastWriteTime) {
        appendByte(h, 0x14); appendU64(h, 10);
        appendByte(h, 0x01); appendByte(h, 0x00);
        for (unsigned i = 0; i < 8; ++i)
            appendByte(h, static_cast<uint8_t>(metadata->lastWriteTime >> (8 * i)));
    }
    if (metadata && metadata->hasAttributes) {
        appendByte(h, 0x15); appendU64(h, 6);
        appendByte(h, 0x01); appendByte(h, 0x00);
        appendU32(h, metadata->attributes);
    }
    appendByte(h, 0x00); appendByte(h, 0x00);
    return h;
}

void setError(std::wstring* error, const wchar_t* message) { if (error) *error = message; }
}

namespace sevenzip_codec {
bool ExtractSingleLzma2(const std::vector<uint8_t>& archive, std::string& memberName,
                        std::vector<uint8_t>& contents, FileMetadata* metadata,
                        std::wstring* error) {
    contents.clear(); memberName.clear();
    if (metadata) *metadata = {};
    if (archive.size() < k7zStartHeaderSize || archive.size() > kMaxArchiveSize ||
        std::memcmp(archive.data(), kSignature, sizeof(kSignature)) != 0) {
        setError(error, L"El flujo QuickCMC no es un archivo 7z válido o supera el límite permitido."); return false;
    }
    CrcGenerateTable();
    MemoryStream stream;
    stream.iface.Look = memoryLook; stream.iface.Skip = memorySkip;
    stream.iface.Read = memoryRead; stream.iface.Seek = memorySeek;
    stream.data = archive.data(); stream.size = archive.size();
    CSzArEx db; SzArEx_Init(&db);
    SRes result = SzArEx_Open(&db, &stream.iface, &kAlloc, &kAllocTemp);
    if (result != SZ_OK) {
        SzArEx_Free(&db, &kAlloc);
        setError(error, L"No se pudo validar ni descomprimir el contenedor 7z de QuickCMC."); return false;
    }
    bool ok = false;
    Byte* output = nullptr; size_t outputSize = 0; UInt32 blockIndex = 0xFFFFFFFF;
    size_t offset = 0, processed = 0;
    do {
        if (db.NumFiles != 1 || db.db.NumFolders != 1 || SzArEx_IsDir(&db, 0)) break;
        CSzFolder folder{};
        const size_t start = db.db.FoCodersOffsets[0];
        const size_t end = db.db.FoCodersOffsets[1];
        if (end <= start) break;
        CSzData coderData{db.db.CodersData + start, end - start};
        if (SzGetNextFolderItem(&folder, &coderData) != SZ_OK || folder.NumCoders != 1 ||
            folder.NumPackStreams != 1 || folder.Coders[0].MethodID != 0x21 ||
            folder.Coders[0].PropsSize != 1 || folder.Coders[0].NumStreams != 1) break;
        const Byte dictionaryProp = db.db.CodersData[start + folder.Coders[0].PropsOffset];
        if (dictionaryProp > 40 || SzArEx_GetFileSize(&db, 0) > kMaxDecodedSize) break;
        const size_t nameChars = SzArEx_GetFileNameUtf16(&db, 0, nullptr);
        if (nameChars < 2 || nameChars > 241) break;
        std::vector<UInt16> wide(nameChars);
        if (SzArEx_GetFileNameUtf16(&db, 0, wide.data()) != nameChars) break;
        std::string narrow;
        narrow.reserve(nameChars);
        for (size_t i = 0; i + 1 < nameChars; ++i) {
            if (wide[i] > 0x7F) { narrow.clear(); break; }
            narrow.push_back(static_cast<char>(wide[i]));
        }
        if (!safeMemberName(narrow)) break;
        result = SzArEx_Extract(&db, &stream.iface, 0, &blockIndex, &output, &outputSize,
                                &offset, &processed, &kAlloc, &kAllocTemp);
        if (result != SZ_OK || processed > kMaxDecodedSize || offset > outputSize ||
            processed > outputSize - offset) break;
        if (metadata) {
            if (SzBitWithVals_Check(&db.MTime, 0)) {
                metadata->hasLastWriteTime = true;
                metadata->lastWriteTime = (static_cast<uint64_t>(db.MTime.Vals[0].High) << 32) |
                                           db.MTime.Vals[0].Low;
            }
            if (SzBitWithVals_Check(&db.Attribs, 0)) {
                metadata->hasAttributes = true;
                metadata->attributes = db.Attribs.Vals[0];
            }
        }
        contents.assign(output + offset, output + offset + processed);
        memberName = std::move(narrow);
        ok = true;
    } while (false);
    ISzAlloc_Free(&kAlloc, output);
    SzArEx_Free(&db, &kAlloc);
    if (!ok) setError(error, L"El 7z de QuickCMC no es un único flujo LZMA2 simple compatible o está dañado.");
    return ok;
}

bool BuildSingleLzma2(const std::string& memberName, const std::vector<uint8_t>& contents,
                      std::vector<uint8_t>& archive, const FileMetadata* metadata,
                      std::wstring* error) {
    archive.clear();
    if (!safeMemberName(memberName) || contents.empty() || contents.size() > kMaxDecodedSize) {
        setError(error, L"El nombre o tamaño del contenido QuickCMC no es válido."); return false;
    }
    CrcGenerateTable();
    try {
        const std::vector<uint8_t> packed = makeRawLzma2(contents);
        const std::vector<uint8_t> header = makeHeader(memberName, contents.size(), packed.size(),
                                                       CrcCalc(contents.data(), contents.size()), metadata);
        archive.reserve(k7zStartHeaderSize + packed.size() + header.size());
        archive.insert(archive.end(), std::begin(kSignature), std::end(kSignature));
        archive.push_back(0); archive.push_back(4);
        appendU32(archive, 0);
        const uint64_t nextHeaderOffset = packed.size();
        for (unsigned i = 0; i < 8; ++i) archive.push_back(static_cast<uint8_t>(nextHeaderOffset >> (8 * i)));
        for (unsigned i = 0; i < 8; ++i) archive.push_back(static_cast<uint8_t>(header.size() >> (8 * i)));
        appendU32(archive, CrcCalc(header.data(), header.size()));
        const UInt32 startCrc = CrcCalc(archive.data() + 12, 20);
        for (unsigned i = 0; i < 4; ++i) archive[8 + i] = static_cast<uint8_t>(startCrc >> (8 * i));
        archive.insert(archive.end(), packed.begin(), packed.end());
        archive.insert(archive.end(), header.begin(), header.end());
    } catch (...) {
        archive.clear(); setError(error, L"No hay memoria suficiente para reconstruir el flujo 7z de QuickCMC."); return false;
    }
    return true;
}
}
