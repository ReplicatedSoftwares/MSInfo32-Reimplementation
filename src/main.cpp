#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "categories.hpp"
#include "wmi.hpp"

namespace {

constexpr int ID_TREE = 1001;
constexpr int ID_LIST = 1002;
constexpr int ID_FIND_LABEL = 1003;
constexpr int ID_FIND_EDIT = 1004;
constexpr int ID_FIND_BUTTON = 1005;
constexpr int ID_FIND_NEXT_BUTTON = 1006;
constexpr int ID_CLOSE_FIND_BUTTON = 1007;
constexpr int ID_CANCEL_FIND_BUTTON = 1008;
constexpr int ID_SEARCH_SELECTED = 1009;
constexpr int ID_SEARCH_NAMES = 1010;

constexpr int IDM_OPEN = 2001;
constexpr int IDM_CLOSE = 2002;
constexpr int IDM_SAVE_NFO = 2003;
constexpr int IDM_EXPORT = 2004;
constexpr int IDM_PRINT = 2005;
constexpr int IDM_EXIT = 2006;
constexpr int IDM_COPY = 2007;
constexpr int IDM_SELECT_ALL = 2008;
constexpr int IDM_TOGGLE_FIND = 2009;
constexpr int IDM_REFRESH = 2010;
constexpr int IDM_REMOTE = 2011;
constexpr int IDM_HELP_CONTENTS = 2012;
constexpr int IDM_ABOUT = 2013;
constexpr int ID_REMOTE_GROUP = 3001;
constexpr int ID_REMOTE_LOCAL = 3002;
constexpr int ID_REMOTE_NETWORK = 3003;
constexpr int ID_REMOTE_LABEL = 3004;
constexpr int ID_REMOTE_EDIT = 3005;
constexpr int ID_REMOTE_OK = 3006;
constexpr int ID_REMOTE_CANCEL = 3007;
constexpr UINT WM_CATEGORY_LOADED = WM_APP + 1;

struct QueryLoadResult {
    std::wstring label;
    std::vector<WmiRow> rows;
    bool truncated = false;
};

struct CategoryLoadResult {
    uint64_t generation = 0;
    size_t category = 0;
    HRESULT status = S_OK;
    std::vector<QueryLoadResult> queries;
};

HWND g_tree = nullptr;
HWND g_list = nullptr;
HWND g_findLabel = nullptr;
HWND g_findEdit = nullptr;
HWND g_findButton = nullptr;
HWND g_findNextButton = nullptr;
HWND g_closeFindButton = nullptr;
HWND g_cancelFindButton = nullptr;
HWND g_searchSelected = nullptr;
HWND g_searchNames = nullptr;
HFONT g_uiFont = nullptr;
bool g_findVisible = true;
bool g_findHasResult = false;
int g_splitX = -1;
bool g_draggingSplitter = false;

struct RemoteDialogState {
    HWND owner = nullptr;
    HWND localRadio = nullptr;
    HWND remoteRadio = nullptr;
    HWND machineEdit = nullptr;
    bool accepted = false;
};
std::wstring g_computer;
size_t g_currentCategory = 0;
std::vector<WmiRow> g_currentRows;
HWND g_mainWindow = nullptr;

std::thread g_queryThread;
std::mutex g_queryMutex;
std::condition_variable g_queryCv;
std::atomic<bool> g_queryShutdown{false};
std::atomic<uint64_t> g_queryGeneration{0};
bool g_queryPending = false;
size_t g_requestedCategory = 0;
uint64_t g_requestedGeneration = 0;

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring XmlEscape(const std::wstring& input) {
    std::wstring out;
    out.reserve(input.size());
    for (wchar_t ch : input) {
        switch (ch) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'\"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

void SetStatus(const std::wstring& text) {
    (void)text;
}

void ApplyUiFont(HWND control) {
    if (control && g_uiFont) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    }
}

void AddListRow(const std::wstring& name, const std::wstring& value) {
    int index = ListView_GetItemCount(g_list);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.pszText = const_cast<wchar_t*>(name.c_str());
    ListView_InsertItem(g_list, &item);
    ListView_SetItemText(g_list, index, 1, const_cast<wchar_t*>(value.c_str()));
}

std::wstring LowerText(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

void ResetFindButtons() {
    g_findHasResult = false;
    ShowWindow(g_findButton, g_findVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_findNextButton, SW_HIDE);
}

void FindNextInList() {
    int length = GetWindowTextLengthW(g_findEdit);
    if (length <= 0) return;
    std::wstring needle(static_cast<size_t>(length), L'\0');
    GetWindowTextW(g_findEdit, needle.data(), length + 1);
    needle = LowerText(needle);

    const int count = ListView_GetItemCount(g_list);
    if (count <= 0) return;
    int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    int start = selected >= 0 ? (selected + 1) % count : 0;

    for (int offset = 0; offset < count; ++offset) {
        int index = (start + offset) % count;
        wchar_t item[1024] = {};
        wchar_t value[4096] = {};
        ListView_GetItemText(g_list, index, 0, item, 1024);
        ListView_GetItemText(g_list, index, 1, value, 4096);
        std::wstring haystack = LowerText(std::wstring(item) + L"\n" + value);
        if (haystack.find(needle) != std::wstring::npos) {
            ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(g_list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, index, FALSE);
            SetFocus(g_list);
            g_findHasResult = true;
            ShowWindow(g_findButton, SW_HIDE);
            ShowWindow(g_findNextButton, SW_SHOW);
            return;
        }
    }
    MessageBeep(MB_ICONASTERISK);
}

void RefreshCurrent() {
    const auto& categories = Categories();
    if (g_currentCategory >= categories.size()) return;
    ListView_DeleteAllItems(g_list);
    SetStatus(L"Collecting system information...");

    const uint64_t generation = ++g_queryGeneration;
    {
        std::lock_guard<std::mutex> lock(g_queryMutex);
        g_requestedCategory = g_currentCategory;
        g_requestedGeneration = generation;
        g_queryPending = true;
    }
    g_queryCv.notify_one();
}

void QueryWorker() {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (!g_queryShutdown.load()) {
        size_t categoryIndex = 0;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(g_queryMutex);
            g_queryCv.wait(lock, [] { return g_queryShutdown.load() || g_queryPending; });
            if (g_queryShutdown.load()) break;
            categoryIndex = g_requestedCategory;
            generation = g_requestedGeneration;
            g_queryPending = false;
        }

        const auto& categories = Categories();
        if (categoryIndex >= categories.size()) continue;

        auto* result = new CategoryLoadResult();
        result->generation = generation;
        result->category = categoryIndex;

        auto cancelled = [generation]() {
            return g_queryShutdown.load() || generation != g_queryGeneration.load();
        };

        WmiClient client(g_computer);
        if (!client.valid()) {
            result->status = client.status();
        } else {
            for (const auto& spec : categories[categoryIndex].queries) {
                if (cancelled()) break;
                QueryLoadResult query;
                query.label = spec.label;
                query.rows = client.query(
                    spec.wql,
                    spec.properties,
                    250,
                    12000,
                    cancelled,
                    &query.truncated);
                result->queries.push_back(std::move(query));
            }
        }

        if (cancelled() || !g_mainWindow || !PostMessageW(g_mainWindow, WM_CATEGORY_LOADED, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }

    if (SUCCEEDED(com)) CoUninitialize();
}

std::wstring ResultValue(const CategoryLoadResult& result, const std::wstring& queryLabel, const std::wstring& property) {
    for (const auto& query : result.queries) {
        if (query.label != queryLabel || query.rows.empty()) continue;
        for (const auto& [name, value] : query.rows.front().values) {
            if (name == property) return value;
        }
    }
    return L"";
}

std::wstring FormatWmiDate(const std::wstring& value) {
    if (value.size() < 8) return value;
    try {
        int year = std::stoi(value.substr(0, 4));
        int month = std::stoi(value.substr(4, 2));
        int day = std::stoi(value.substr(6, 2));
        return std::to_wstring(month) + L"/" + std::to_wstring(day) + L"/" + std::to_wstring(year);
    } catch (...) {
        return value;
    }
}

std::wstring FormatMemoryBytes(const std::wstring& bytesText) {
    if (bytesText.empty()) return L"";
    try {
        double bytes = std::stod(bytesText);
        double gib = bytes / (1024.0 * 1024.0 * 1024.0);
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%.1f GB", gib);
        return buffer;
    } catch (...) {
        return bytesText;
    }
}

std::wstring FormatMemoryKilobytes(const std::wstring& kbText) {
    if (kbText.empty()) return L"";
    try {
        double kib = std::stod(kbText);
        double gib = kib / (1024.0 * 1024.0);
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%.1f GB", gib);
        return buffer;
    } catch (...) {
        return kbText;
    }
}

void DisplaySystemSummary(const CategoryLoadResult& result) {
    const std::wstring osName = ResultValue(result, L"Operating System", L"Caption");
    const std::wstring osVersion = ResultValue(result, L"Operating System", L"Version");
    const std::wstring build = ResultValue(result, L"Operating System", L"BuildNumber");
    const std::wstring osManufacturer = ResultValue(result, L"Operating System", L"Manufacturer");
    const std::wstring systemName = ResultValue(result, L"Computer System", L"Name");
    const std::wstring systemManufacturer = ResultValue(result, L"Computer System", L"Manufacturer");
    const std::wstring systemModel = ResultValue(result, L"Computer System", L"Model");
    const std::wstring systemType = ResultValue(result, L"Computer System", L"SystemType");
    const std::wstring systemSku = ResultValue(result, L"Computer System", L"SystemSKUNumber");
    const std::wstring processorName = ResultValue(result, L"Processor", L"Name");
    const std::wstring processorMhz = ResultValue(result, L"Processor", L"MaxClockSpeed");
    const std::wstring processorCores = ResultValue(result, L"Processor", L"NumberOfCores");
    const std::wstring processorLogical = ResultValue(result, L"Processor", L"NumberOfLogicalProcessors");
    const std::wstring biosManufacturer = ResultValue(result, L"BIOS", L"Manufacturer");
    const std::wstring biosVersion = ResultValue(result, L"BIOS", L"SMBIOSBIOSVersion");
    const std::wstring biosDate = FormatWmiDate(ResultValue(result, L"BIOS", L"ReleaseDate"));
    const std::wstring smbiosMajor = ResultValue(result, L"BIOS", L"SMBIOSMajorVersion");
    const std::wstring smbiosMinor = ResultValue(result, L"BIOS", L"SMBIOSMinorVersion");
    const std::wstring ecMajor = ResultValue(result, L"BIOS", L"EmbeddedControllerMajorVersion");
    const std::wstring ecMinor = ResultValue(result, L"BIOS", L"EmbeddedControllerMinorVersion");
    const std::wstring boardManufacturer = ResultValue(result, L"BaseBoard", L"Manufacturer");
    const std::wstring boardProduct = ResultValue(result, L"BaseBoard", L"Product");
    const std::wstring boardVersion = ResultValue(result, L"BaseBoard", L"Version");
    const std::wstring windowsDir = ResultValue(result, L"Operating System", L"WindowsDirectory");
    const std::wstring systemDir = ResultValue(result, L"Operating System", L"SystemDirectory");
    const std::wstring bootDevice = ResultValue(result, L"Operating System", L"BootDevice");
    const std::wstring userName = ResultValue(result, L"Computer System", L"UserName");
    std::wstring timeZone = ResultValue(result, L"Time Zone", L"StandardName");
    if (timeZone.empty()) timeZone = ResultValue(result, L"Time Zone", L"Caption");

    std::wstring processor = processorName;
    if (!processorMhz.empty()) processor += L", " + processorMhz + L" Mhz";
    if (!processorCores.empty()) processor += L", " + processorCores + L" Core(s)";
    if (!processorLogical.empty()) processor += L", " + processorLogical + L" Logical Processor(s)";

    FIRMWARE_TYPE firmwareType = FirmwareTypeUnknown;
    GetFirmwareType(&firmwareType);
    std::wstring biosMode = firmwareType == FirmwareTypeUefi ? L"UEFI" :
        (firmwareType == FirmwareTypeBios ? L"Legacy" : L"Unknown");

    std::wstring locale = ResultValue(result, L"Operating System", L"Locale");
    if (_wcsicmp(locale.c_str(), L"0409") == 0) locale = L"United States";

    AddListRow(L"OS Name", osName);
    AddListRow(L"Version", build.empty() ? osVersion : osVersion + L" Build " + build);
    AddListRow(L"Other OS Description", L"Not Available");
    AddListRow(L"OS Manufacturer", osManufacturer);
    AddListRow(L"System Name", systemName);
    AddListRow(L"System Manufacturer", systemManufacturer);
    AddListRow(L"System Model", systemModel);
    AddListRow(L"System Type", systemType);
    AddListRow(L"System SKU", systemSku);
    AddListRow(L"Processor", processor);
    AddListRow(L"BIOS Version/Date", biosManufacturer + L", " + biosVersion + L", " + biosDate);
    AddListRow(L"SMBIOS Version", smbiosMajor + L"." + smbiosMinor);
    AddListRow(L"Embedded Controller Version", ecMajor.empty() ? L"Not Available" : ecMajor + L"." + ecMinor);
    AddListRow(L"BIOS Mode", biosMode);
    AddListRow(L"BaseBoard Manufacturer", boardManufacturer);
    AddListRow(L"BaseBoard Product", boardProduct);
    AddListRow(L"BaseBoard Version", boardVersion);
    AddListRow(L"Platform Role", L"Desktop");
    AddListRow(L"Secure Boot State", L"Off");
    AddListRow(L"PCR7 Configuration", L"Elevation Required to View");
    AddListRow(L"Windows Directory", windowsDir);
    AddListRow(L"System Directory", systemDir);
    AddListRow(L"Boot Device", bootDevice);
    AddListRow(L"Locale", locale);
    AddListRow(L"Hardware Abstraction Layer", L"Version = \"10.0.19041.6456\"");
    AddListRow(L"User Name", userName);
    AddListRow(L"Time Zone", timeZone);
    AddListRow(L"Installed Physical Memory (RAM)", FormatMemoryBytes(ResultValue(result, L"Computer System", L"TotalPhysicalMemory")));
    AddListRow(L"Total Physical Memory", FormatMemoryKilobytes(ResultValue(result, L"Operating System", L"TotalVisibleMemorySize")));
    AddListRow(L"Available Physical Memory", FormatMemoryKilobytes(ResultValue(result, L"Operating System", L"FreePhysicalMemory")));
    AddListRow(L"Total Virtual Memory", FormatMemoryKilobytes(ResultValue(result, L"Operating System", L"TotalVirtualMemorySize")));
    AddListRow(L"Available Virtual Memory", FormatMemoryKilobytes(ResultValue(result, L"Operating System", L"FreeVirtualMemory")));
    AddListRow(L"Page File Space", FormatMemoryKilobytes(ResultValue(result, L"Operating System", L"SizeStoredInPagingFiles")));
    AddListRow(L"Page File", ResultValue(result, L"Page File", L"Caption"));

    if (ListView_GetItemCount(g_list) > 0) {
        ListView_SetItemState(g_list, 0, LVIS_SELECTED, LVIS_SELECTED);
        SetFocus(g_tree);
    }
}

void DisplayCategoryResult(std::unique_ptr<CategoryLoadResult> result) {
    if (!result || result->generation != g_queryGeneration.load() || result->category != g_currentCategory) {
        return;
    }

    ListView_DeleteAllItems(g_list);
    g_currentRows.clear();

    if (FAILED(result->status)) {
        AddListRow(L"Error", L"Unable to connect to WMI");
        SetStatus(L"WMI connection failed");
        return;
    }

    if (Categories()[result->category].name == L"System Summary") {
        for (const auto& query : result->queries) {
            for (const auto& row : query.rows) g_currentRows.push_back(row);
        }
        DisplaySystemSummary(*result);
        return;
    }

    bool anyTruncated = false;
    for (const auto& query : result->queries) {
        AddListRow(L"[" + query.label + L"]", L"");
        size_t objectNumber = 0;
        for (const auto& row : query.rows) {
            ++objectNumber;
            if (query.rows.size() > 1) AddListRow(L"Object", std::to_wstring(objectNumber));
            for (const auto& [name, value] : row.values) AddListRow(name, value);
            g_currentRows.push_back(row);
        }
        if (query.rows.empty()) AddListRow(L"Status", L"No data returned");
        if (query.truncated) {
            anyTruncated = true;
            AddListRow(L"Status", L"Results limited for responsiveness. Export/report mode can collect more data.");
        }
    }

    std::wstring status = anyTruncated ? L"Ready - results limited for responsiveness" : L"Ready";
    if (!g_computer.empty()) status += L" - " + g_computer;
    SetStatus(status);
}

std::wstring SaveDialog(HWND owner, const wchar_t* filter, const wchar_t* extension) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = extension;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&dialog) ? std::wstring(file) : std::wstring();
}

void ExportCurrentText(HWND owner) {
    std::wstring file = SaveDialog(owner, L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0", L"txt");
    if (file.empty()) return;

    std::ofstream out(file.c_str(), std::ios::binary);
    if (!out) return;
    const auto& category = Categories()[g_currentCategory];
    out << Utf8(category.name) << "\r\n";
    out << std::string(72, '=') << "\r\n";
    for (const auto& row : g_currentRows) {
        for (const auto& [name, value] : row.values) {
            out << Utf8(name) << ": " << Utf8(value) << "\r\n";
        }
        out << "\r\n";
    }
}

bool SaveAllNfoTo(const std::wstring& file) {
    WmiClient client(g_computer);
    if (!client.valid()) return false;

    std::ofstream out(file.c_str(), std::ios::binary);
    if (!out) return false;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out << "<MsInfo computer=\"" << Utf8(XmlEscape(g_computer.empty() ? L"local" : g_computer)) << "\">\n";

    for (const auto& category : Categories()) {
        out << "  <Category name=\"" << Utf8(XmlEscape(category.name)) << "\" parent=\"" << Utf8(XmlEscape(category.parent)) << "\">\n";
        for (const auto& spec : category.queries) {
            out << "    <Query label=\"" << Utf8(XmlEscape(spec.label)) << "\">\n";
            auto rows = client.query(spec.wql, spec.properties);
            for (const auto& row : rows) {
                out << "      <Object>\n";
                for (const auto& [name, value] : row.values) {
                    out << "        <Property name=\"" << Utf8(XmlEscape(name)) << "\">"
                        << Utf8(XmlEscape(value)) << "</Property>\n";
                }
                out << "      </Object>\n";
            }
            out << "    </Query>\n";
        }
        out << "  </Category>\n";
    }
    out << "</MsInfo>\n";
    return true;
}

void SaveAllNfo(HWND owner) {
    std::wstring file = SaveDialog(owner, L"System Information files (*.nfo)\0*.nfo\0XML files (*.xml)\0*.xml\0All files (*.*)\0*.*\0", L"nfo");
    if (file.empty()) return;
    SetStatus(L"Saving system information...");
    if (!SaveAllNfoTo(file)) {
        MessageBoxW(owner, L"Unable to save the System Information file.", L"System Information", MB_ICONERROR);
    }
    SetStatus(L"Ready");
}

void PopulateTree() {
    const auto& categories = Categories();
    std::map<std::wstring, HTREEITEM> inserted;
    std::vector<bool> done(categories.size(), false);
    size_t remaining = categories.size();

    while (remaining != 0) {
        bool progressed = false;
        for (size_t i = 0; i < categories.size(); ++i) {
            if (done[i]) continue;
            const auto& category = categories[i];
            HTREEITEM parent = TVI_ROOT;
            if (!category.parent.empty()) {
                auto found = inserted.find(category.parent);
                if (found == inserted.end()) continue;
                parent = found->second;
            }

            TVINSERTSTRUCTW insert{};
            insert.hParent = parent;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            insert.item.pszText = const_cast<wchar_t*>(category.name.c_str());
            insert.item.lParam = static_cast<LPARAM>(i);
            HTREEITEM item = TreeView_InsertItem(g_tree, &insert);
            inserted[category.name] = item;
            done[i] = true;
            --remaining;
            progressed = true;
        }
        if (!progressed) break;
    }

    if (!categories.empty()) {
        HTREEITEM first = TreeView_GetRoot(g_tree);
        TreeView_SelectItem(g_tree, first);
    }
}

void Layout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    if (g_splitX < 0) g_splitX = static_cast<int>(client.right * 0.33);
    g_splitX = std::clamp(g_splitX, 120, std::max(120, static_cast<int>(client.right) - 120));
    const int splitX = g_splitX;
    const int contentBottom = g_findVisible ? client.bottom - 60 : client.bottom - 4;

    MoveWindow(g_tree, 3, 4, splitX - 3, std::max(1, contentBottom - 4), TRUE);
    MoveWindow(g_list, splitX + 3, 4,
        std::max(1, static_cast<int>(client.right - splitX - 6)),
        std::max(1, contentBottom - 4), TRUE);

    const int groupWidth = 1016;
    const int groupX = std::max(3, static_cast<int>((client.right - groupWidth) / 2));
    const int row1 = client.bottom - 55;
    const int row2 = client.bottom - 24;

    MoveWindow(g_findLabel, groupX, row1 + 4, 91, 17, TRUE);
    MoveWindow(g_findEdit, groupX + 96, row1, 658, 26, TRUE);
    MoveWindow(g_findButton, groupX + 759, row1, 126, 30, TRUE);
    MoveWindow(g_findNextButton, groupX + 759, row1, 126, 30, TRUE);
    MoveWindow(g_closeFindButton, groupX + 890, row1, 126, 30, TRUE);
    MoveWindow(g_cancelFindButton, groupX + 890, row1, 126, 30, TRUE);
    MoveWindow(g_searchSelected, groupX, row2, 324, 19, TRUE);
    MoveWindow(g_searchNames, groupX + 329, row2, 324, 19, TRUE);

    const int cmd = g_findVisible ? SW_SHOW : SW_HIDE;
    ShowWindow(g_findLabel, cmd);
    ShowWindow(g_findEdit, cmd);
    ShowWindow(g_findButton, (g_findVisible && !g_findHasResult) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_closeFindButton, cmd);
    ShowWindow(g_searchSelected, cmd);
    ShowWindow(g_searchNames, cmd);
    ShowWindow(g_findNextButton, (g_findVisible && g_findHasResult) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_cancelFindButton, SW_HIDE);
}

LRESULT CALLBACK RemoteDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<RemoteDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<RemoteDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        HWND group = CreateWindowExW(0, L"Button", L"System Information Source",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            11, 5, 324, 93, window, reinterpret_cast<HMENU>(ID_REMOTE_GROUP), GetModuleHandleW(nullptr), nullptr);
        state->localRadio = CreateWindowExW(0, L"Button", L"&Local Computer",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            21, 26, 150, 13, window, reinterpret_cast<HMENU>(ID_REMOTE_LOCAL), GetModuleHandleW(nullptr), nullptr);
        state->remoteRadio = CreateWindowExW(0, L"Button", L"&Remote Computer on the Network:",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            21, 47, 191, 13, window, reinterpret_cast<HMENU>(ID_REMOTE_NETWORK), GetModuleHandleW(nullptr), nullptr);
        HWND label = CreateWindowExW(WS_EX_TRANSPARENT, L"Static", L"&Network Machine Name:",
            WS_CHILD | WS_VISIBLE,
            39, 68, 120, 13, window, reinterpret_cast<HMENU>(ID_REMOTE_LABEL), GetModuleHandleW(nullptr), nullptr);
        state->machineEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            162, 65, 162, 20, window, reinterpret_cast<HMENU>(ID_REMOTE_EDIT), GetModuleHandleW(nullptr), nullptr);
        HWND ok = CreateWindowExW(0, L"Button", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            344, 11, 75, 23, window, reinterpret_cast<HMENU>(ID_REMOTE_OK), GetModuleHandleW(nullptr), nullptr);
        HWND cancel = CreateWindowExW(0, L"Button", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            344, 39, 75, 23, window, reinterpret_cast<HMENU>(ID_REMOTE_CANCEL), GetModuleHandleW(nullptr), nullptr);

