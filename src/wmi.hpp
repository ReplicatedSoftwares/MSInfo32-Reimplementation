#pragma once

#include <windows.h>
#include <wbemidl.h>

#include <string>
#include <functional>
#include <utility>
#include <vector>

struct WmiRow {
    std::vector<std::pair<std::wstring, std::wstring>> values;
};

class WmiClient {
public:
    explicit WmiClient(const std::wstring& computer = L"");
    ~WmiClient();

    WmiClient(const WmiClient&) = delete;
    WmiClient& operator=(const WmiClient&) = delete;

    bool valid() const { return services_ != nullptr; }
    HRESULT status() const { return status_; }

    std::vector<WmiRow> query(
        const std::wstring& wql,
        const std::vector<std::wstring>& properties,
        size_t maxRows = 500,
        DWORD totalTimeoutMs = 15000,
        const std::function<bool()>& cancelled = {},
        bool* truncated = nullptr) const;

    static std::wstring variantToString(const VARIANT& value);

private:
    IWbemLocator* locator_ = nullptr;
    IWbemServices* services_ = nullptr;
    HRESULT status_ = E_FAIL;
};
