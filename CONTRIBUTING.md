# Contribuir

## Entorno

- Windows 11 x64 y Visual Studio 2022 Build Tools (Desktop development with C++, compilador MSVC y Windows SDK).
- PowerShell abierto en la raíz: `./Compilar.bat /quiet`.
- Test de fasores (Developer Command Prompt x64): `cl /nologo /utf-8 /std:c++20 /EHsc /W4 tests\test_phasor_model.cpp /Fe:dist\test_phasor_model.exe`.

No se incluye ni se requiere Test Universe para compilar. Para verificar compatibilidad real de archivos modificados debe usarse el software oficial en un entorno autorizado.

## Normas para cambios

1. Incluye pruebas de regresión para toda modificación en análisis binario, compresión o geometría.
2. Nunca adjuntes archivos OCC de clientes, resultados reales ni datos personales.
3. No cambies el guardado o parsing de CFB/7z sin validar que se conserva el resto del documento.
4. No publiques builds como **firmadas** sin una solicitud real y completada de SignPath.
