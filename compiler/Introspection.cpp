#include "Introspection.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "CallableInfo.h"
#include "core/common.h"

namespace roxal {

namespace {

std::string describeTypeAnnotation(const Value& value)
{
    if (value.isNil())
        return "any";

    if (isObjectType(value)) {
        auto* type = asObjectType(value);
        return toUTF8StdString(type->name);
    }

    if (isTypeSpec(value)) {
        auto* typeSpec = asTypeSpec(value);
        if (typeSpec->typeValue == ValueType::Object ||
            typeSpec->typeValue == ValueType::Actor) {
            auto* obj = asObjectType(value);
            return toUTF8StdString(obj->name);
        }
        return to_string(typeSpec->typeValue);
    }

    if (isModuleType(value)) {
        auto* module = asModuleType(value);
        if (module->name.isEmpty())
            return "module";
        return toUTF8StdString(module->name);
    }

    return value.typeName();
}

} // namespace

bool isCallableValue(const Value& value)
{
    return isClosure(value) || isFunction(value) ||
           isBoundMethod(value) || isBoundNative(value) ||
           isNative(value);
}

std::string describeValueType(const Value& value, std::string* docOut)
{
    if (docOut)
        docOut->clear();

    if (isCallableValue(value)) {
        CallableInfo info = describeCallable(value);
        if (docOut)
            *docOut = info.doc;
        if (info.signature.has_value())
            return info.signature.value();
        if (isNative(value))
            return "<native fn>";
        if (isBoundNative(value))
            return "<native method>";
        return "function";
    }

    if (isModuleType(value)) {
        auto* module = asModuleType(value);
        if (module->name.isEmpty())
            return "module";
        return std::string("module ") + toUTF8StdString(module->name);
    }

    if (isObjectInstance(value)) {
        auto* type = asObjectType(asObjectInstance(value)->instanceType);
        return std::string("object ") + toUTF8StdString(type->name);
    }

    if (isActorInstance(value)) {
        auto* type = asObjectType(asActorInstance(value)->instanceType);
        return std::string("actor ") + toUTF8StdString(type->name);
    }

    if (isObjectType(value)) {
        auto* type = asObjectType(value);
        return std::string("type ") + toUTF8StdString(type->name);
    }

    if (isTypeSpec(value)) {
        auto* typeSpec = asTypeSpec(value);
        if (typeSpec->typeValue == ValueType::Object ||
            typeSpec->typeValue == ValueType::Actor) {
            auto* obj = asObjectType(value);
            return std::string("type ") + toUTF8StdString(obj->name);
        }
        return to_string(typeSpec->typeValue);
    }

    return value.typeName();
}

std::vector<SymbolEntry> collectModuleEntries(ObjModuleType* module)
{
    std::vector<SymbolEntry> entries;
    if (!module)
        return entries;

    auto snapshot = module->vars.snapshot();
    entries.reserve(snapshot.size());
    for (const auto& nameValue : snapshot) {
        if (nameValue.first.isEmpty())
            continue;
        SymbolEntry entry;
        entry.name = toUTF8StdString(nameValue.first);
        entry.type = describeValueType(nameValue.second, &entry.doc);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const SymbolEntry& a, const SymbolEntry& b) {
                  return a.name < b.name;
              });
    return entries;
}

std::vector<SymbolEntry> collectPropertyEntries(ObjObjectType* type)
{
    std::vector<SymbolEntry> entries;
    if (!type)
        return entries;

    for (int32_t hash : type->propertyOrder) {
        auto it = type->properties.find(hash);
        if (it == type->properties.end())
            continue;
        const auto& property = it->second;
        if (property.access != ast::Access::Public)
            continue;

        SymbolEntry entry;
        entry.name = toUTF8StdString(property.name);
        entry.type = describeTypeAnnotation(property.type);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<SymbolEntry> collectMethodEntries(ObjObjectType* type)
{
    std::vector<SymbolEntry> entries;
    if (!type)
        return entries;

    for (const auto& kv : type->methods) {
        const auto& method = kv.second;
        if (method.access != ast::Access::Public)
            continue;

        SymbolEntry entry;
        entry.name = toUTF8StdString(method.name);
        if (method.closure.isNil()) {
            entry.type = "<abstract>";
        } else {
            entry.type = describeValueType(method.closure, &entry.doc);
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const SymbolEntry& a, const SymbolEntry& b) {
                  return a.name < b.name;
              });
    return entries;
}

std::string formatSymbolEntries(const std::vector<SymbolEntry>& entries,
                                size_t indent,
                                size_t maxLineLength,
                                const std::string& emptyPlaceholder)
{
    auto collapseWhitespace = [](const std::string& text) {
        std::string result;
        result.reserve(text.size());
        bool lastWasSpace = false;
        for (char ch : text) {
            char normalized = ch;
            if (ch == '\n' || ch == '\r' || ch == '\t')
                normalized = ' ';
            if (std::isspace(static_cast<unsigned char>(normalized))) {
                if (!lastWasSpace) {
                    result.push_back(' ');
                    lastWasSpace = true;
                }
            } else {
                result.push_back(normalized);
                lastWasSpace = false;
            }
        }
        return trim(result);
    };

    auto formatLine = [&](const SymbolEntry& entry) {
        std::string base = entry.name.empty()
                               ? entry.type
                               : entry.name + ": " + entry.type;
        if (entry.doc.empty())
            return base;

        std::string sanitized = collapseWhitespace(entry.doc);
        std::string line = base + " - " + sanitized;
        if (line.size() <= maxLineLength)
            return line;

        size_t baseline = base.size() + 3;
        if (maxLineLength <= baseline + 3)
            return base + " - ...";
        size_t allowed = maxLineLength - baseline - 3;
        if (allowed > sanitized.size())
            allowed = sanitized.size();
        std::string truncated = sanitized.substr(0, allowed);
        return base + " - " + truncated + "...";
    };

    std::ostringstream out;
    std::string prefix(indent, ' ');
    if (entries.empty()) {
        out << prefix << emptyPlaceholder << "\n";
        return out.str();
    }
    for (const auto& entry : entries)
        out << prefix << formatLine(entry) << "\n";
    return out.str();
}

} // namespace roxal
