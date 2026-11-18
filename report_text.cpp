#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "report_text.h"
#include <richedit.h>
#include <algorithm>
#include <cstring>
#include <string_view>

namespace {
size_t findBytes(const std::vector<uint8_t>& data, const char* pattern, size_t count,
                 size_t begin, size_t limit) {
    if (count == 0 || begin > limit || limit > data.size()) return std::string::npos;
    const uint8_t* first = data.data() + begin;
    const uint8_t* last = data.data() + limit;
    const uint8_t* found = std::search(first, last,
        reinterpret_cast<const uint8_t*>(pattern),
        reinterpret_cast<const uint8_t*>(pattern) + count);
    return found == last ? std::string::npos : static_cast<size_t>(found - data.data());
}

std::vector<uint8_t> encodeText(HWND editor, std::wstring& error) {
    const int chars = GetWindowTextLengthW(editor);
    if (chars < 0 || chars > 1024 * 1024) { error = L"El texto editado supera el límite compatible."; return {}; }
    std::wstring text(static_cast<size_t>(chars) + 1, L'\0');
    const int copied = GetWindowTextW(editor, text.data(), chars + 1);
    text.resize(static_cast<size_t>(std::max(0, copied)));

    std::vector<uint8_t> out;
    const char prefix[] = "\\pard\\plain\\f0\\fs20 ";
    out.insert(out.end(), prefix, prefix + sizeof(prefix) - 1);
    DWORD previous = 0;
    for (int i = 0; i < copied; ++i) {
        CHARRANGE range{ i, i + 1 };
        SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        SendMessageW(editor, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        DWORD styles = 0;
        if (format.dwEffects & CFE_BOLD) styles |= 1;
        if (format.dwEffects & CFE_ITALIC) styles |= 2;
        if (format.dwEffects & CFE_UNDERLINE) styles |= 4;
        constexpr DWORD flags[3] = {1, 2, 4};
        constexpr char names[3] = {'b','i','u'};
        for (unsigned n = 0; n < 3; ++n) {
            if ((styles & flags[n]) != (previous & flags[n])) {
                out.push_back('\\'); out.push_back(static_cast<uint8_t>(names[n]));
                if (!(styles & flags[n])) out.push_back('0');
                out.push_back(' ');
            }
        }
        previous = styles;
        const wchar_t ch = text[static_cast<size_t>(i)];
        if (ch == L'\r') {
            if (i + 1 < copied && text[static_cast<size_t>(i + 1)] == L'\n') ++i;
            const char paragraph[] = "\\par\r\n";
            out.insert(out.end(), paragraph, paragraph + sizeof(paragraph) - 1);
        } else if (ch == L'\n') {
            const char paragraph[] = "\\par\r\n";
            out.insert(out.end(), paragraph, paragraph + sizeof(paragraph) - 1);
        } else if (ch == L'\t') {
            const char tab[] = "\\tab "; out.insert(out.end(), tab, tab + sizeof(tab) - 1);
        } else if (ch == L'\\' || ch == L'{' || ch == L'}') {
            out.push_back('\\'); out.push_back(static_cast<uint8_t>(ch));
        } else if (ch >= 32 && ch < 127) out.push_back(static_cast<uint8_t>(ch));
        else {
            const short signedCode = static_cast<short>(ch);
            const std::string control = "\\u" + std::to_string(signedCode) + "?";
            out.insert(out.end(), control.begin(), control.end());
        }
    }
    constexpr DWORD flags[3] = {1, 2, 4};
    constexpr char names[3] = {'b','i','u'};
    for (unsigned n = 0; n < 3; ++n) if (previous & flags[n]) {
        const char control[] = {'\\', names[n], '0', ' '}; out.insert(out.end(), control, control + 4);
    }
    const char end[] = "\\par\r\n";
    out.insert(out.end(), end, end + sizeof(end) - 1);
    CHARRANGE caret{copied, copied};
    SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&caret));
    return out;
}
}

bool FindEditableText(const std::vector<uint8_t>& body, size_t& start, size_t& end, std::wstring& error) {
    end = std::string::npos;
    if (body.size() >= 3)
        end = findBytes(body, "}\r\n", 3, 0, body.size());
    while (end != std::string::npos) {
        const size_t next = findBytes(body, "}\r\n", 3, end + 3, body.size());
        if (next == std::string::npos) break;
        end = next;
    }
    if (end == std::string::npos) { error = L"No se reconoció el cierre RTF del informe OCC."; return false; }
    const std::string_view bytes(reinterpret_cast<const char*>(body.data()), end);
    const size_t last = bytes.rfind("\\objattph");
    if (last != std::string::npos) {
        const size_t newline = findBytes(body, "\n", 1, last + 9, end);
        if (newline != std::string::npos && newline > last) start = newline + 1;
        else start = end;
    } else {
        const size_t view = findBytes(body, "\\viewkind", 9, 0, end);
        start = view == std::string::npos ? end : findBytes(body, "\\pard", 5, view, end);
        if (start == std::string::npos) start = end;
    }
    if (start > end) { error = L"El texto libre del informe no tiene límites válidos."; return false; }
    return true;
}

bool BuildEditedBody(const std::vector<uint8_t>& original, HWND richEdit,
                     std::vector<uint8_t>& result, std::wstring& error) {
    size_t start = 0, end = 0;
    if (!FindEditableText(original, start, end, error)) return false;
    std::vector<uint8_t> encoded = encodeText(richEdit, error);
    if (!error.empty()) return false;
    result.clear();
    result.reserve(start + encoded.size() + original.size() - end);
    result.insert(result.end(), original.begin(), original.begin() + start);
    result.insert(result.end(), encoded.begin(), encoded.end());
    result.insert(result.end(), original.begin() + end, original.end());
    return true;
}
