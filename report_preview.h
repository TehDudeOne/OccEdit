#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <ole2.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>
#include "phasor_model.h"

namespace report_preview {
using Bytes = std::vector<uint8_t>;
struct Cell {
    std::wstring text;
    size_t first{}, last{};
    bool editable = false;
    bool empty() const { return text.empty(); }
};
using Rows = std::vector<std::vector<Cell>>;
struct Target {
    std::wstring module;
    size_t table, row, column;
    std::wstring value;
    long first = -1, last = -1;
};
using View = std::string_view;

inline uint32_t number(View data, size_t pos, size_t size) {
    if (pos > data.size() || size > data.size()-pos) return 0;
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i)
        value |= static_cast<uint32_t>(static_cast<unsigned char>(data[pos+i])) << (i*8);
    return value;
}
inline View bytes(const Bytes& data) {
    if (data.empty()) return {};
    return {reinterpret_cast<const char*>(data.data()),data.size()};
}
inline Cell cellText(View data, size_t offset) {
    Cell result;
    size_t blocks = 0;
    for (size_t i = 0; i+14 <= data.size(); ++i) {
        if (number(data,i,2)!=0x8008 || number(data,i+2,4)-1>=32 ||
            number(data,i+6,4)-1>=32 || number(data,i+10,3)!=0xfffeff) continue;
        size_t first = i+14, count = number(data,i+13,1);
        if (count == 255) {
            if (first+2 > data.size()) continue;
            count = number(data,first,2);
            first += 2;
        }
        if (count > (data.size()-first)/2) continue;
        std::wstring line;
        for (size_t n = 0; n < count; ++n)
            line.push_back(static_cast<wchar_t>(number(data,first+n*2,2)));
        if (std::any_of(line.begin(),line.end(),[](wchar_t c) { return c < 32 && c != 9 && c != 10 && c != 13; })) continue;
        if (!result.empty() && !line.empty()) result.text += L'\n';
        result.text += line;
        result.first = offset+i+10;
        result.last = offset+first+count*2;
        result.editable = ++blocks == 1;
        i = first+count*2-1;
    }
    return result;
}
inline Rows readRows(View data) {
    constexpr View frame("\xff\xff\x01\x00\x0b\x00" "COMRTabCtrl",17);
    constexpr View cell("\x04\x80\x04\x80",4);
    const auto rows = number(data,frame.size(),4), columns = number(data,frame.size()+4,4);
    size_t stop = data.find("CTableCol");
    if (!data.starts_with(frame) || rows < 1 || rows > 400 || columns < 1 || columns > 64 ||
        number(data,frame.size()+8,2)!=rows || stop == View::npos) return {};
    data = data.substr(0,stop);
    std::vector<size_t> starts;
    for (size_t i = 27; i+6 <= data.size(); ++i) {
        if (number(data,i,2)==0x8002 && number(data,i+2,2)==columns &&
            (number(data,i+4,2)==0x8004 || number(data,i+4,2)==0xffff))
            starts.push_back(i);
    }
    if (starts.size()!=rows) return {};
    starts.push_back(data.size());
    Rows result;
    for (size_t row = 0; row < rows; ++row) {
        auto raw = data.substr(starts[row]+4,starts[row+1]-starts[row]-4);
        std::vector<size_t> cells;
        if (row==0) cells.push_back(0);
        for (size_t pos=0; (pos=raw.find(cell,pos))!=View::npos; pos+=cell.size()) cells.push_back(pos);
        if (cells.size()!=columns) return {};
        cells.push_back(raw.size());
        auto& values = result.emplace_back();
        for (size_t c=0; c<columns; ++c)
            values.push_back(cellText(raw.substr(cells[c],cells[c+1]-cells[c]),starts[row]+4+cells[c]));
    }
    return result;
}

