#include "gzip_codec.h"
#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace gzip_codec {
namespace {
struct Failure { const wchar_t* message; };

class Bits {
public:
    Bits(const std::vector<uint8_t>& source, size_t byteOffset) : src(source), bitPos(byteOffset * 8) {}
    uint32_t Read(unsigned count) {
        if (count > 24 || bitPos + count > src.size() * 8) throw Failure{L"El flujo DEFLATE está truncado."};
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i, ++bitPos)
            value |= static_cast<uint32_t>((src[bitPos >> 3] >> (bitPos & 7)) & 1) << i;
        return value;
    }
    void Align() { bitPos = (bitPos + 7) & ~size_t(7); }
    size_t BytePosition() const { return bitPos >> 3; }
private:
    const std::vector<uint8_t>& src;
    size_t bitPos;
};

unsigned Reverse(unsigned code, unsigned bits) {
    unsigned result = 0;
    for (unsigned i = 0; i < bits; ++i) { result = (result << 1) | (code & 1); code >>= 1; }
    return result;
}

class Huffman {
public:
    explicit Huffman(const std::vector<unsigned>& lengths) {
        std::array<unsigned, 16> count{};
        for (unsigned length : lengths) {
            if (length > 15) throw Failure{L"Árbol Huffman DEFLATE inválido."};
            if (length) ++count[length];
            maxBits = std::max(maxBits, length);
        }
        if (!maxBits) throw Failure{L"Árbol Huffman DEFLATE vacío."};
        int left = 1;
        for (unsigned bits = 1; bits <= 15; ++bits) {
            left = (left << 1) - static_cast<int>(count[bits]);
            if (left < 0) throw Failure{L"Árbol Huffman DEFLATE sobrecargado."};
        }
        std::array<unsigned, 16> next{};
        unsigned code = 0;
        for (unsigned bits = 1; bits <= 15; ++bits) {
            code = (code + count[bits - 1]) << 1;
            next[bits] = code;
        }
        table.resize(maxBits + 1);
        for (unsigned bits = 1; bits <= maxBits; ++bits)
            table[bits].assign(size_t(1) << bits, -1);
        for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned bits = lengths[symbol];
            if (!bits) continue;
            const unsigned reversed = Reverse(next[bits]++, bits);
            table[bits][reversed] = static_cast<int>(symbol);
        }
    }
    unsigned Decode(Bits& bits) const {
        unsigned code = 0;
        for (unsigned length = 1; length <= maxBits; ++length) {
            code |= bits.Read(1) << (length - 1);
            const int symbol = table[length][code];
            if (symbol >= 0) return static_cast<unsigned>(symbol);
        }
        throw Failure{L"No se pudo decodificar el árbol Huffman."};
    }
private:
    unsigned maxBits = 0;
    std::vector<std::vector<int>> table;
};

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t Read32(const std::vector<uint8_t>& data, size_t at) {
    if (at + 4 > data.size()) throw Failure{L"El encabezado gzip está truncado."};
    return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) |
        (uint32_t(data[at + 2]) << 16) | (uint32_t(data[at + 3]) << 24);
}

void EnsureRoom(const std::vector<uint8_t>& output, size_t extra, size_t limit) {
    if (extra > limit || output.size() > limit - extra)
        throw Failure{L"El flujo gzip supera el límite de descompresión."};
}

void FixedTrees(Huffman& literals, Huffman& distances) {
    std::vector<unsigned> lit(288), dist(32, 5);
    for (unsigned i = 0; i <= 143; ++i) lit[i] = 8;
    for (unsigned i = 144; i <= 255; ++i) lit[i] = 9;
    for (unsigned i = 256; i <= 279; ++i) lit[i] = 7;
    for (unsigned i = 280; i <= 287; ++i) lit[i] = 8;
    literals = Huffman(lit);
    distances = Huffman(dist);
}

void DynamicTrees(Bits& bits, Huffman& literals, Huffman& distances) {
    const unsigned hlit = bits.Read(5) + 257;
    const unsigned hdist = bits.Read(5) + 1;
    const unsigned hclen = bits.Read(4) + 4;
    if (hlit > 286 || hdist > 32) throw Failure{L"Conteo de códigos DEFLATE inválido."};
    constexpr unsigned order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    std::vector<unsigned> codeLengths(19, 0);
    for (unsigned i = 0; i < hclen; ++i) codeLengths[order[i]] = bits.Read(3);
    Huffman codeTree(codeLengths);
    std::vector<unsigned> all;
    all.reserve(hlit + hdist);
    while (all.size() < hlit + hdist) {
        const unsigned symbol = codeTree.Decode(bits);
        if (symbol <= 15) all.push_back(symbol);
        else if (symbol == 16) {
            if (all.empty()) throw Failure{L"Repetición Huffman sin código previo."};
            const unsigned repeat = bits.Read(2) + 3;
            if (all.size() + repeat > hlit + hdist) throw Failure{L"Repetición Huffman fuera de rango."};
            all.insert(all.end(), repeat, all.back());
        } else if (symbol == 17 || symbol == 18) {
            const unsigned repeat = symbol == 17 ? bits.Read(3) + 3 : bits.Read(7) + 11;
            if (all.size() + repeat > hlit + hdist) throw Failure{L"Repetición Huffman fuera de rango."};
            all.insert(all.end(), repeat, 0);
        } else throw Failure{L"Símbolo de longitud Huffman inválido."};
    }
    std::vector<unsigned> lit(all.begin(), all.begin() + hlit);
    std::vector<unsigned> dist(all.begin() + hlit, all.end());
    if (lit.size() <= 256 || lit[256] == 0) throw Failure{L"Falta el fin de bloque DEFLATE."};
    literals = Huffman(lit);
    distances = Huffman(dist);
}

