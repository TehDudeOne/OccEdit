#define NOMINMAX
#include "quick_results.h"
#include <ole2.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace quick_results {
namespace {
template<class T> struct Release { void operator()(T* ptr) const { if (ptr) ptr->Release(); } };
template<class T> using Com = std::unique_ptr<T, Release<T>>;
constexpr size_t missing = std::wstring::npos;
constexpr uint8_t marker[] = {255, 254, 255};
constexpr uint8_t cfb[] = {0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1};
constexpr std::wstring_view titlePrefix = L"T\u00edtulo: ";

void require(bool condition) { if (!condition) throw std::runtime_error("QuickCMC format"); }
template<class T> T read(const Bytes& data, size_t pos) {
    require(pos <= data.size() && sizeof(T) <= data.size() - pos);
    T value{};
    std::memcpy(&value, data.data() + pos, sizeof value);
    return value;
}
template<class T> Bytes binary(T value) {
    Bytes result(sizeof(T));
    std::memcpy(result.data(), &value, sizeof value);
    return result;
}
Bytes tag(std::wstring_view text) {
    require(text.size() <= 254);
    Bytes bytes{255,254,255,static_cast<uint8_t>(text.size())};
    for (wchar_t c : text) {
        require(c >= 32);
        bytes.push_back(static_cast<uint8_t>(c));
        bytes.push_back(static_cast<uint8_t>(c >> 8));
    }
    return bytes;
}
bool matches(const Bytes& data, size_t pos, const Bytes& token) {
    return pos <= data.size() && token.size() <= data.size() - pos &&
           std::equal(token.begin(), token.end(), data.begin() + pos);
}
size_t find(const Bytes& data, const Bytes& token, size_t from, size_t to) {
    require(from <= to && to <= data.size());
    auto result = std::search(data.begin() + from, data.begin() + to, token.begin(), token.end());
    return result == data.begin() + to ? missing : static_cast<size_t>(result - data.begin());
}
size_t unique(const Bytes& data, const Bytes& token, size_t from, size_t to) {
    size_t pos = find(data, token, from, to);
    require(pos != missing && find(data, token, pos + token.size(), to) == missing);
    return pos;
}
std::wstring unpack(const Bytes& data, size_t& pos, size_t limit) {
    require(pos <= limit && limit <= data.size() && limit - pos >= 4);
    require(std::equal(std::begin(marker), std::end(marker), data.begin() + pos));
    size_t count = data[pos + 3];
    pos += 4;
    require(count < 255 && count <= (limit - pos) / 2);
    std::wstring result;
    for (size_t i = 0; i < count; ++i, pos += 2)
        result.push_back(static_cast<wchar_t>(read<uint16_t>(data, pos)));
    return result;
}
Span preceding(const Bytes& data, size_t end, std::wstring& value, uint32_t field) {
    for (size_t start = end - std::min<size_t>(end, 512); start + 4 <= end; ++start) {
        if (start < 8 || !matches(data, start, {255,254,255}) ||
            start + 4 + 2u * data[start + 3] != end ||
            read<uint32_t>(data,start-8) != 2 || read<uint32_t>(data,start-4) != field) continue;
        size_t cursor = start;
        value = unpack(data, cursor, end);
        return {start, end};
    }
    throw std::runtime_error("Missing state field");
}
std::array<std::wstring, 9> csv(std::wstring_view line) {
    std::array<std::wstring, 9> fields;
    size_t pos = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        auto& field = fields[i];
        if (pos < line.size() && line[pos] == L'"') {
            for (++pos;;) {
                require(pos < line.size());
                wchar_t c = line[pos++];
                if (c == L'"') {
                    if (pos == line.size() || line[pos] != L'"') break;
                    ++pos;
                }
                field.push_back(c);
            }
        } else {
            while (pos < line.size() && line[pos] != L';') field.push_back(line[pos++]);
        }
        if (i + 1 < fields.size()) require(pos < line.size() && line[pos++] == L';');
    }
    require(pos == line.size());
    return fields;
}
std::wstring quote(std::wstring_view text) {
    std::wstring result(1, L'"');
    for (wchar_t c : text) { if (c == L'"') result.push_back(c); result.push_back(c); }
    return result + L'"';
}
std::wstring normalizeReading(std::wstring value) {
    const auto isSpace = [](wchar_t c) { return std::iswspace(c) != 0 || c == L'\u00a0' || c == L'\u202f'; };
    auto first = std::find_if_not(value.begin(),value.end(),isSpace);
    if (first == value.end()) return {};
    auto last = std::find_if_not(value.rbegin(),value.rend(),isSpace).base();
    std::wstring normalized(first,last);
    std::replace(normalized.begin(),normalized.end(),L'\u2212',L'-');
    if (normalized.find(L'.') == missing && std::count(normalized.begin(),normalized.end(),L',') == 1)
        std::replace(normalized.begin(),normalized.end(),L',',L'.');
    return normalized;
}