inline Rows readTable(View file, const Target* edit = nullptr,
                      const std::wstring* value = nullptr, Bytes* updated = nullptr) {
    const auto release = [](auto* ptr) { if (ptr) ptr->Release(); };
    ILockBytes* rawBuffer = nullptr;
    if (FAILED(CreateILockBytesOnHGlobal(nullptr,TRUE,&rawBuffer))) return {};
    std::unique_ptr<ILockBytes,decltype(release)> buffer(rawBuffer,release);
    ULARGE_INTEGER zero{};
    ULONG count{};
    if (FAILED(buffer->WriteAt(zero,file.data(),static_cast<ULONG>(file.size()),&count)) || count != file.size()) return {};
    const DWORD mode = (edit ? STGM_READWRITE : STGM_READ) | STGM_SHARE_EXCLUSIVE;
    IStorage* rawStorage = nullptr;
    if (FAILED(StgOpenStorageOnILockBytes(buffer.get(),nullptr,mode,nullptr,0,&rawStorage))) return {};
    std::unique_ptr<IStorage,decltype(release)> storage(rawStorage,release);
    IStream* rawStream = nullptr;
    if (FAILED(storage->OpenStream(L"Report",nullptr,mode,0,&rawStream))) return {};
    std::unique_ptr<IStream,decltype(release)> stream(rawStream,release);
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat,STATFLAG_NONAME)) || stat.cbSize.QuadPart > file.size()) return {};
    std::string data(static_cast<size_t>(stat.cbSize.QuadPart),'\0');
    if (FAILED(stream->Read(data.data(),static_cast<ULONG>(data.size()),&count)) || count != data.size()) return {};
    auto rows = readRows(data);
    if (!edit) return rows;
    if (edit->row >= rows.size() || edit->column >= rows[edit->row].size()) return {};
    const auto& cell = rows[edit->row][edit->column];
    if (!cell.editable || cell.text != edit->value || value->size() > 254) return {};
    std::string record("\xff\xfe\xff",3);
    record.push_back(static_cast<char>(value->size()));
    for (wchar_t ch : *value) {
        if (ch < 32 || ch == 0xe001 || ch == 0xe002) return {};
        record.push_back(static_cast<char>(ch & 255));
        record.push_back(static_cast<char>(ch >> 8));
    }
    data.replace(cell.first,cell.last-cell.first,record);
    rows = readRows(data);
    if (edit->row >= rows.size() || edit->column >= rows[edit->row].size() ||
        !rows[edit->row][edit->column].editable || rows[edit->row][edit->column].text != *value) return {};
    ULARGE_INTEGER length{};
    length.QuadPart = data.size();
    LARGE_INTEGER start{};
    if (FAILED(stream->SetSize(length)) || FAILED(stream->Seek(start,STREAM_SEEK_SET,nullptr)) ||
        FAILED(stream->Write(data.data(),static_cast<ULONG>(data.size()),&count)) || count != data.size() ||
        FAILED(stream->Commit(0))) return {};
    stream.reset();
    if (FAILED(storage->Commit(0))) return {};
    storage.reset();
    if (FAILED(buffer->Stat(&stat,STATFLAG_NONAME)) || stat.cbSize.QuadPart > 16*1024*1024) return {};
    updated->resize(static_cast<size_t>(stat.cbSize.QuadPart));
    if (FAILED(buffer->ReadAt(zero,updated->data(),static_cast<ULONG>(updated->size()),&count)) ||
        count != updated->size()) return {};
    return rows;
}

inline std::string escape(std::wstring_view text) {
    std::string result;
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') result += "\\line ";
        else if (c == L'\t') result += " ";
        else if (c >= 32 && c < 127) {
            if (c == L'{' || c == L'}' || c == L'\\') result += '\\';
            result += static_cast<char>(c);
        } else if (c >= 32) result += "\\u" + std::to_string(static_cast<int16_t>(c)) + "?";
    }
    return result;
}

