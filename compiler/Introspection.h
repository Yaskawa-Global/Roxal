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
// maxEntries bounds the walk for bounded-work consumers (debugger previews);
// default collects everything.
std::vector<SymbolEntry> collectPropertyEntries(ObjObjectType* type,
                                                size_t maxEntries = (size_t)-1);
std::vector<SymbolEntry> collectMethodEntries(ObjObjectType* type);

std::string formatSymbolEntries(const std::vector<SymbolEntry>& entries,
                                size_t indent = 0,
                                size_t maxLineLength = 100,
                                const std::string& emptyPlaceholder = "<no symbols>");

} // namespace roxal