double number(const std::wstring& value, double scale) {
    require(!value.empty() && value.size() <= 32);
    std::string ascii;
    for (wchar_t c : value) { require(c < 128); ascii.push_back(static_cast<char>(c)); }
    const char* start = ascii.data() + (ascii.front() == '+');
    double result{};
    auto parsed = std::from_chars(start, ascii.data() + ascii.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == ascii.data() + ascii.size() &&
            std::isfinite(result * scale));
    return result * scale;
}
bool sameReading(const Bytes& data, size_t pos, const std::wstring& field, double scale, double tolerance) {
    try {
        double a = read<double>(data,pos), b = number(field,scale);
        return std::isfinite(a) && std::abs(a-b) <= std::max(tolerance,1e-9*std::max(std::abs(a), std::abs(b)));
    } catch (const std::runtime_error&) { return false; }
}
struct Table {
    Com<ILockBytes> buffer;
    Com<IStorage> storage;
    Com<IStream> stream;
    Bytes data;

    Table(const Bytes& report, Span span, DWORD access) {
        ILockBytes* rawBuffer = nullptr;
        require(SUCCEEDED(CreateILockBytesOnHGlobal(nullptr, TRUE, &rawBuffer)));
        buffer.reset(rawBuffer);
        ULARGE_INTEGER offset{};
        const auto size = static_cast<ULONG>(span.last - span.first);
        ULONG written{};
        require(SUCCEEDED(buffer->WriteAt(offset, report.data() + span.first, size, &written)) && written == size);
        IStorage* rawStorage = nullptr;
        require(SUCCEEDED(StgOpenStorageOnILockBytes(buffer.get(), nullptr, access, nullptr, 0, &rawStorage)));
        storage.reset(rawStorage);
        IStream* rawStream = nullptr;
        require(SUCCEEDED(storage->OpenStream(L"Report", nullptr, access, 0, &rawStream)));
        stream.reset(rawStream);
        STATSTG stat{};
        require(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart <= size);
        data.resize(static_cast<size_t>(stat.cbSize.QuadPart));
        ULONG readCount{};
        require(SUCCEEDED(stream->Read(data.data(), static_cast<ULONG>(data.size()), &readCount)) && readCount == data.size());
    }
    Bytes save() {
        LARGE_INTEGER start{};
        ULARGE_INTEGER size{};
        size.QuadPart = data.size();
        ULONG written{};
        require(SUCCEEDED(stream->SetSize(size)) && SUCCEEDED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) &&
                SUCCEEDED(stream->Write(data.data(), static_cast<ULONG>(data.size()), &written)) && written == data.size() &&
                SUCCEEDED(stream->Commit(0)));
        stream.reset();
        require(SUCCEEDED(storage->Commit(0)));
        storage.reset();
        STATSTG stat{};
        require(SUCCEEDED(buffer->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart <= 16 * 1024 * 1024);
        Bytes file(static_cast<size_t>(stat.cbSize.QuadPart));
        ULARGE_INTEGER offset{};
        ULONG readCount{};
        require(SUCCEEDED(buffer->ReadAt(offset, file.data(), static_cast<ULONG>(file.size()), &readCount)) && readCount == file.size());
        Bytes result = binary(static_cast<uint32_t>(file.size()));
        result.insert(result.end(), file.begin(), file.end());
        return result;
    }
};

void replace(Bytes& data, Span span, const Bytes& value) {
    require(span.first <= span.last && span.last <= data.size());
    auto at = data.erase(data.begin() + span.first, data.begin() + span.last);
    data.insert(at, value.begin(), value.end());
}
struct Change { Span span; Bytes bytes; };
void patch(Bytes& data, std::vector<Change>& changes) {
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.span.first > b.span.first; });
    size_t limit = data.size();
    for (const auto& change : changes) {
        require(change.span.last <= limit);
        replace(data, change.span, change.bytes);
        limit = change.span.first;
    }
}
}