inline std::string vectorPicture(const Rows& rows) {
    const auto values = phasor::extract(rows);
    if (values.empty()) return {};
    double maxV=0,maxA=0;
    for (const auto& v : values) (v.unit==L'V' ? maxV : maxA) = std::max(v.unit==L'V' ? maxV : maxA,std::abs(v.amplitude));
    using namespace Gdiplus;
    Bitmap bitmap(190,172,PixelFormat32bppARGB); Graphics graphics(&bitmap); graphics.SetSmoothingMode(SmoothingModeAntiAlias); graphics.Clear(Color::White);
    Pen grid(Color(255,190,198,210),1.0f); graphics.DrawEllipse(&grid,36,17,118,118); graphics.DrawLine(&grid,36,76,154,76); graphics.DrawLine(&grid,95,17,95,135);
    for (const auto& v : values) {
        double scale = v.unit==L'V' ? maxV : maxA; if (!scale || !v.amplitude) continue;
        const double length = 46.0 * v.amplitude / scale;
        const int x = static_cast<int>(std::lround(95 + length*std::cos(v.angle*3.141592653589793/180.0)));
        const int y = static_cast<int>(std::lround(76 - length*std::sin(v.angle*3.141592653589793/180.0)));
        Color color(255,v.unit==L'V'?36:166,v.unit==L'V'?99:54,v.unit==L'V'?167:64); Pen pen(color,2.0f); graphics.DrawLine(&pen,95,76,x,y);
        const double dx=x-95, dy=y-76, len=std::sqrt(dx*dx+dy*dy); if (len>1) { const int ax=static_cast<int>(std::lround(x-7*dx/len+3*dy/len)), ay=static_cast<int>(std::lround(y-7*dy/len-3*dx/len)); const int bx=static_cast<int>(std::lround(x-7*dx/len-3*dy/len)), by=static_cast<int>(std::lround(y-7*dy/len+3*dx/len)); graphics.DrawLine(&pen,x,y,ax,ay); graphics.DrawLine(&pen,x,y,bx,by); }
        FontFamily family(L"Segoe UI"); Font font(&family,9,FontStyleRegular,UnitPixel); SolidBrush brush(color); PointF origin(static_cast<REAL>(x),static_cast<REAL>(y)); graphics.DrawString(v.name.c_str(),-1,&font,origin,&brush);
    }
    FontFamily family(L"Segoe UI"); Font caption(&family,9,FontStyleRegular,UnitPixel); SolidBrush captionBrush(Color(255,83,97,116)); graphics.DrawString(L"Diagrama reconstruido",-1,&caption,PointF(53,148),&captionBrush);
    IStream* stream=nullptr; if (FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream))) return {}; CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}}; if (bitmap.Save(stream,&png,nullptr)!=Ok) { stream->Release(); return {}; }
    STATSTG stat{}; stream->Stat(&stat,STATFLAG_NONAME); HGLOBAL global=nullptr; GetHGlobalFromStream(stream,&global); SIZE_T size=GlobalSize(global); std::vector<BYTE> bits(size); void* locked=GlobalLock(global); std::memcpy(bits.data(),locked,size); GlobalUnlock(global); stream->Release();
    static constexpr char hex[]="0123456789abcdef"; std::string out; out.reserve(size*2+100); out="{\\pict\\pngblip\\picw190\\pich172\\picwgoal3800\\pichgoal3440\n"; for (BYTE b:bits) { out+=hex[b>>4]; out+=hex[b&15]; } out+='}'; return out;
}

inline constexpr View cfb("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1",8);
struct TableSpan { size_t first, last; };
inline std::vector<TableSpan> tableSpans(View data) {
    constexpr View frame("\xff\xff\x08\x00\x10\x00" "COMREBasicStream",22);
    const size_t bodyStart = frame.size()+4;
    size_t size = number(data,frame.size(),4);
    if (!data.starts_with(frame) || data.size()<bodyStart || size>data.size()-bodyStart) return {};
    std::string token("\xff\xfe\xff\x10",4);
    for (char c : View("OMICRON.RTabCtrl")) { token+=c; token+='\0'; }
    std::vector<TableSpan> tables;
    for (size_t pos=bodyStart+size; (pos=data.find(token,pos))!=View::npos;) {
        pos += token.size();
        size_t count = number(data,pos,4);
        pos += 4;
        if (!data.substr(std::min(pos,data.size())).starts_with(cfb)) continue;
        if (count<512 || count>16*1024*1024 || count>data.size()-pos || tables.size()>=256) return {};
        tables.push_back({pos,pos+count});
        pos += count;
    }
    return tables;
}

inline bool editCell(Bytes& module, const Target& target, const std::wstring& value) {
    auto raw = bytes(module);
    const auto spans = tableSpans(raw);
    if (target.table >= spans.size()) return false;
    const auto span = spans[target.table];
    Bytes table;
    if (readTable(raw.substr(span.first,span.last-span.first),&target,&value,&table).empty()) return false;
    const auto length = static_cast<uint32_t>(table.size());
    for (size_t i=0; i<4; ++i) module[span.first-4+i] = static_cast<uint8_t>(length>>(i*8));
    auto next = module.erase(module.begin()+span.first,module.begin()+span.last);
    module.insert(next,table.begin(),table.end());
    return true;
}

