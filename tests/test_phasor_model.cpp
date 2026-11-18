#include "../phasor_model.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

struct Cell { std::wstring text; };
using Rows = std::vector<std::vector<Cell>>;

static void assertVector(const report_preview::phasor::VectorValue& v,
                         const wchar_t* name, double magnitude, double angle, wchar_t unit) {
    assert(v.name == name);
    assert(std::abs(v.amplitude - magnitude) < 1.e-9);
    assert(std::abs(v.angle - angle) < 1.e-9);
    assert(v.unit == unit);
}

int main() {
    // Caso reproducido del informe del usuario: la primera columna esta vacia
    // tras la fila 'Directo'. No debe descartarse el fasor de L2.
    const Rows rows = {
        {{L"Calculo de falta:"}, {}, {}, {}, {}},
        {{L"Modo de entrada en tabla"}, {L"Parametros"}, {}, {}, {}},
        {{L"Directo"}, {L"V L1-E"}, {L"0,00 V"}, {L"0,00 \u00b0"}, {L"50,000 Hz"}},
        {{}, {L"V L2-E"}, {L"115,0 V"}, {L"-120,00 \u00b0"}, {L"50,000 Hz"}},
        {{}, {L"V L3-E"}, {L"0,00 V"}, {L"120,00 \u00b0"}, {L"50,000 Hz"}},
        {{}, {L"I L1"}, {L"0,00 A"}, {L"0,00 \u00b0"}, {L"50,000 Hz"}},
        {{}, {L"I L2"}, {L"0,00 A"}, {L"-120,00 \u00b0"}, {L"50,000 Hz"}},
        {{}, {L"I L3"}, {L"0,00 A"}, {L"120,00 \u00b0"}, {L"50,000 Hz"}},
    };
    auto vectors = report_preview::phasor::extract(rows);
    assert(vectors.size() == 6);
    assertVector(vectors[0], L"V L1-E", 0., 0., L'V');
    assertVector(vectors[1], L"V L2-E", 115., -120., L'V');
    assertVector(vectors[2], L"V L3-E", 0., 120., L'V');
    assertVector(vectors[3], L"I L1", 0., 0., L'A');

    const double deg = vectors[1].angle * 3.14159265358979323846 / 180.;
    const double x = 46 * std::cos(deg), y = -46 * std::sin(deg);
    assert(x < -22 && x > -24 && y > 39 && y < 41);

    using namespace report_preview::phasor;
    VectorValue parsed{};
    double angle{};
    assert(parseMagnitude(L"115,0 V", parsed) && parsed.amplitude == 115);
    assert(parseMagnitude(L"0,00 A", parsed) && parsed.amplitude == 0);
    assert(!parseMagnitude(L"50,000 Hz", parsed));
    assert(!parseMagnitude(L"V L2-E", parsed));
    assert(!parseMagnitude(L"infinity V", parsed));
    assert(!parseMagnitude(L"-115 V", parsed));
    assert(parseAngle(L"-120,00 \u00b0", angle) && angle == -120);
    assert(!parseAngle(L"-120 \u00b0 basura", angle));
    assert(extract(Rows{}).empty());
    std::cout << "PASS: 6 vectores, V L2-E a -120 deg correctamente extraido\n";
}