bool Parse(const Bytes& summary, const Bytes& states, const Bytes& report, Model& model) {
    try {
        Model next;
        const Bytes name = tag(L"QCMC_State.CSV");
        size_t pos = unique(summary,name,0,summary.size()) + name.size();
        size_t count = read<uint32_t>(summary,pos);
        require(count >= 2 && count <= 4096);
        pos += 4;
        require(unpack(summary,pos,summary.size()) ==
                L"TestReportID;TestReportOrder;StateNumber;Name;Comment;Assessment;TestDate;VDCIn;IDCIn");
        for (size_t i = 1; i < count; ++i) {
            Row row{};
            row.summary.first = pos;
            row.fields = csv(unpack(summary,pos,summary.size()));
            row.summary.last = pos;
            require(row.fields[0] == L"$$REPORT_ID$$" && row.fields[1] == L"$$REPORT_ORDER$$" &&
                    row.fields[2] == std::to_wstring(i));
            next.rows.push_back(std::move(row));
        }

        const auto stateName = tag(L"StateName");
        const auto commentName = tag(L"Comment");
        const std::array keys{tag(L"VDCIn"),tag(L"IDCIn")};
        pos = find(states,stateName,0,states.size());
        next.readings = true;
        for (size_t i = 0; pos != missing; ++i) {
            require(i <= next.rows.size());
            size_t begin = pos + stateName.size();
            size_t following = find(states,stateName,begin,states.size());
            size_t end = following == missing ? states.size() : following;
            std::wstring title, comment;
            Span titleSpan = preceding(states,pos,title,0);
            Span commentSpan = preceding(states,unique(states,commentName,begin,end),comment,10);
            size_t offsets[2]{};
            for (size_t k = 0; k < 2; ++k) {
                size_t key = unique(states,keys[k],begin,end);
                require(key >= begin + 24 && read<uint32_t>(states,key-24) == 4 &&
                        read<uint32_t>(states,key-20) == 2 && read<uint32_t>(states,key-16) == 7+k);
                offsets[k] = key - 12;
            }
            if (i < next.rows.size()) {
                auto& row = next.rows[i];
                require(title == row.fields[3] && comment == row.fields[4]);
                row.title = titleSpan; row.comment = commentSpan;
                row.voltage = offsets[0]; row.current = offsets[1];
                next.readings &= sameReading(states,offsets[0],row.fields[7],1,0.00005) &&
                                 sameReading(states,offsets[1],row.fields[8],1000,0.00000005);
            } else {
                const auto& last = next.rows.back();
                if (title == last.fields[3] && comment == last.fields[4] &&
                    read<double>(states,offsets[0]) == read<double>(states,last.voltage) &&
                    read<double>(states,offsets[1]) == read<double>(states,last.current)) {
                    next.hasCurrentState = true;
                    next.currentTitle = titleSpan; next.currentComment = commentSpan;
                    next.currentVoltage = offsets[0]; next.currentCurrent = offsets[1];
                }
            }
            pos = following;
        }
        require(next.rows.back().voltage != 0);
        const Bytes tableToken = tag(L"OMICRON.RTabCtrl");
        Bytes prefix = tag(titlePrefix);
        prefix.erase(prefix.begin(), prefix.begin() + 4);
        size_t index = 0;
        for (pos = 0; (pos = find(report,tableToken,pos,report.size())) != missing;) {
            pos += tableToken.size();
            size_t start = pos + 4;
            if (!matches(report,start,Bytes(std::begin(cfb),std::end(cfb)))) continue;
            size_t size = read<uint32_t>(report,pos);
            require(size && size % 512 == 0 && size <= report.size() - start && size <= 16 * 1024 * 1024);
            pos = start + size;
            if (find(report,prefix,start,pos) == missing) continue;
            Table table(report,{start,pos},STGM_READ | STGM_SHARE_EXCLUSIVE);
            const Bytes tagMarker(std::begin(marker),std::end(marker));
            for (size_t offset = 0; (offset = find(table.data,tagMarker,offset,table.data.size())) != missing; ++offset) {
                if (table.data.size() - offset < 4 || table.data[offset+3] == 255 ||
                    !matches(table.data,offset+4,prefix)) continue;
                size_t stop = offset;
                auto title = unpack(table.data,stop,table.data.size());
                require(index < next.rows.size() && title == std::wstring(titlePrefix) + next.rows[index].fields[3]);
                next.rows[index].table = {start-4,pos};
                next.rows[index++].tableTitle = {offset,stop};
            }
        }
        require(index == next.rows.size());
        model = std::move(next);
        return true;
    } catch (const std::runtime_error&) { return false; }
}

