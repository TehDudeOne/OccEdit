#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace quick_results {
using Bytes = std::vector<uint8_t>;
struct Span { size_t first, last; };
struct Row {
    std::array<std::wstring, 9> fields;
    Span summary, title, comment, table, tableTitle;
    size_t voltage, current;
};
struct Model {
    std::vector<Row> rows;
    bool readings = false;
    bool hasCurrentState = false;
    Span currentTitle{}, currentComment{};
    size_t currentVoltage{}, currentCurrent{};
};
bool Parse(const Bytes& summary, const Bytes& states, const Bytes& report, Model& model);
using Edit = std::array<std::wstring, 4>;
bool Apply(Bytes& summary, Bytes& states, Bytes& report, Model& model,
           size_t index, const Edit& edit, std::wstring& error);
}
