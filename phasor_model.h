#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace report_preview::phasor {

struct VectorValue {
    std::wstring name;
    double amplitude{};
    wchar_t unit{};
    double angle{};
};

inline std::wstring firstLine(std::wstring_view text) {
    const auto end = text.find_first_of(L"\r\n");
    if (end != std::wstring_view::npos) text = text.substr(0, end);
    const auto begin = text.find_first_not_of(L" \t");
    if (begin == std::wstring_view::npos) return {};
    const auto last = text.find_last_not_of(L" \t");
    return std::wstring(text.substr(begin, last - begin + 1));
}

inline bool parseMagnitude(std::wstring_view text, VectorValue& result) {
    std::wstring value = firstLine(text);
    if (value.empty()) return false;
    const wchar_t unit = value.back();
    if (unit != L'V' && unit != L'A') return false;
    value.pop_back();
    std::replace(value.begin(), value.end(), L',', L'.');
    const wchar_t* begin = value.c_str();
    wchar_t* end = nullptr;
    const double magnitude = std::wcstod(begin, &end);
    if (end == begin || !std::isfinite(magnitude) || magnitude < 0) return false;
    while (*end && std::iswspace(*end)) ++end;
    if (*end) return false;
    result.amplitude = magnitude;
    result.unit = unit;
    return true;
}

inline bool parseAngle(std::wstring_view text, double& angle) {
    std::wstring value = firstLine(text);
    if (value.empty()) return false;
    std::replace(value.begin(), value.end(), L',', L'.');
    const wchar_t* begin = value.c_str();
    wchar_t* end = nullptr;
    const double degrees = std::wcstod(begin, &end);
    if (end == begin || !std::isfinite(degrees)) return false;
    while (*end && std::iswspace(*end)) ++end;
    if (*end == L'\u00b0') ++end;
    while (*end && std::iswspace(*end)) ++end;
    if (*end) return false;
    angle = degrees;
    return true;
}

// El primer campo de una tabla QuickCMC puede estar vacio en todas las filas
// excepto la primera (celdas agrupadas visualmente). Esas filas contienen
// igualmente las magnitudes y los angulos de los fasores.
template <typename Rows>
std::vector<VectorValue> extract(const Rows& rows) {
    std::vector<VectorValue> result;
    for (size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() < 3) continue;
        for (size_t c = 0; c + 1 < row.size(); ++c) {
            VectorValue vector;
            if (!parseMagnitude(row[c].text, vector) ||
                !parseAngle(row[c + 1].text, vector.angle)) continue;

            // Elegir la etiqueta mas cercana a la magnitud (ej. 'V L2-E'),
            // nunca una celda agrupada como 'Directo'.
            for (size_t preceding = 0; preceding < c; ++preceding) {
                const auto label = firstLine(row[preceding].text);
                if (!label.empty()) vector.name = label;
            }
            if (!vector.name.empty()) result.push_back(std::move(vector));
            break;
        }
    }
    return result;
}

} // namespace report_preview::phasor