void InflateBlock(Bits& bits, const Huffman& literals, const Huffman& distances,
                  std::vector<uint8_t>& out, size_t limit) {
    constexpr unsigned lengthBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    constexpr unsigned lengthExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    constexpr unsigned distBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    constexpr unsigned distExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        const unsigned symbol = literals.Decode(bits);
        if (symbol < 256) {
            EnsureRoom(out, 1, limit); out.push_back(static_cast<uint8_t>(symbol));
        } else if (symbol == 256) return;
        else if (symbol <= 285) {
            const unsigned li = symbol - 257;
            const unsigned length = lengthBase[li] + bits.Read(lengthExtra[li]);
            const unsigned ds = distances.Decode(bits);
            if (ds >= 30) throw Failure{L"Código de distancia DEFLATE reservado."};
            const unsigned distance = distBase[ds] + bits.Read(distExtra[ds]);
            if (!distance || distance > out.size()) throw Failure{L"Distancia DEFLATE fuera del historial."};
            EnsureRoom(out, length, limit);
            for (unsigned i = 0; i < length; ++i) out.push_back(out[out.size() - distance]);
        } else throw Failure{L"Código literal DEFLATE reservado."};
    }
}

void InflateMember(const std::vector<uint8_t>& input, size_t& cursor,
                   std::vector<uint8_t>& output, size_t limit) {
    const size_t memberStart = output.size();
    if (cursor + 10 > input.size() || input[cursor] != 0x1f || input[cursor + 1] != 0x8b || input[cursor + 2] != 8)
        throw Failure{L"El flujo no es un miembro gzip válido."};
    const uint8_t flags = input[cursor + 3];
    if (flags & 0xe0) throw Failure{L"Banderas gzip reservadas."};
    cursor += 10;
    if (flags & 4) {
        const unsigned extra = input.at(cursor) | (unsigned(input.at(cursor + 1)) << 8);
        cursor += 2 + extra;
        if (cursor > input.size()) throw Failure{L"Campo extra gzip truncado."};
    }
    auto skipString = [&]() {
        while (cursor < input.size() && input[cursor++] != 0) {}
        if (!cursor || input[cursor - 1] != 0) throw Failure{L"Campo de texto gzip truncado."};
    };
    if (flags & 8) skipString();
    if (flags & 16) skipString();
    if (flags & 2) { if (cursor + 2 > input.size()) throw Failure{L"CRC del encabezado gzip truncado."}; cursor += 2; }

    Bits bits(input, cursor);
    bool finalBlock = false;
    while (!finalBlock) {
        finalBlock = bits.Read(1) != 0;
        const unsigned kind = bits.Read(2);
        if (kind == 0) {
            bits.Align();
            const unsigned len = bits.Read(16), inverse = bits.Read(16);
            if ((len ^ 0xffffu) != inverse) throw Failure{L"Longitud de bloque gzip inválida."};
            EnsureRoom(output, len, limit);
            for (unsigned i = 0; i < len; ++i) output.push_back(static_cast<uint8_t>(bits.Read(8)));
        } else if (kind == 1 || kind == 2) {
            Huffman literals(std::vector<unsigned>{1}), distances(std::vector<unsigned>{1});
            if (kind == 1) FixedTrees(literals, distances);
            else DynamicTrees(bits, literals, distances);
            InflateBlock(bits, literals, distances, output, limit);
        } else throw Failure{L"Tipo de bloque DEFLATE reservado."};
    }
    bits.Align();
    cursor = bits.BytePosition();
    if (cursor + 8 > input.size()) throw Failure{L"Pie gzip truncado."};
    if (Read32(input, cursor) != Crc32(output.data() + memberStart, output.size() - memberStart))
        throw Failure{L"La suma de verificación gzip no coincide."};
    if (Read32(input, cursor + 4) != static_cast<uint32_t>(output.size() - memberStart))
        throw Failure{L"El tamaño gzip no coincide."};
    cursor += 8;
}
}

bool Decompress(const std::vector<uint8_t>& input, std::vector<uint8_t>& output,
                size_t outputLimit, std::wstring* error) {
    output.clear();
    try {
        size_t cursor = 0;
        while (cursor < input.size()) InflateMember(input, cursor, output, outputLimit);
        if (input.empty()) throw Failure{L"El flujo gzip está vacío."};
        return true;
    } catch (const Failure& failure) {
        output.clear();
        if (error) *error = failure.message;
        return false;
    } catch (...) {
        output.clear();
        if (error) *error = L"No se pudo leer el flujo gzip.";
        return false;
    }
}

std::vector<uint8_t> CompressStored(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out = {0x1f,0x8b,0x08,0x00,0,0,0,0,0,0xff};
    size_t offset = 0;
    do {
        const size_t count = std::min<size_t>(65535, input.size() - offset);
        const bool final = offset + count == input.size();
        out.push_back(final ? 1 : 0); // BFINAL and BTYPE=00, followed by byte alignment.
        const uint16_t len = static_cast<uint16_t>(count);
        const uint16_t nlen = static_cast<uint16_t>(~len);
        out.push_back(static_cast<uint8_t>(len)); out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(nlen)); out.push_back(static_cast<uint8_t>(nlen >> 8));
        out.insert(out.end(), input.begin() + offset, input.begin() + offset + count);
        offset += count;
    } while (offset < input.size());
    const uint32_t crc = Crc32(input.data(), input.size());
    const uint32_t size = static_cast<uint32_t>(input.size());
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(crc >> (8 * i)));
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(size >> (8 * i)));
    return out;
}
}
