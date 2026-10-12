#include "wmi.hpp"

#include <combaseapi.h>
#include <oleauto.h>

#include <sstream>

namespace {

struct BstrHolder {
    BSTR value = nullptr;
    explicit BstrHolder(const wchar_t* text) : value(SysAllocString(text)) {}
    explicit BstrHolder(const std::wstring& text) : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    ~BstrHolder() { SysFreeString(value); }
    operator BSTR() const { return value; }
};

std::wstring NumberToString(const VARIANT& v) {
    std::wostringstream out;
    switch (v.vt) {
    case VT_I1: out << static_cast<int>(v.cVal); break;
    case VT_UI1: out << static_cast<unsigned>(v.bVal); break;
    case VT_I2: out << v.iVal; break;
    case VT_UI2: out << v.uiVal; break;
    case VT_I4:
    case VT_INT: out << v.lVal; break;
    case VT_UI4:
    case VT_UINT: out << v.ulVal; break;
    case VT_I8: out << v.llVal; break;
    case VT_UI8: out << v.ullVal; break;
    case VT_R4: out << v.fltVal; break;
    case VT_R8: out << v.dblVal; break;
    default: return L"";
    }
    return out.str();
}

} // namespace

WmiClient::WmiClient(const std::wstring& computer) {
    status_ = CoCreateInstance(
        CLSID_WbemLocator,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        reinterpret_cast<void**>(&locator_));

    if (FAILED(status_)) {
        return;
    }

    std::wstring ns = L"ROOT\\CIMV2";
    if (!computer.empty() && computer != L"." && computer != L"localhost") {
        ns = L"\\\\" + computer + L"\\ROOT\\CIMV2";
    }

    BstrHolder namespaceName(ns);
    status_ = locator_->ConnectServer(
        namespaceName,
        nullptr,
        nullptr,
        nullptr,
        0,
        nullptr,
        nullptr,
        &services_);

    if (FAILED(status_)) {
        return;
    }

    status_ = CoSetProxyBlanket(
        services_,
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE);
}

WmiClient::~WmiClient() {
    if (services_) services_->Release();
    if (locator_) locator_->Release();
}

std::wstring WmiClient::variantToString(const VARIANT& value) {
    if (value.vt == VT_EMPTY || value.vt == VT_NULL) return L"";
    if (value.vt == VT_BSTR) return value.bstrVal ? std::wstring(value.bstrVal, SysStringLen(value.bstrVal)) : L"";
    if (value.vt == VT_BOOL) return value.boolVal == VARIANT_TRUE ? L"Yes" : L"No";

    std::wstring numeric = NumberToString(value);
    if (!numeric.empty()) return numeric;

    if ((value.vt & VT_ARRAY) != 0) {
        SAFEARRAY* array = value.parray;
        if (!array || SafeArrayGetDim(array) != 1) return L"[array]";

        LONG lower = 0, upper = -1;
        if (FAILED(SafeArrayGetLBound(array, 1, &lower)) || FAILED(SafeArrayGetUBound(array, 1, &upper))) {
            return L"[array]";
        }

        std::wostringstream out;
        VARTYPE base = static_cast<VARTYPE>(value.vt & VT_TYPEMASK);
        for (LONG i = lower; i <= upper; ++i) {
            if (i != lower) out << L", ";
            if (base == VT_BSTR) {
                BSTR item = nullptr;
                if (SUCCEEDED(SafeArrayGetElement(array, &i, &item))) {
                    if (item) out << item;
                    SysFreeString(item);
                }
            } else {
                out << L"[item]";
            }
        }
        return out.str();
    }

    VARIANT converted;
    VariantInit(&converted);
    if (SUCCEEDED(VariantChangeType(&converted, const_cast<VARIANT*>(&value), 0, VT_BSTR))) {
        std::wstring result = converted.bstrVal ? std::wstring(converted.bstrVal) : L"";
        VariantClear(&converted);
        return result;
    }
    VariantClear(&converted);
    return L"";
}

std::vector<WmiRow> WmiClient::query(
    const std::wstring& wql,
    const std::vector<std::wstring>& properties,
    size_t maxRows,
    DWORD totalTimeoutMs,
    const std::function<bool()>& cancelled,
    bool* truncated) const {

    std::vector<WmiRow> rows;
    if (truncated) *truncated = false;
    if (!services_) return rows;
    if (cancelled && cancelled()) return rows;

    BstrHolder language(L"WQL");
    BstrHolder queryText(wql);
    IEnumWbemClassObject* enumerator = nullptr;

    HRESULT hr = services_->ExecQuery(
        language,
        queryText,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &enumerator);

    if (FAILED(hr) || !enumerator) return rows;

    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (cancelled && cancelled()) break;
        if (totalTimeoutMs != INFINITE && GetTickCount64() - started >= totalTimeoutMs) {
            if (truncated) *truncated = true;
            break;
        }
        if (rows.size() >= maxRows) {
            if (truncated) *truncated = true;
            break;
        }

        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        // Never wait indefinitely for a WMI provider.  This is especially
        // important for large/slow classes such as Win32_ClassicCOMClassSetting
        // and Win32_NTLogEvent.
        hr = enumerator->Next(250, 1, &object, &returned);
        if (hr == WBEM_S_TIMEDOUT && returned == 0) continue;
        if (FAILED(hr) || returned == 0 || !object) break;

        WmiRow row;
        for (const auto& property : properties) {
            VARIANT value;
            VariantInit(&value);
            HRESULT getHr = object->Get(property.c_str(), 0, &value, nullptr, nullptr);
            row.values.emplace_back(property, SUCCEEDED(getHr) ? variantToString(value) : L"");
            VariantClear(&value);
        }
        rows.push_back(std::move(row));
        object->Release();
    }

    enumerator->Release();
    return rows;
}