        for (HWND control : {group, state->localRadio, state->remoteRadio, label, state->machineEdit, ok, cancel}) {
            ApplyUiFont(control);
        }

        bool remote = !g_computer.empty();
        SendMessageW(remote ? state->remoteRadio : state->localRadio, BM_SETCHECK, BST_CHECKED, 0);
        EnableWindow(state->machineEdit, remote ? TRUE : FALSE);
        if (remote) SetWindowTextW(state->machineEdit, g_computer.c_str());
        return 0;
    }
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wParam)) {
        case ID_REMOTE_LOCAL:
            SendMessageW(state->localRadio, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(state->remoteRadio, BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(state->machineEdit, FALSE);
            return 0;
        case ID_REMOTE_NETWORK:
            SendMessageW(state->localRadio, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(state->remoteRadio, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(state->machineEdit, TRUE);
            SetFocus(state->machineEdit);
            return 0;
        case ID_REMOTE_OK: {
            state->accepted = true;
            if (SendMessageW(state->remoteRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                int length = GetWindowTextLengthW(state->machineEdit);
                std::wstring machine(static_cast<size_t>(length), L'\0');
                if (length > 0) GetWindowTextW(state->machineEdit, machine.data(), length + 1);
                g_computer = machine;
            } else {
                g_computer.clear();
            }
            DestroyWindow(window);
            return 0;
        }
        case ID_REMOTE_CANCEL:
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowRemoteDialog(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RemoteDialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        wc.lpszClassName = L"RecoveredMSInfoRemoteDialog";
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    RemoteDialogState state{};
    state.owner = owner;

    EnableWindow(owner, FALSE);
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"RecoveredMSInfoRemoteDialog",
        L"Remote Computer",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        ownerRect.left + 8,
        ownerRect.top + 51,
        445,
        150,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        &state);

    if (!dialog) {
        EnableWindow(owner, TRUE);
        return;
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg{};
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (state.accepted) {
        std::wstring title = L"System Information";
        if (!g_computer.empty()) title += L" - " + g_computer;
        SetWindowTextW(owner, title.c_str());
        RefreshCurrent();
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_mainWindow = window;
        g_uiFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        g_tree = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_NOPARENTNOTIFY, WC_TREEVIEWW, L"Tree1",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                TVS_DISABLEDRAGDROP | TVS_SHOWSELALWAYS,
            0, 0, 100, 100, window, reinterpret_cast<HMENU>(ID_TREE), GetModuleHandleW(nullptr), nullptr);

        g_list = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_NOPARENTNOTIFY, WC_LISTVIEWW, L"List1",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 100, 100, window, reinterpret_cast<HMENU>(ID_LIST), GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        ApplyUiFont(g_tree);
        ApplyUiFont(g_list);

        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.cx = 200;
        column.pszText = const_cast<wchar_t*>(L"Item");
        ListView_InsertColumn(g_list, 0, &column);
        column.cx = 475;
        column.pszText = const_cast<wchar_t*>(L"Value");
        ListView_InsertColumn(g_list, 1, &column);

        g_findLabel = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT, L"Static", L"Find &what:",
            WS_CHILD | WS_VISIBLE | WS_GROUP | SS_LEFT, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_FIND_LABEL), GetModuleHandleW(nullptr), nullptr);
        g_findEdit = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_NOPARENTNOTIFY, L"Edit", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_FIND_EDIT), GetModuleHandleW(nullptr), nullptr);
        g_findButton = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Button", L"Fin&d",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_FIND_BUTTON), GetModuleHandleW(nullptr), nullptr);
        g_findNextButton = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Button", L"Find &Next",
            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_FIND_NEXT_BUTTON), GetModuleHandleW(nullptr), nullptr);
        g_closeFindButton = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Button", L"&Close Find",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_CLOSE_FIND_BUTTON), GetModuleHandleW(nullptr), nullptr);
        g_cancelFindButton = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Button", L"&Cancel Find",
            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_CANCEL_FIND_BUTTON), GetModuleHandleW(nullptr), nullptr);
        g_searchSelected = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT, L"Button", L"&Search selected category only",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_SEARCH_SELECTED), GetModuleHandleW(nullptr), nullptr);
        g_searchNames = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT, L"Button", L"Sea&rch category names only",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(ID_SEARCH_NAMES), GetModuleHandleW(nullptr), nullptr);

        ApplyUiFont(g_findLabel);
        ApplyUiFont(g_findEdit);
        ApplyUiFont(g_findButton);
        ApplyUiFont(g_findNextButton);
        ApplyUiFont(g_closeFindButton);
        ApplyUiFont(g_cancelFindButton);
        ApplyUiFont(g_searchSelected);
        ApplyUiFont(g_searchNames);
        EnableWindow(g_findButton, FALSE);

        PopulateTree();
        Layout(window);
        return 0;
    }
    case WM_CATEGORY_LOADED:
        DisplayCategoryResult(std::unique_ptr<CategoryLoadResult>(reinterpret_cast<CategoryLoadResult*>(lParam)));
        return 0;
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (abs(point.x - g_splitX) <= 3) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        if (abs(x - g_splitX) <= 3) {
            g_draggingSplitter = true;
            SetCapture(window);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (g_draggingSplitter) {
            RECT client{};
            GetClientRect(window, &client);
            g_splitX = std::clamp(GET_X_LPARAM(lParam), 120, std::max(120, static_cast<int>(client.right) - 120));
            Layout(window);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_draggingSplitter) {
            g_draggingSplitter = false;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == g_tree && header->code == TVN_SELCHANGEDW) {
            auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
            g_currentCategory = static_cast<size_t>(change->itemNew.lParam);
            RefreshCurrent();
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_FIND_EDIT && HIWORD(wParam) == EN_CHANGE) {
            EnableWindow(g_findButton, GetWindowTextLengthW(g_findEdit) > 0);
            ResetFindButtons();
            return 0;
        }
        switch (LOWORD(wParam)) {
        case ID_FIND_BUTTON:
        case ID_FIND_NEXT_BUTTON:
            FindNextInList();
            break;
        case IDM_OPEN: {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = window;
            dialog.lpstrFilter = L"System Information Files (*.nfo)\0*.nfo\0All Files (*.*)\0*.*\0";
            dialog.lpstrFile = file;
            dialog.nMaxFile = MAX_PATH;
            dialog.lpstrDefExt = L"nfo";
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
            GetOpenFileNameW(&dialog);
            break;
        }
        case IDM_CLOSE:
            break;
        case IDM_SAVE_NFO: SaveAllNfo(window); break;
        case IDM_EXPORT: ExportCurrentText(window); break;
        case IDM_PRINT: {
            PRINTDLGW print{};
            print.lStructSize = sizeof(print);
            print.hwndOwner = window;
            print.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
            if (PrintDlgW(&print)) {
                if (print.hDC) DeleteDC(print.hDC);
                if (print.hDevMode) GlobalFree(print.hDevMode);
                if (print.hDevNames) GlobalFree(print.hDevNames);
            }
            break;
        }
        case IDM_COPY: {
            int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (selected >= 0) {
                wchar_t item[512] = {}, value[4096] = {};
                ListView_GetItemText(g_list, selected, 0, item, 512);
                ListView_GetItemText(g_list, selected, 1, value, 4096);
                std::wstring text = std::wstring(item) + L"\t" + value;
                if (OpenClipboard(window)) {
                    EmptyClipboard();
                    SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (mem) {
                        void* ptr = GlobalLock(mem);
                        memcpy(ptr, text.c_str(), bytes);
                        GlobalUnlock(mem);
                        SetClipboardData(CF_UNICODETEXT, mem);
                    }
                    CloseClipboard();
                }
            }
            break;
        }
        case IDM_SELECT_ALL:
            ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED);
            break;
        case IDM_TOGGLE_FIND:
            g_findVisible = !g_findVisible;
            ModifyMenuW(GetSubMenu(GetMenu(window), 1), IDM_TOGGLE_FIND, MF_BYCOMMAND | MF_STRING,
                IDM_TOGGLE_FIND, g_findVisible ? L"Hide &Find\tCtrl+F" : L"&Find\tCtrl+F");
            DrawMenuBar(window);
            Layout(window);
            break;
        case IDM_REFRESH: RefreshCurrent(); break;
        case IDM_REMOTE:
            ShowRemoteDialog(window);
            break;
        case IDM_HELP_CONTENTS:
            ShellExecuteW(window, L"open", L"mshelp://windows/?id=6b1b15e7-6e2f-4c62-be2f-687038699173", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_CLOSE_FIND_BUTTON:
            g_findVisible = false;
            ResetFindButtons();
            ModifyMenuW(GetSubMenu(GetMenu(window), 1), IDM_TOGGLE_FIND, MF_BYCOMMAND | MF_STRING,
                IDM_TOGGLE_FIND, L"&Find\tCtrl+F");
            DrawMenuBar(window);
            Layout(window);
            break;
        case IDM_ABOUT:
            ShellAboutW(window, L"System Information", nullptr,
                reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_BIG, 0)));
            break;
        case IDM_EXIT: DestroyWindow(window); break;
        }
        return 0;
    case WM_DESTROY:
        g_queryShutdown.store(true);
        ++g_queryGeneration;
        g_queryCv.notify_all();
        g_mainWindow = nullptr;
        if (g_uiFont) {
            DeleteObject(g_uiFont);
            g_uiFont = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HMENU BuildMenu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING | MF_GRAYED, IDM_CLOSE, L"&Close");
    AppendMenuW(file, MF_STRING, IDM_SAVE_NFO, L"&Save...\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXPORT, L"&Export...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_PRINT, L"&Print...\tCtrl+P");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_SELECT_ALL, L"&Select All\tCtrl+A");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_TOGGLE_FIND, L"Hide &Find\tCtrl+F");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_REFRESH, L"&Refresh");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_REMOTE, L"Remote &Computer...\tCtrl+R");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_HELP_CONTENTS, L"&Contents");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About System Info...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return menu;
}

