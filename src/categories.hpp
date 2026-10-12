#pragma once

#include <string>
#include <vector>

struct QuerySpec {
    std::wstring label;
    std::wstring wql;
    std::vector<std::wstring> properties;
};

struct Category {
    std::wstring parent;
    std::wstring name;
    std::vector<QuerySpec> queries;
};

const std::vector<Category>& Categories();

