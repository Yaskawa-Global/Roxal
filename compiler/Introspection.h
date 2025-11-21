#pragma once

#include <string>
#include <vector>

#include "Value.h"
#include "Object.h"

namespace roxal {

struct SymbolEntry {
    std::string name;
    std::string type;
    std::string doc;
};

bool isCallableValue(const Value& value);

std::string describeValueType(const Value& value, std::string* docOut = nullptr);

std::vector<SymbolEntry> collectModuleEntries(ObjModuleType* module);
std::vector<SymbolEntry> collectPropertyEntries(ObjObjectType* type);
std::vector<SymbolEntry> collectMethodEntries(ObjObjectType* type);

std::string formatSymbolEntries(const std::vector<SymbolEntry>& entries,
                                size_t indent = 0,
                                size_t maxLineLength = 100,
                                const std::string& emptyPlaceholder = "<no symbols>");

} // namespace roxal