bool Apply(Bytes& summary, Bytes& states, Bytes& report, Model& model,
           size_t index, const Edit& edit, std::wstring& error) {
    if (index >= model.rows.size()) return false;
    const auto& row = model.rows[index];
    auto fields = row.fields;
    fields[3] = edit[0];
    fields[4] = edit[1];
    for (size_t i = 0; i < 2; ++i) {
        if (edit[2+i] != row.fields[7+i]) fields[7+i] = normalizeReading(edit[2+i]);
    }
    if (fields == row.fields) return true;
    error.clear();
    if (std::all_of(fields[3].begin(),fields[3].end(),[](wchar_t c) {
        return std::iswspace(c) != 0 || c == L'\u00a0' || c == L'\u202f';
    })) {
        error = L"Escribe un t\u00edtulo para el resultado.";
        return false;
    }
    for (size_t i : {3u,4u}) {
        if (std::any_of(fields[i].begin(),fields[i].end(),[](wchar_t c) { return c < 32; })) {
            error = i == 3 ? L"El t\u00edtulo contiene un salto de l\u00ednea o tabulaci\u00f3n."
                           : L"El comentario contiene un salto de l\u00ednea o tabulaci\u00f3n.";
            return false;
        }
    }
    std::array<double,2> values{};
    for (size_t i = 0; i < values.size(); ++i) {
        if (fields[7+i] == row.fields[7+i]) continue;
        if (!model.readings) {
            error = L"Las lecturas internas y el resumen no coinciden. Solo se puede editar el t\u00edtulo y el comentario.";
            return false;
        }
        try {
            values[i] = number(fields[7+i],i == 0 ? 1 : 1000);
        } catch (const std::runtime_error&) {
            error = i == 0 ? L"Entrada CC (V): escribe un n\u00famero, sin unidad ni separador de miles."
                           : L"Entrada CC (A): escribe un n\u00famero, sin unidad ni separador de miles.";
            return false;
        }
    }
    auto csvFields = fields;
    for (size_t i : {3u,4u,5u}) csvFields[i] = quote(csvFields[i]);
    std::wstring line = csvFields[0];
    for (size_t i = 1; i < csvFields.size(); ++i) line += L";" + csvFields[i];
    if (line.size() > 254) {
        error = L"El t\u00edtulo y el comentario comparten espacio con los dem\u00e1s campos del estado. Quita al menos "
                + std::to_wstring(line.size()-254) + L" caracteres del t\u00edtulo o del comentario.";
        return false;
    }
    const wchar_t* failure = L"No se pudo actualizar el estado interno de QuickCMC.";
    try {
        auto newSummary = summary, newStates = states, newReport = report;
        replace(newSummary,row.summary,tag(line));
        std::vector<Change> changes;
        auto strings = [&](Span title, Span comment) {
            if (fields[3] != row.fields[3]) changes.push_back({title,tag(edit[0])});
            if (fields[4] != row.fields[4]) changes.push_back({comment,tag(edit[1])});
        };
        auto readings = [&](size_t voltage, size_t current) {
            if (fields[7] != row.fields[7]) {

                changes.push_back({{voltage,voltage+8},binary(values[0])});
            }
            if (fields[8] != row.fields[8]) {

                changes.push_back({{current,current+8},binary(values[1])});
            }
        };
        strings(row.title,row.comment);
        readings(row.voltage,row.current);
        if (model.hasCurrentState && index+1 == model.rows.size()) {
            strings(model.currentTitle,model.currentComment);
            readings(model.currentVoltage,model.currentCurrent);
        }
        patch(newStates,changes);
        if (fields[3] != row.fields[3]) {
            failure = L"No se pudo actualizar el t\u00edtulo en el informe de QuickCMC.";
            Table table(report,{row.table.first+4,row.table.last},STGM_READWRITE | STGM_SHARE_EXCLUSIVE);
            replace(table.data,row.tableTitle,tag(std::wstring(titlePrefix) + edit[0]));
            replace(newReport,row.table,table.save());
        }
        failure = L"El resumen, los estados y el informe de QuickCMC no coinciden despu\u00e9s del cambio.";
        Model checked;
        require(Parse(newSummary,newStates,newReport,checked) && checked.rows[index].fields == fields);
        summary = std::move(newSummary); states = std::move(newStates); report = std::move(newReport);
        model = std::move(checked);
        return true;
    } catch (const std::runtime_error&) {
        error = failure;
        return false;
    }
}
}