inline std::wstring anchor(size_t index) { return L"\ue001" + std::to_wstring(index) + L"\ue002"; }

inline void appendTable(std::string& output, const Rows& rows, int width,
                        const std::wstring& module, size_t table, std::vector<Target>& targets) {
    auto content = [&](const Cell& value,size_t row,size_t column) {
        if (!value.editable || targets.size() >= 12000) return escape(value.text);
        auto prefix = escape(anchor(targets.size()));
        targets.push_back({module,table,row,column,value.text});
        return prefix + escape(value.empty() ? L"…" : value.text) + "\\u-8190?";
    };
    if (rows.empty()) return;
    for (const auto& row : rows) {
        const size_t rowIndex = static_cast<size_t>(&row-rows.data());
        if (std::count_if(row.begin(),row.end(),[](const auto& value) { return !value.empty(); })==1 &&
            !row.front().empty() && row.size()==1) {
            output += "\\pard\\sb160\\sa90\\b\\fs22 " + content(row.front(),rowIndex,0) + "\\b0\\par\n";
            continue;
        }
        const bool header = &row == &rows.front();
        output += "\\trowd\\trgaph90\\trleft0";
        for (size_t i=0; i<row.size(); ++i) {
            output += "\\clbrdrt\\brdrs\\brdrw10\\brdrcf2\\clbrdrl\\brdrs\\brdrw10\\brdrcf2"
                      "\\clbrdrb\\brdrs\\brdrw10\\brdrcf2\\clbrdrr\\brdrs\\brdrw10\\brdrcf2";
            if (header) output += "\\clcbpat3";
            output += "\\cellx" + std::to_string(static_cast<size_t>(width)*(i+1)/row.size());
        }
        output += ' ';
        for (size_t column=0; column<row.size(); ++column) {
            output += "\\pard\\intbl\\sa50\\sb50\\fs19 ";
            output += content(row[column],rowIndex,column);
            output += "\\cell ";
        }
        output += "\\row\n";
    }
    auto picture = vectorPicture(rows);
    if (!picture.empty()) output += "\\pard\\qc\\sa80 " + picture + "\\par\n";
    output += "\\pard\\sa95\\par\n";
}

template<class DecodeRtf>
bool appendModule(std::string& output, const Bytes& raw, int width, DecodeRtf decode, const std::wstring& module, std::vector<Target>& targets) {
    constexpr View frame("\xff\xff\x08\x00\x10\x00" "COMREBasicStream",22);
    auto data = bytes(raw);
    size_t size = number(data,frame.size(),4), bodyStart = frame.size()+4;
    if (!data.starts_with(frame) || data.size()<bodyStart || size>data.size()-bodyStart || size>1024*1024) return false;
    auto body = data.substr(bodyStart,size);
    if (!body.starts_with("{\\rtf")) return false;
    constexpr View placeholder(R"(\objattph\'20)");
    std::string editable(body.substr(0,body.find('\0')));
    for (size_t offset=0; (offset=editable.find(placeholder,offset))!=std::string::npos; offset+=8)
        editable.replace(offset,placeholder.size(),R"(\u-8192?)");
    auto text = decode(editable);
    std::vector<Rows> tables;
    for (const auto& span : tableSpans(data))
        tables.push_back(readTable(data.substr(span.first,span.last-span.first)));
    if (static_cast<size_t>(std::count(text.begin(),text.end(),L'\ue000'))!=tables.size()) return false;
    size_t table=0;
    std::wstring line;
    auto heading = [&]() {
        auto first=line.find_first_not_of(L" \t");
        if (first!=std::wstring::npos)
            output += "\\pard\\sb190\\sa105\\b\\fs26 " + escape(line.substr(first)) + "\\b0\\par\n";
        line.clear();
    };
    for (wchar_t c : text) {
        if (c==L'\ue000') {
            heading();
            if (tables[table].empty()) output += "\\pard\\sa100\\fs19 Tabla sin vista disponible.\\par\n";
            else appendTable(output,tables[table],width,module,table,targets);
            ++table;
        } else if (c==L'\r' || c==L'\n') heading();
        else line += c;
    }
    heading();
    return !text.empty();
}
}