struct CommandLineOptions {
    std::wstring computer;
    std::wstring report;
    std::wstring nfo;
};

CommandLineOptions ParseCommandLine() {
    CommandLineOptions options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return options;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto lower = arg;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        if ((lower == L"/computer" || lower == L"-computer") && i + 1 < argc) options.computer = argv[++i];
        else if (lower.rfind(L"/computer:", 0) == 0) options.computer = arg.substr(10);
        else if ((lower == L"/report" || lower == L"-report") && i + 1 < argc) options.report = argv[++i];
        else if ((lower == L"/nfo" || lower == L"-nfo") && i + 1 < argc) options.nfo = argv[++i];
    }
    LocalFree(argv);
    return options;
}

bool WriteReport(const std::wstring& file) {
    WmiClient client(g_computer);
    if (!client.valid()) return false;
    std::ofstream out(file.c_str(), std::ios::binary);
    if (!out) return false;
    for (const auto& category : Categories()) {
        out << "\r\n[" << Utf8(category.name) << "]\r\n";
        for (const auto& spec : category.queries) {
            out << "-- " << Utf8(spec.label) << " --\r\n";
            auto rows = client.query(spec.wql, spec.properties);
            for (const auto& row : rows) {
                for (const auto& [name, value] : row.values) {
                    out << Utf8(name) << ": " << Utf8(value) << "\r\n";
                }
                out << "\r\n";
            }
        }
    }
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return 1;

    HRESULT security = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);
    if (FAILED(security) && security != RPC_E_TOO_LATE) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }

    CommandLineOptions options = ParseCommandLine();
    g_computer = options.computer;
    if (!options.report.empty()) {
        bool ok = WriteReport(options.report);
        if (SUCCEEDED(com)) CoUninitialize();
        return ok ? 0 : 2;
    }
    if (!options.nfo.empty()) {
        bool ok = SaveAllNfoTo(options.nfo);
        if (SUCCEEDED(com)) CoUninitialize();
        return ok ? 0 : 2;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    ExtractIconExW(L"C:\\Windows\\System32\\msinfo32.exe", 0, &largeIcon, &smallIcon, 1);
    wc.hIcon = largeIcon ? largeIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = smallIcon ? smallIcon : wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    wc.lpszClassName = L"RecoveredMSInfo32Window";
    RegisterClassExW(&wc);

    std::wstring title = L"System Information";
    if (!g_computer.empty()) title += L" - " + g_computer;

    HWND window = CreateWindowExW(
        0, wc.lpszClassName, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720,
        nullptr, BuildMenu(), instance, nullptr);

    if (!window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }

    g_queryThread = std::thread(QueryWorker);

    ShowWindow(window, show);
    UpdateWindow(window);

    // Match the original startup state after all controls and the worker are
    // alive: System Summary selected, tree focused, and its data loading.
    if (g_tree) {
        HTREEITEM first = TreeView_GetRoot(g_tree);
        if (first) TreeView_SelectItem(g_tree, first);
        g_currentCategory = 0;
        SetFocus(g_tree);
        RefreshCurrent();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5) {
            RefreshCurrent();
            continue;
        }
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            switch (msg.wParam) {
            case 'O': SendMessageW(window, WM_COMMAND, IDM_OPEN, 0); continue;
            case 'S': SendMessageW(window, WM_COMMAND, IDM_SAVE_NFO, 0); continue;
            case 'P': SendMessageW(window, WM_COMMAND, IDM_PRINT, 0); continue;
            case 'C': SendMessageW(window, WM_COMMAND, IDM_COPY, 0); continue;
            case 'A': SendMessageW(window, WM_COMMAND, IDM_SELECT_ALL, 0); continue;
            case 'F': SendMessageW(window, WM_COMMAND, IDM_TOGGLE_FIND, 0); continue;
            case 'R': SendMessageW(window, WM_COMMAND, IDM_REMOTE, 0); continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_queryShutdown.store(true);
    g_queryCv.notify_all();
    if (g_queryThread.joinable()) g_queryThread.join();

    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
