#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <map>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <ole2.h>
#include <richedit.h>
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include "gzip_codec.h"
#include "report_text.h"
#include "sevenzip_codec.h"
#include "quick_results.h"
#include "report_preview.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")

namespace {
constexpr int ID_FILE_OPEN = 1001;
constexpr int ID_FILE_CLOSE = 1002;
constexpr int ID_FILE_EXIT = 1003;
constexpr int ID_FILE_SAVE_COPY = 1004;
constexpr int ID_FILE_LIST = 2001;
constexpr int ID_STATUS = 2002;
constexpr int ID_REPORT = 2003;
constexpr int ID_TABS = 2004;
constexpr int ID_FREE_TEXT = 2005;

struct Entry {
    std::wstring name;
    bool storage = false;
    ULONGLONG size = 0;
    unsigned depth = 0;
};

struct Document {
    IStorage* root = nullptr;
    std::wstring path;
    std::vector<Entry> entries;
    std::vector<uint8_t> rtf;
    std::vector<uint8_t> quickCmc;
    std::string quickCmcName;
    std::wstring quickCmcStorageName;
    sevenzip_codec::FileMetadata quickCmcMetadata;
    std::vector<uint8_t> summary, quickReport, originalSummary, originalStates, originalQuickReport;
    std::string reportName;
    sevenzip_codec::FileMetadata reportMetadata;
    quick_results::Model results;
    struct ReportModule {
        quick_results::Bytes data;
        std::string member;
        sevenzip_codec::FileMetadata metadata;
        bool dirty = false;
    };
    std::map<std::wstring,ReportModule> reports;
    bool quickCmcSupported = false;
    ~Document() { if (root) root->Release(); }
};

HWND g_window = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
HWND g_report = nullptr;
HWND g_freeText = nullptr;
HWND g_tabs = nullptr;
HWND g_results = nullptr, g_apply = nullptr, g_title = nullptr;
HWND g_closeButton = nullptr, g_saveButton = nullptr, g_openButton = nullptr;
HFONT g_uiFont = nullptr, g_titleFont = nullptr;
std::array<HWND,4> g_fields{}, g_labels{};
int g_selectedResult = -1;
std::vector<report_preview::Target> g_reportCells;

HMODULE g_richEditModule = nullptr;
std::unique_ptr<Document> g_document;
void setStatus(const std::wstring& value);

bool quickDirty() {
    return g_document && (g_document->summary != g_document->originalSummary ||
        g_document->quickCmc != g_document->originalStates ||
        g_document->quickReport != g_document->originalQuickReport);
}

std::wstring fieldText(HWND field) {
    int size = GetWindowTextLengthW(field);
    std::wstring value(static_cast<size_t>(size) + 1, L'\0');
    value.resize(static_cast<size_t>(GetWindowTextW(field,value.data(),size+1)));
    return value;
}
void fillResults(int selected) {
    SendMessageW(g_results,CB_RESETCONTENT,0,0);
    g_selectedResult = -1;
    if (g_document) {
        for (const auto& row : g_document->results.rows) {
            const auto label = row.fields[2] + L". " + row.fields[3] + L"  |  " + row.fields[5];
            SendMessageW(g_results,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
    }
    if (SendMessageW(g_results,CB_SETCURSEL,selected,0) != CB_ERR) g_selectedResult = selected;
    constexpr size_t columns[] = {3,4,7,8};
    for (size_t i = 0; i < g_fields.size(); ++i) {
        const bool enabled = g_selectedResult >= 0;
        SetWindowTextW(g_fields[i], enabled ? g_document->results.rows[g_selectedResult].fields[columns[i]].c_str() : L"");
        EnableWindow(g_fields[i],enabled && (i < 2 || g_document->results.readings));
        SendMessageW(g_fields[i],EM_SETMODIFY,FALSE,0);
    }
    EnableWindow(g_apply,g_selectedResult >= 0);
}

bool applyResult(HWND owner) {
    if (g_selectedResult < 0) return true;
    quick_results::Edit edit;
    for (size_t i = 0; i < g_fields.size(); ++i) edit[i] = fieldText(g_fields[i]);
    auto& doc = *g_document;
    std::wstring error;
    if (!quick_results::Apply(doc.summary,doc.quickCmc,doc.quickReport,doc.results,
                              static_cast<size_t>(g_selectedResult),edit,error)) {
        MessageBoxW(owner,error.c_str(),L"QuickCMC",MB_OK | MB_ICONWARNING);
        return false;
    }
    for (HWND field : g_fields) SendMessageW(field,EM_SETMODIFY,FALSE,0);
    return true;
}

bool openDocument(HWND owner, const wchar_t* initialPath = nullptr, bool skipDiscardConfirmation = false,
                  const Document* expected = nullptr, const std::vector<uint8_t>* expectedRtf = nullptr);
DWORD CALLBACK streamToRichEdit(DWORD_PTR cookie, LPBYTE buffer, LONG bytes, LONG* written) {
    auto* state = reinterpret_cast<std::pair<const std::vector<uint8_t>*, size_t>*>(cookie);
    const size_t remaining = state->first->size() - state->second;
    const size_t count = std::min<size_t>(remaining, static_cast<size_t>(bytes));
    if (count) std::memcpy(buffer, state->first->data() + state->second, count);
    state->second += count;
    *written = static_cast<LONG>(count);
    return 0;
}

bool readStream(IStorage* storage, const wchar_t* name, std::vector<uint8_t>& data, std::wstring& error) {
    IStream* stream = nullptr;
    HRESULT hr = storage->OpenStream(name, nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
    if (FAILED(hr)) { error = L"Falta el flujo de informe OCC."; return false; }
    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    const ULONGLONG size = SUCCEEDED(hr) ? ((static_cast<ULONGLONG>(stat.cbSize.HighPart) << 32) | stat.cbSize.LowPart) : 0;
    if (FAILED(hr) || size > 100ull * 1024 * 1024) {
        stream->Release(); error = L"El flujo de informe no se pudo leer o supera 100 MB."; return false;
    }
    data.resize(static_cast<size_t>(size));
    size_t offset = 0;
    while (offset < data.size()) {
        const ULONG request = static_cast<ULONG>(std::min<size_t>(data.size() - offset, 1024 * 1024));
        ULONG got = 0;
        hr = stream->Read(data.data() + offset, request, &got);
        if (FAILED(hr) || got == 0) { stream->Release(); error = L"El flujo de informe está truncado."; return false; }
        offset += got;
    }
    stream->Release();
    std::vector<uint8_t> decoded;
    if (!gzip_codec::Decompress(data, decoded, gzip_codec::kDefaultLimit, &error)) return false;
    data = std::move(decoded);
    return true;
}

bool readRawStream(IStorage* storage, const wchar_t* name, std::vector<uint8_t>& data, std::wstring& error) {
    IStream* stream = nullptr;
    HRESULT hr = storage->OpenStream(name, nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
    if (FAILED(hr)) { error = L"Falta el flujo QuickCMC dentro del OCC."; return false; }
    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    const ULONGLONG size = SUCCEEDED(hr) ? ((static_cast<ULONGLONG>(stat.cbSize.HighPart) << 32) | stat.cbSize.LowPart) : 0;
    if (FAILED(hr) || size > 128ull * 1024 * 1024) {
        stream->Release(); error = L"El flujo QuickCMC supera el límite de lectura permitido."; return false;
    }
    data.resize(static_cast<size_t>(size));
    size_t offset = 0;
    while (offset < data.size()) {
        const ULONG request = static_cast<ULONG>(std::min<size_t>(data.size() - offset, 1024 * 1024));
        ULONG got = 0;
        hr = stream->Read(data.data() + offset, request, &got);
        if (FAILED(hr) || got == 0) { stream->Release(); error = L"El flujo QuickCMC está truncado."; return false; }
        offset += got;
    }
    stream->Release();
    return true;
}

void loadRtf(HWND control, const std::vector<uint8_t>& rtf) {
    std::pair<const std::vector<uint8_t>*, size_t> state{&rtf, 0};
    EDITSTREAM edit{};
    edit.dwCookie = reinterpret_cast<DWORD_PTR>(&state);
    edit.pfnCallback = streamToRichEdit;
    SendMessageW(control, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&edit));
}

Document::ReportModule* reportModule(Document& doc, const std::wstring& module) {
    if (auto located=doc.reports.find(module); located!=doc.reports.end()) return &located->second;
    IStorage* child = nullptr;
    if (FAILED(doc.root->OpenStorage(module.c_str(),nullptr,STGM_READ | STGM_SHARE_EXCLUSIVE,
            nullptr,0,&child))) return nullptr;
    Document::ReportModule report;
    quick_results::Bytes packed;
    std::wstring error;
    bool found = readRawStream(child,L"RTF",packed,error);
    child->Release();
    if (found) {
        found = packed.size() >= 2 && packed[0]==0x1f && packed[1]==0x8b
            ? gzip_codec::Decompress(packed,report.data,gzip_codec::kDefaultLimit,&error)
            : sevenzip_codec::ExtractSingleLzma2(packed,report.member,report.data,&report.metadata,&error);
    }
    if (!found) report.data.clear();
    return &doc.reports.emplace(module,std::move(report)).first->second;
}

void attachReportCells() {
    long cursor = 0;
    for (size_t i=0; i<g_reportCells.size(); ++i) {
        auto token = report_preview::anchor(i);
        FINDTEXTEXW open{};
        open.chrg = {cursor,-1};
        open.lpstrText = token.data();
        if (SendMessageW(g_report,EM_FINDTEXTEXW,FR_DOWN | FR_MATCHCASE,reinterpret_cast<LPARAM>(&open))<0) continue;
        wchar_t closing[] = L"\ue002";
        FINDTEXTEXW end{};
        end.chrg = {open.chrgText.cpMax,-1};
        end.lpstrText = closing;
        if (SendMessageW(g_report,EM_FINDTEXTEXW,FR_DOWN | FR_MATCHCASE,reinterpret_cast<LPARAM>(&end))<0) break;
        const auto remove = [](CHARRANGE& range) {
            SendMessageW(g_report,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));
            SendMessageW(g_report,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L""));
        };
        remove(end.chrgText);
        remove(open.chrgText);
        g_reportCells[i].first = open.chrgText.cpMin;
        g_reportCells[i].last = end.chrgText.cpMin - (open.chrgText.cpMax-open.chrgText.cpMin);
        cursor = g_reportCells[i].last;
    }
}

void showReport() {
    if (!g_report || !g_document) return;
    g_reportCells.clear();
    HWND scratch = CreateWindowExW(0,L"RICHEDIT50W",L"",WS_CHILD | ES_MULTILINE,
        0,0,0,0,g_window,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!scratch) return;
    RECT area{};
    GetClientRect(g_report,&area);
    const int width = std::clamp((area.right-64)*15,4500L,14500L);
    std::string display = "{\\rtf1\\ansi\\uc1\\deff0{\\fonttbl{\\f0 Segoe UI;}}"
        "{\\colortbl;\\red23\\green41\\blue65;\\red204\\green216\\blue229;\\red235\\green242\\blue250;}"
        "\\cf1\\f0 ";
    std::vector<std::wstring> modules;
    for (const auto& entry : g_document->entries) {
        if (entry.storage && entry.depth == 0 && entry.name.starts_with(L"Embedding "))
            modules.push_back(entry.name);
    }
    std::stable_sort(modules.begin(),modules.end(),[](const auto& a,const auto& b) {
        return a.size()==b.size() ? a<b : a.size()<b.size();
    });
    bool available = false;
    auto decode = [scratch](const std::string& rtf) -> std::wstring {
        if (rtf.find("\\object") != std::string::npos || rtf.find("\\field") != std::string::npos) return {};
        loadRtf(scratch,{rtf.begin(),rtf.end()});
        return fieldText(scratch);
    };
    for (const auto& module : modules) {
        if (module==g_document->quickCmcStorageName && !g_document->quickReport.empty()) {
            available |= report_preview::appendModule(display,g_document->quickReport,width,decode,module,g_reportCells);
        } else if (auto* stored = reportModule(*g_document,module); stored && !stored->data.empty()) {
            available |= report_preview::appendModule(display,stored->data,width,decode,module,g_reportCells);
        }
    }
    DestroyWindow(scratch);
    if (!available) display += "\\pard\\fs22 Este archivo no contiene secciones compatibles con la vista previa.\\par ";
    display += '}';
    SendMessageW(g_report,WM_SETREDRAW,FALSE,0);
    SendMessageW(g_report,EM_SETREADONLY,FALSE,0);
    loadRtf(g_report,{display.begin(),display.end()});
    attachReportCells();
    SendMessageW(g_report,EM_EMPTYUNDOBUFFER,0,0);
    SendMessageW(g_report,EM_SETSEL,0,0);
    SendMessageW(g_report,EM_SCROLLCARET,0,0);
    SendMessageW(g_report,EM_SETREADONLY,TRUE,0);
    SendMessageW(g_report,WM_SETREDRAW,TRUE,0);
    InvalidateRect(g_report,nullptr,TRUE);
}

struct CellDialog {
    std::wstring value;
    HWND field{};
};

INT_PTR CALLBACK cellDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<CellDialog*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if (message==WM_INITDIALOG) {
        state = reinterpret_cast<CellDialog*>(lParam);
        SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog,L"Editar celda del informe");
        RECT client{};
        GetClientRect(dialog,&client);
        const auto control = [&](const wchar_t* type, const wchar_t* caption, DWORD style,
                                 int x,int y,int width,int height,int id) {
            HWND child = CreateWindowExW(0,type,caption,WS_CHILD | WS_VISIBLE | style,x,y,width,height,
                dialog,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(g_uiFont),TRUE);
            return child;
        };
        control(L"STATIC",L"Texto de la celda",0,16,12,200,24,0);
        state->field = control(L"EDIT",state->value.c_str(),WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                              16,42,client.right-32,29,100);
        SendMessageW(state->field,EM_LIMITTEXT,254,0);
        control(L"BUTTON",L"Cancelar",WS_TABSTOP,client.right-226,88,100,32,IDCANCEL);
        control(L"BUTTON",L"Aplicar",WS_TABSTOP | BS_DEFPUSHBUTTON,client.right-116,88,100,32,IDOK);
        SendMessageW(dialog,DM_SETDEFID,IDOK,0);
        SetFocus(state->field);
        SendMessageW(state->field,EM_SETSEL,0,-1);
        return FALSE;
    }
    if (message==WM_COMMAND && LOWORD(wParam)==IDOK && state) {
        state->value = fieldText(state->field);
        EndDialog(dialog,IDOK);
        return TRUE;
    }
    if (message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wParam)==IDCANCEL)) {
        EndDialog(dialog,IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool editReportField(const report_preview::Target& target, const std::wstring& value, std::wstring& error) {
    auto& doc = *g_document;
    if (target.module==doc.quickCmcStorageName && !doc.quickReport.empty()) {
        const std::wstring prefix = L"Título: ";
        if (target.value.starts_with(prefix) && !doc.results.rows.empty()) {
            size_t result = 0;
            const auto spans = report_preview::tableSpans(report_preview::bytes(doc.quickReport));
            const auto raw = report_preview::bytes(doc.quickReport);
            for (size_t i=0; i<target.table && i<spans.size(); ++i) {
                const auto span = spans[i];
                auto rows = report_preview::readTable(raw.substr(span.first,span.last-span.first));
                if (!rows.empty() && !rows.front().empty() && rows.front().front().text.starts_with(prefix)) ++result;
            }
            if (result >= doc.results.rows.size() || prefix+doc.results.rows[result].fields[3]!=target.value) {
                error = L"No se pudo asociar este título con un resultado QuickCMC.";
                return false;
            }
            const auto fields = doc.results.rows[result].fields;
            auto title = value.starts_with(prefix) ? value.substr(prefix.size()) : value;
            if (!quick_results::Apply(doc.summary,doc.quickCmc,doc.quickReport,doc.results,result,
                                      {title,fields[4],fields[7],fields[8]},error)) return false;
            fillResults(g_selectedResult);
            return true;
        }
        auto changed = doc.quickReport;
        if (report_preview::editCell(changed,target,value)) {
            quick_results::Model checked;
            if (doc.results.rows.empty() || quick_results::Parse(doc.summary,doc.quickCmc,changed,checked)) {
                if (!doc.results.rows.empty()) doc.results = std::move(checked);
                doc.quickReport = std::move(changed);
                return true;
            }
        }
    } else if (auto* module = reportModule(doc,target.module)) {
        if (report_preview::editCell(module->data,target,value)) {
            module->dirty = true;
            return true;
        }
    }
    error = L"El OCC no pudo conservar esta celda con el texto ingresado. El cambio no se aplicó.";
    return false;
}

void editReportAt(long position) {
    if (!g_document) return;
    auto cell = std::find_if(g_reportCells.begin(),g_reportCells.end(),[position](const auto& field) {
        return field.first>=0 && position>=field.first && position<=field.last;
    });
    if (cell==g_reportCells.end()) {
        setStatus(L"Selecciona una celda que contenga un campo de texto guardado.");
        return;
    }
    const auto target = *cell;
    CellDialog state{target.value};
    struct alignas(DWORD) DialogTemplate {
        DLGTEMPLATE header;
        WORD menu{}, dialogClass{}, title{};
    } dialog{{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,0,0,0,0,265,86}};
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&dialog.header,g_window,cellDialogProc,
                                reinterpret_cast<LPARAM>(&state)) != IDOK || state.value==target.value) return;
    if (!applyResult(g_window)) return;
    std::wstring error;
    if (!editReportField(target,state.value,error)) {
        MessageBoxW(g_window,error.c_str(),L"Editar informe",MB_OK | MB_ICONWARNING);
        return;
    }
    POINT scroll{};
    SendMessageW(g_report,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    showReport();
    SendMessageW(g_report,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    setStatus(L"Celda modificada. Usa Guardar como copia para escribir el cambio en el OCC.");
}

LRESULT CALLBACK reportInput(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                             UINT_PTR, DWORD_PTR) {
    if (message==WM_LBUTTONDBLCLK) {
        POINTL point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        auto position = SendMessageW(window,EM_CHARFROMPOS,0,reinterpret_cast<LPARAM>(&point));
        if (position>=0) editReportAt(static_cast<long>(position));
        return 0;
    }
    if (message==WM_KEYDOWN && wParam==VK_F2) {
        CHARRANGE selection{};
        SendMessageW(window,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
        editReportAt(selection.cpMin);
        return 0;
    }
    return DefSubclassProc(window,message,wParam,lParam);
}

void showFreeText(const std::vector<uint8_t>& body) {
    size_t start = 0, end = 0;
    std::wstring error;
    if (!FindEditableText(body, start, end, error)) {
        SetWindowTextW(g_freeText, L"");
        SendMessageW(g_freeText, EM_SETMODIFY, FALSE, 0);
        setStatus(error);
        return;
    }
    std::vector<uint8_t> wrapper;
    constexpr char prefix[] = "{\\rtf1\\ansi\\ansicpg1252 ";
    wrapper.insert(wrapper.end(), prefix, prefix + sizeof(prefix) - 1);
    wrapper.insert(wrapper.end(), body.begin() + start, body.begin() + end);
    wrapper.push_back('}');
    SetWindowTextW(g_freeText, L"");
    std::pair<const std::vector<uint8_t>*, size_t> state{&wrapper, 0};
    EDITSTREAM edit{};
    edit.dwCookie = reinterpret_cast<DWORD_PTR>(&state);
    edit.pfnCallback = streamToRichEdit;
    SendMessageW(g_freeText, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&edit));
    SendMessageW(g_freeText, EM_SETREADONLY, FALSE, 0);
    SendMessageW(g_freeText, EM_SETMODIFY, FALSE, 0);
    setStatus(L"Edita el texto libre. Guardar como crea una copia OCC y conserva intacto el original.");
}

bool confirmDiscard(HWND owner) {
    if (!g_document) return true;
    const bool pending = std::any_of(g_fields.begin(),g_fields.end(),[](HWND field) {
        return SendMessageW(field,EM_GETMODIFY,0,0) != 0;
    });
    const bool reportDirty = std::any_of(g_document->reports.begin(),g_document->reports.end(),
        [](const auto& entry) { return entry.second.dirty; });
    if (!pending && !reportDirty && !quickDirty() && !SendMessageW(g_freeText, EM_GETMODIFY, 0, 0)) return true;
    return MessageBoxW(owner, L"Hay cambios sin guardar. ¿Descartarlos?",
                       L"Cambios pendientes", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

void saveCopy(HWND owner, const wchar_t* destinationPath = nullptr) {
    if (!g_document) return;
    wchar_t target[MAX_PATH]{};
    if (destinationPath) {
        if (wcslen(destinationPath) >= MAX_PATH) return;
        wcscpy_s(target,destinationPath);
    } else {
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = L"Archivos OMICRON OCC (*.occ)\0*.occ\0\0";
        const std::wstring initial = g_document->path.substr(0, g_document->path.find_last_of(L"\\/"));
        dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
        dialog.lpstrFile = target;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = L"occ";
        dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        dialog.lpstrTitle = L"Guardar una copia editada";
        if (!GetSaveFileNameW(&dialog)) return;
    }
    if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(owner, L"El destino ya existe. Elige un nombre nuevo para protegerlo.",
                    L"El archivo ya existe", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!applyResult(owner)) return;
    std::wstring error;
    auto& doc = *g_document;
    using Bytes = quick_results::Bytes;
    struct Update { std::wstring module; const wchar_t* name; Bytes bytes; };
    std::vector<Update> updates;
    Bytes expectedRtf = doc.rtf;
    if (SendMessageW(g_freeText,EM_GETMODIFY,0,0)) {
        if (!BuildEditedBody(doc.rtf,g_freeText,expectedRtf,error)) {
            MessageBoxW(owner,error.c_str(),L"No se pudo preparar el informe",MB_OK | MB_ICONERROR);
            return;
        }
        updates.push_back({{},L"RTF",gzip_codec::CompressStored(expectedRtf)});
    }
    if (doc.summary != doc.originalSummary)
        updates.push_back({{},L"Contents",gzip_codec::CompressStored(doc.summary)});
    auto archive = [&](const std::wstring& module, const wchar_t* stream, const Bytes& value, const Bytes& original,
                       const std::string& member, const sevenzip_codec::FileMetadata& metadata) {
        if (value == original) return true;
        Bytes packed, verified;
        std::string name;
        sevenzip_codec::FileMetadata actual;
        if (!sevenzip_codec::BuildSingleLzma2(member,value,packed,&metadata,&error) ||
            !sevenzip_codec::ExtractSingleLzma2(packed,name,verified,&actual,&error) ||
            name != member || verified != value ||
            actual.hasAttributes != metadata.hasAttributes || actual.attributes != metadata.attributes ||
            actual.hasLastWriteTime != metadata.hasLastWriteTime || actual.lastWriteTime != metadata.lastWriteTime)
            return false;
        updates.push_back({module,stream,std::move(packed)});
        return true;
    };
    if (!archive(doc.quickCmcStorageName,L"Contents",doc.quickCmc,doc.originalStates,doc.quickCmcName,doc.quickCmcMetadata) ||
        !archive(doc.quickCmcStorageName,L"RTF",doc.quickReport,doc.originalQuickReport,doc.reportName,doc.reportMetadata)) {
        MessageBoxW(owner,L"Falló la comprobación del archivo QuickCMC.",L"No se pudo guardar",MB_OK | MB_ICONERROR);
        return;
    }
    for (const auto& [module,report] : doc.reports) {
        if (!report.dirty) continue;
        if (report.member.empty()) {
            auto packed = gzip_codec::CompressStored(report.data);
            Bytes verified;
            if (!gzip_codec::Decompress(packed,verified,gzip_codec::kDefaultLimit,&error) || verified != report.data) {
                MessageBoxW(owner,L"No se pudo verificar una tabla editada.",L"No se pudo guardar",MB_OK | MB_ICONERROR);
                return;
            }
            updates.push_back({module,L"RTF",std::move(packed)});
        } else if (!archive(module,L"RTF",report.data,{},report.member,report.metadata)) {
            MessageBoxW(owner,L"No se pudo verificar una tabla editada.",L"No se pudo guardar",MB_OK | MB_ICONERROR);
            return;
        }
    }
    std::wstring destination(target);
    std::wstring folder = destination.substr(0,destination.find_last_of(L"\\/")+1);
    wchar_t temp[MAX_PATH]{};
    if (!GetTempFileNameW(folder.c_str(),L"OCC",0,temp)) {
        MessageBoxW(owner,L"No se pudo crear un archivo temporal junto al destino.",L"No se pudo guardar",MB_OK | MB_ICONERROR);
        return;
    }
    HRESULT hr = CopyFileW(doc.path.c_str(),temp,FALSE) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    IStorage* storage = nullptr;
    if (SUCCEEDED(hr) && !updates.empty())
        hr = StgOpenStorage(temp,nullptr,STGM_READWRITE | STGM_SHARE_EXCLUSIVE,nullptr,0,&storage);
    for (const auto& update : updates) {
        if (FAILED(hr)) break;
        IStorage* child = nullptr;
        if (!update.module.empty())
            hr = storage->OpenStorage(update.module.c_str(),nullptr,
                STGM_READWRITE | STGM_SHARE_EXCLUSIVE,nullptr,0,&child);
        IStream* stream = nullptr;
        if (SUCCEEDED(hr))
            hr = (child ? child : storage)->OpenStream(update.name,nullptr,STGM_READWRITE | STGM_SHARE_EXCLUSIVE,0,&stream);
        ULARGE_INTEGER length{};
        length.QuadPart = update.bytes.size();
        if (SUCCEEDED(hr)) hr = stream->SetSize(length);
        ULONG written{};
        if (SUCCEEDED(hr)) hr = stream->Write(update.bytes.data(),static_cast<ULONG>(update.bytes.size()),&written);
        if (SUCCEEDED(hr) && written != update.bytes.size()) hr = STG_E_WRITEFAULT;
        if (SUCCEEDED(hr)) hr = stream->Commit(0);
        if (stream) stream->Release();
        if (SUCCEEDED(hr) && child) hr = child->Commit(0);
        if (SUCCEEDED(hr)) {
            Bytes actual;
            if (!readRawStream(child ? child : storage,update.name,actual,error) || actual != update.bytes)
                hr = STG_E_WRITEFAULT;
        }
        if (child) child->Release();
    }
    if (storage) {
        if (SUCCEEDED(hr)) hr = storage->Commit(0);
        storage->Release();
    }
    if (FAILED(hr) || !MoveFileW(temp,target)) {
        DeleteFileW(temp);
        MessageBoxW(owner,L"No se pudo escribir o verificar la copia OCC.",L"No se pudo guardar",MB_OK | MB_ICONERROR);
        return;
    }
    // No descartamos el documento en memoria hasta que la copia se pueda abrir.
    // Al guardar, sus cambios siguen pendientes y no corresponde preguntar si se descartan.
    if (!openDocument(owner,target,true,&doc,&expectedRtf)) {
        MessageBoxW(owner,
            L"Se creó la copia OCC, pero falló su reapertura o validación de contenido. "
            L"El documento anterior y sus cambios permanecen disponibles. "
            L"No se considera verificada la copia.",
            L"Copia creada, verificación pendiente", MB_OK | MB_ICONWARNING);
        return;
    }
    setStatus(L"Copia OCC guardada y reabierta correctamente.");
}

void selectTab(int selected) {
    if (g_report) ShowWindow(g_report, selected == 0 ? SW_SHOW : SW_HIDE);
    if (g_freeText) ShowWindow(g_freeText, selected == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list,selected == 3 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_results,selected == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_apply,selected == 2 ? SW_SHOW : SW_HIDE);
    for (HWND field : g_fields) ShowWindow(field,selected == 2 ? SW_SHOW : SW_HIDE);
    for (HWND label : g_labels) ShowWindow(label,selected == 2 ? SW_SHOW : SW_HIDE);
    if (selected == 0 && g_document) {
        showReport();
        setStatus(L"Haz doble clic sobre el texto de una celda del informe para editarla.");
    }
    if (selected == 2) setStatus(g_selectedResult >= 0 ?
        (g_document->results.readings ? L"Edita el estado seleccionado. Usa Guardar como copia." :
         L"Las lecturas de este archivo no coinciden entre el resumen y los estados.") :
        L"No se encontraron resultados QuickCMC con estructura compatible.");
}

std::wstring formatSize(ULONGLONG size) {
    std::wostringstream out;
    out << size << L" bytes";
    return out.str();
}

HRESULT enumerateStorage(IStorage* storage, unsigned depth, std::vector<Entry>& entries) {
    if (depth > 12) return STG_E_INVALIDHEADER;
    IEnumSTATSTG* enumerator = nullptr;
    HRESULT hr = storage->EnumElements(0, nullptr, 0, &enumerator);
    if (FAILED(hr)) return hr;

    STATSTG item{};
    ULONG fetched = 0;
    while (enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        Entry entry;
        entry.name = item.pwcsName ? item.pwcsName : L"";
        entry.storage = item.type == STGTY_STORAGE;
        entry.size = (static_cast<ULONGLONG>(item.cbSize.HighPart) << 32) | item.cbSize.LowPart;
        entry.depth = depth;
        entries.push_back(entry);

        if (entry.storage) {
            IStorage* child = nullptr;
            const HRESULT childHr = storage->OpenStorage(
                entry.name.c_str(), nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE,
                nullptr, 0, &child);
            if (SUCCEEDED(childHr) && child) {
                const HRESULT nestedHr = enumerateStorage(child, depth + 1, entries);
                child->Release();
                if (FAILED(nestedHr)) {
                    if (item.pwcsName) CoTaskMemFree(item.pwcsName);
                    enumerator->Release();
                    return nestedHr;
                }
            }
        }
        if (item.pwcsName) CoTaskMemFree(item.pwcsName);
        item = {};
    }
    enumerator->Release();
    return S_OK;
}

std::wstring displayEntry(const Entry& entry) {
    std::wstring line(entry.depth * 2, L' ');
    line += entry.storage ? L"[STORAGE] " : L"[STREAM ] ";
    line += entry.name;
    if (!entry.storage) {
        line += L"    ";
        line += formatSize(entry.size);
    }
    return line;
}

void setStatus(const std::wstring& value) {
    if (g_status) SetWindowTextW(g_status, value.c_str());
}

void closeDocument(HWND owner) {
    if (!confirmDiscard(owner)) return;
    g_document.reset();
    EnableWindow(g_saveButton,FALSE);
    EnableWindow(g_closeButton,FALSE);
    fillResults(-1);
    if (g_list) SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    if (g_report) SetWindowTextW(g_report, L"");
    if (g_freeText) SetWindowTextW(g_freeText, L"");
    EnableMenuItem(GetMenu(g_window), ID_FILE_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(GetMenu(g_window), ID_FILE_SAVE_COPY, MF_BYCOMMAND | MF_GRAYED);
    DrawMenuBar(g_window);
    SetWindowTextW(g_window, L"OCCEdit");
    if (g_report) SetWindowTextW(g_report, L"");
    setStatus(L"Archivo cerrado. Abre un OCC para comenzar.");
}

bool openDocument(HWND owner, const wchar_t* initialPath, bool skipDiscardConfirmation,
                  const Document* expected, const std::vector<uint8_t>* expectedRtf) {
    if (!skipDiscardConfirmation && !confirmDiscard(owner)) return false;
    wchar_t path[MAX_PATH]{};
    if (initialPath && *initialPath) {
        if (wcslen(initialPath) >= MAX_PATH) {
            MessageBoxW(owner, L"La ruta del archivo es demasiado larga.", L"No se pudo abrir", MB_OK | MB_ICONERROR);
            return false;
        }
        wcscpy_s(path, initialPath);
        const size_t pathLength = wcslen(path);
        if (pathLength >= 2 && path[0] == L'"' && path[pathLength - 1] == L'"') {
            memmove(path, path + 1, (pathLength - 2) * sizeof(wchar_t));
            path[pathLength - 2] = L'\0';
        }
    } else {
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Archivos OMICRON OCC (*.occ)\0*.occ\0Todos los archivos (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Abrir archivo OCC";
    if (!GetOpenFileNameW(&dialog)) return false;
    }

    auto next = std::make_unique<Document>();
    next->path = path;
    const HRESULT hr = StgOpenStorage(
        path, nullptr, STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, &next->root);
    if (FAILED(hr)) {
        MessageBoxW(owner,
            L"Windows no pudo abrir el contenedor estructurado del archivo OCC.",
            L"No se pudo abrir", MB_OK | MB_ICONERROR);
        return false;
    }
    const HRESULT walkHr = enumerateStorage(next->root, 0, next->entries);
    if (FAILED(walkHr)) {
        MessageBoxW(owner,
            L"No se pudo leer la estructura completa del archivo OCC.",
            L"Formato no reconocido", MB_OK | MB_ICONWARNING);
        return false;
    }
    std::wstring readError;
    if (!readStream(next->root, L"RTF", next->rtf, readError)) {
        MessageBoxW(owner, readError.c_str(), L"No se pudo leer el informe", MB_OK | MB_ICONERROR);
        return false;
    }
    for (const auto& entry : next->entries) {
        if (!entry.storage || entry.depth != 0) continue;
        IStorage* embedded = nullptr;
        if (FAILED(next->root->OpenStorage(entry.name.c_str(), nullptr,
                STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &embedded))) continue;
        std::vector<uint8_t> packedQuick;
        std::wstring quickError;
        if (readRawStream(embedded, L"Contents", packedQuick, quickError)) {
            std::string memberName;
            std::vector<uint8_t> decoded;
            sevenzip_codec::FileMetadata metadata;
            if (sevenzip_codec::ExtractSingleLzma2(packedQuick, memberName, decoded,
                                                   &metadata, &quickError)) {
                next->quickCmcSupported = true;
                next->quickCmcName = std::move(memberName);
                next->quickCmc = std::move(decoded);
                next->quickCmcStorageName = entry.name;
                next->quickCmcMetadata = metadata;
                embedded->Release();
                break;
            }
        }
        embedded->Release();
    }

    if (next->quickCmcSupported) {
        IStorage* child = nullptr;
        if (SUCCEEDED(next->root->OpenStorage(next->quickCmcStorageName.c_str(),nullptr,
                STGM_READ | STGM_SHARE_EXCLUSIVE,nullptr,0,&child))) {
            quick_results::Bytes packed, info;
            const std::wstring module = L"QuickCMC";
            auto first = reinterpret_cast<const uint8_t*>(module.data());
            if (readRawStream(child,L"TestModuleInfo",info,readError) &&
                std::search(info.begin(),info.end(),first,first+module.size()*sizeof(wchar_t)) != info.end() &&
                readStream(next->root,L"Contents",next->summary,readError) &&
                readRawStream(child,L"RTF",packed,readError) &&
                sevenzip_codec::ExtractSingleLzma2(packed,next->reportName,next->quickReport,&next->reportMetadata,&readError))
                quick_results::Parse(next->summary,next->quickCmc,next->quickReport,next->results);
            child->Release();
        }
    }
    // Validación semántica posterior a la escritura: el OCC debe conservar los datos
    // editados de QuickCMC, el texto libre y las tablas modificadas.
    if (expected &&
        (next->quickCmcSupported != expected->quickCmcSupported ||
         next->summary != expected->summary ||
         next->quickCmc != expected->quickCmc ||
         next->quickReport != expected->quickReport ||
         next->results.rows.size() != expected->results.rows.size())) {
        MessageBoxW(owner, L"La copia OCC no reproduce los datos QuickCMC editados.",
                    L"Verificación fallida", MB_OK | MB_ICONERROR);
        return false;
    }
    if (expectedRtf && next->rtf != *expectedRtf) {
        MessageBoxW(owner, L"La copia OCC no conserva el texto del informe editado.",
                    L"Verificación fallida", MB_OK | MB_ICONERROR);
        return false;
    }
    if (expected) {
        for (const auto& [module, report] : expected->reports) {
            if (!report.dirty) continue;
            auto* actual = reportModule(*next, module);
            if (!actual || actual->data != report.data) {
                MessageBoxW(owner, L"La copia OCC no conserva todas las celdas editadas.",
                            L"Verificación fallida", MB_OK | MB_ICONERROR);
                return false;
            }
        }
    }
    next->originalSummary = next->summary;
    next->originalStates = next->quickCmc;
    next->originalQuickReport = next->quickReport;
    g_document = std::move(next);
    EnableWindow(g_saveButton,TRUE);
    EnableWindow(g_closeButton,TRUE);
    fillResults(0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& entry : g_document->entries) {
        const std::wstring line = displayEntry(entry);
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    showReport();
    showFreeText(g_document->rtf);
    EnableMenuItem(GetMenu(g_window), ID_FILE_CLOSE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(GetMenu(g_window), ID_FILE_SAVE_COPY, MF_BYCOMMAND | MF_ENABLED);
    DrawMenuBar(g_window);
    const std::wstring title = L"OCCEdit · " + g_document->path;
    SetWindowTextW(g_window, title.c_str());
    setStatus(L"Haz doble clic sobre el texto de una celda del informe para editarla.");
    return true;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_uiFont = CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_titleFont = CreateFontW(-29,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
            12, 12, 700, 440, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FILE_LIST)),
            GetModuleHandleW(nullptr), nullptr);
        g_report = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
            724, 12, 700, 440, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REPORT)),
            GetModuleHandleW(nullptr), nullptr);
        g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            724, 12, 700, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TABS)),
            GetModuleHandleW(nullptr), nullptr);
        g_freeText = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
            724, 44, 700, 408, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FREE_TEXT)),
            GetModuleHandleW(nullptr), nullptr);
        if (g_tabs) {
            TCITEMW tab{};
            tab.mask = TCIF_TEXT;
            tab.pszText = const_cast<LPWSTR>(L"Vista del informe");
            TabCtrl_InsertItem(g_tabs, 0, &tab);
            tab.pszText = const_cast<LPWSTR>(L"Texto libre");
            TabCtrl_InsertItem(g_tabs, 1, &tab);
            tab.pszText = const_cast<LPWSTR>(L"QuickCMC");
            tab.pszText = const_cast<LPWSTR>(L"Resultados QuickCMC");
            TabCtrl_InsertItem(g_tabs, 2, &tab);
            tab.pszText = const_cast<LPWSTR>(L"Estructura");
            TabCtrl_InsertItem(g_tabs, 3, &tab);
            TabCtrl_SetPadding(g_tabs,18,8);
        }
        auto control = [&](const wchar_t* klass, const wchar_t* label, DWORD style, int id) {
            HWND handle = CreateWindowExW(0,klass,label,WS_CHILD | style,0,0,0,0,window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(g_uiFont),TRUE);
            return handle;
        };
        g_title = control(L"STATIC",L"OCCEdit",WS_VISIBLE | SS_LEFT,0);
        SendMessageW(g_title,WM_SETFONT,reinterpret_cast<WPARAM>(g_titleFont),TRUE);
        g_openButton = control(L"BUTTON",L"Abrir OCC…",WS_VISIBLE | WS_TABSTOP,ID_FILE_OPEN);
        g_closeButton = control(L"BUTTON",L"Cerrar archivo",WS_VISIBLE | WS_TABSTOP,ID_FILE_CLOSE);
        g_saveButton = control(L"BUTTON",L"Guardar como copia…",WS_VISIBLE | WS_TABSTOP,ID_FILE_SAVE_COPY);
        EnableWindow(g_closeButton,FALSE);
        EnableWindow(g_saveButton,FALSE);
        g_results = control(L"COMBOBOX",L"",CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,2100);
        g_apply = control(L"BUTTON",L"Aplicar cambios",BS_PUSHBUTTON | WS_TABSTOP,2101);
        const wchar_t* labels[] = {L"Título",L"Comentario",L"Entrada CC (V)",L"Entrada CC (A)"};
        for (size_t i = 0; i < g_fields.size(); ++i) {
            g_labels[i] = control(L"STATIC",labels[i],SS_LEFT,0);
            g_fields[i] = control(L"EDIT",L"",WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,0);
            SendMessageW(g_fields[i],EM_LIMITTEXT,254,0);
        }
        fillResults(-1);
        g_status = CreateWindowExW(0, L"STATIC",
            L"Abre un archivo OCC para ver el informe y sus resultados.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 460, 900, 40, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), GetModuleHandleW(nullptr), nullptr);
        if (!g_list || !g_report || !g_freeText || !g_tabs || !g_status) return -1;
        const HWND styled[] = {g_list,g_tabs,g_status,g_freeText,g_report};
        for (HWND handle : styled) SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(g_uiFont),TRUE);
        SendMessageW(g_report, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
        SetWindowSubclass(g_report,reportInput,1,0);
        SendMessageW(g_report, EM_SETREADONLY, TRUE, 0);
        SendMessageW(g_freeText, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
        SendMessageW(g_freeText, EM_SETMODIFY, FALSE, 0);
        selectTab(0);
        return 0;
    }
    case WM_SIZE: {
        const int width = LOWORD(lParam), height = HIWORD(lParam);
        constexpr int inset = 22, top = 113;
        const int content = std::max(180,width-2*inset);
        MoveWindow(g_title,inset,20,190,42,TRUE);
        MoveWindow(g_saveButton,width-inset-174,22,174,34,TRUE);
        MoveWindow(g_closeButton,width-inset-304,22,120,34,TRUE);
        MoveWindow(g_openButton,width-inset-428,22,114,34,TRUE);
        MoveWindow(g_tabs,inset,72,content,40,TRUE);
        for (HWND panel : {g_list,g_report,g_freeText})
            MoveWindow(panel,inset,top,content,std::max(60,height-top-44),TRUE);
        RECT page{};
        GetClientRect(g_report,&page);
        page.left += 24; page.top += 13; page.right -= 24; page.bottom -= 10;
        SendMessageW(g_report,EM_SETRECT,0,reinterpret_cast<LPARAM>(&page));
        const int formWidth = std::min(content-48,760);
        MoveWindow(g_results,inset+24,top+25,formWidth,240,TRUE);
        for (size_t i = 0; i < g_fields.size(); ++i) {
            const int y = top+78+static_cast<int>(i)*46;
            MoveWindow(g_labels[i],inset+24,y+5,150,30,TRUE);
            MoveWindow(g_fields[i],inset+174,y,std::max(40,formWidth-150),29,TRUE);
        }
        MoveWindow(g_apply,inset+174,top+270,165,34,TRUE);
        MoveWindow(g_status,inset,height-32,content,26,TRUE);
        if (g_document && TabCtrl_GetCurSel(g_tabs)==0) SetTimer(window,1,160,nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wParam==1) {
            KillTimer(window,1);
            if (g_document && TabCtrl_GetCurSel(g_tabs)==0) showReport();
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* size = reinterpret_cast<MINMAXINFO*>(lParam);
        size->ptMinTrackSize.x = 675;
        size->ptMinTrackSize.y = 500;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 2100:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int selected = static_cast<int>(SendMessageW(g_results,CB_GETCURSEL,0,0));
                if (applyResult(window)) fillResults(selected);
                else SendMessageW(g_results,CB_SETCURSEL,g_selectedResult,0);
            }
            return 0;
        case 2101:
            if (applyResult(window)) { fillResults(g_selectedResult); setStatus(L"Cambios aplicados. Guarda una copia para escribirlos en disco."); }
            return 0;
        case ID_FILE_OPEN: openDocument(window); return 0;
        case ID_FILE_CLOSE: closeDocument(window); return 0;
        case ID_FILE_SAVE_COPY: saveCopy(window); return 0;
        case ID_FILE_EXIT: DestroyWindow(window); return 0;
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == ID_TABS &&
            reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE) {
            selectTab(TabCtrl_GetCurSel(g_tabs));
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'O') openDocument(window);
        else if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'W') closeDocument(window);
        return 0;
    case WM_CLOSE:
        if (confirmDiscard(window)) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_document.reset();
        DeleteObject(g_uiFont);
        DeleteObject(g_titleFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplusInput{}; ULONG_PTR gdiplusToken{};
    if (Gdiplus::GdiplusStartup(&gdiplusToken,&gdiplusInput,nullptr) != Gdiplus::Ok) { if (SUCCEEDED(com)) CoUninitialize(); return 1; }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    g_richEditModule = LoadLibraryW(L"Msftedit.dll");
    if (!g_richEditModule) { Gdiplus::GdiplusShutdown(gdiplusToken); if (SUCCEEDED(com)) CoUninitialize(); return 1; }

    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.hInstance = instance;
    klass.lpfnWndProc = windowProc;
    klass.lpszClassName = L"EditorOCCNativeWindow";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&klass)) {
        FreeLibrary(g_richEditModule);
        Gdiplus::GdiplusShutdown(gdiplusToken); if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }

    g_window = CreateWindowExW(0, klass.lpszClassName, L"OCCEdit",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1020, 760,
        nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        FreeLibrary(g_richEditModule);
        Gdiplus::GdiplusShutdown(gdiplusToken); if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);
    if (commandLine && *commandLine) openDocument(g_window, commandLine);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    if (g_richEditModule) FreeLibrary(g_richEditModule);
    return static_cast<int>(message.wParam);
}
