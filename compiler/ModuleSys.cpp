#include "ModuleSys.h"
#include "VM.h"
#include "Object.h"
#include "Chunk.h"
#include "SimpleMarkSweepGC.h"
#include "ThreadManager.h"
#include "core/AST.h"
#include <core/json5.h>
#include <core/TimePoint.h>
#include <core/TimeDuration.h>
#include "dataflow/Signal.h"
#include "dataflow/FuncNode.h"
#include "dataflow/DataflowEngine.h"
#include "FFI.h"
#include "Introspection.h"
#include "CallableInfo.h"
#ifdef ROXAL_ENABLE_XML
#include <pugixml.hpp>
#endif
#include <sstream>
#include <time.h>
#include <cmath>
#include <limits>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <vector>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <optional>
#include <stdexcept>
#include <future>
#include <atomic>
#include <memory>
#include <numeric>

using namespace roxal;

namespace {

constexpr int64_t MICROS_PER_SECOND = 1'000'000;
constexpr int64_t MICROS_PER_MILLISECOND = 1'000;
constexpr int64_t MICROS_PER_MINUTE = 60 * MICROS_PER_SECOND;
constexpr int64_t MICROS_PER_HOUR = 60 * MICROS_PER_MINUTE;
constexpr int64_t MICROS_PER_DAY = 24 * MICROS_PER_HOUR;

ObjObjectType* gSysTimeType = nullptr;
ObjObjectType* gSysTimeSpanType = nullptr;
ObjObjectType* gSysQuantityType = nullptr;

struct NormalizedParts {
    int32_t seconds;
    int32_t micros;
};

NormalizedParts normalizeMicros(int64_t totalMicros)
{
    int64_t seconds = totalMicros / MICROS_PER_SECOND;
    int64_t micros = totalMicros % MICROS_PER_SECOND;
    if (micros < 0) {
        micros += MICROS_PER_SECOND;
        --seconds;
    }
    if (seconds < std::numeric_limits<int32_t>::min() ||
        seconds > std::numeric_limits<int32_t>::max()) {
        throw std::out_of_range("time value exceeds 32-bit range");
    }
    return { static_cast<int32_t>(seconds), static_cast<int32_t>(micros) };
}

int64_t addChecked(int64_t total, int64_t delta)
{
    if (delta > 0 && total > std::numeric_limits<int64_t>::max() - delta)
        throw std::out_of_range("time span overflow");
    if (delta < 0 && total < std::numeric_limits<int64_t>::min() - delta)
        throw std::out_of_range("time span overflow");
    return total + delta;
}

int64_t durationFromFields(int64_t days, int64_t hours, int64_t minutes, int64_t seconds,
                           int64_t millis, int64_t micros)
{
    int64_t total = 0;
    total = addChecked(total, static_cast<int64_t>(days) * MICROS_PER_DAY);
    total = addChecked(total, static_cast<int64_t>(hours) * MICROS_PER_HOUR);
    total = addChecked(total, static_cast<int64_t>(minutes) * MICROS_PER_MINUTE);
    total = addChecked(total, static_cast<int64_t>(seconds) * MICROS_PER_SECOND);
    total = addChecked(total, static_cast<int64_t>(millis) * MICROS_PER_MILLISECOND);
    total = addChecked(total, static_cast<int64_t>(micros));
    return total;
}

std::string toLowerCopy(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

enum class ClockZone { Local, UTC };

ClockZone parseZone(const std::string& tz)
{
    auto lower = toLowerCopy(tz);
    if (lower == "local")
        return ClockZone::Local;
    if (lower == "utc" || lower == "gmt")
        return ClockZone::UTC;
    throw std::invalid_argument("unknown timezone '" + tz + "'");
}

enum class TimeKind { Wall, Steady };

TimeKind parseKind(const std::string& kind)
{
    auto lower = toLowerCopy(kind);
    if (lower == "wall")
        return TimeKind::Wall;
    if (lower == "steady")
        return TimeKind::Steady;
    throw std::invalid_argument("unknown time kind '" + kind + "'");
}

constexpr const char* XML_MODE_COMPACT = "compact";
constexpr const char* XML_MODE_RAW = "raw";
constexpr const char* XML_MODE_AUTO = "auto";

#ifdef ROXAL_ENABLE_XML

struct XmlNode;

struct XmlChild {
    bool isText { false };
    std::string text;
    std::unique_ptr<XmlNode> node;
};

struct XmlNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlChild> children;
};

Value xmlStringValue(const std::string& s)
{
    return Value::stringVal(toUnicodeString(s));
}

bool dictHasStringKey(const Value& dictValue, const char* key)
{
    if (!isDict(dictValue))
        return false;
    return asDict(dictValue)->contains(xmlStringValue(key));
}

std::optional<Value> dictGetStringKey(const Value& dictValue, const char* key)
{
    if (!isDict(dictValue))
        return std::nullopt;
    Value keyValue = xmlStringValue(key);
    if (!asDict(dictValue)->contains(keyValue))
        return std::nullopt;
    return asDict(dictValue)->at(keyValue);
}

void dictStoreStringKey(Value& dictValue, const char* key, const Value& value)
{
    asDict(dictValue)->store(xmlStringValue(key), value);
}

bool isWhitespaceOnly(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

std::pair<int, int> xmlLineColFromOffset(const std::string& text, ptrdiff_t offset)
{
    if (offset < 0)
        return {1, 1};

    size_t pos = static_cast<size_t>(offset);
    if (pos > text.size())
        pos = text.size();

    int line = 1;
    int col = 1;
    for (size_t i = 0; i < pos; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
    return {line, col};
}

std::string requireStringValue(const Value& value, const std::string& context)
{
    if (!isString(value))
        throw std::invalid_argument(context + " must be string");
    return toUTF8StdString(asStringObj(value)->s);
}

void requireExactKeys(const Value& dictValue,
                      const std::vector<std::string>& requiredKeys,
                      const std::string& context)
{
    if (!isDict(dictValue))
        throw std::invalid_argument(context + " must be dict");

    const auto items = asDict(dictValue)->items();
    if (items.size() != requiredKeys.size())
        throw std::invalid_argument(context + " must contain exactly keys tag, attrs, children");

    for (const auto& key : requiredKeys) {
        if (!dictHasStringKey(dictValue, key.c_str()))
            throw std::invalid_argument(context + " must contain exactly keys tag, attrs, children");
    }
}

bool isExactRawNodeShape(const Value& value)
{
    if (!isDict(value))
        return false;

    const auto items = asDict(value)->items();
    if (items.size() != 3)
        return false;

    return dictHasStringKey(value, "tag") &&
           dictHasStringKey(value, "attrs") &&
           dictHasStringKey(value, "children");
}

std::string parseXmlReadMode(const Value& value)
{
    const std::string mode = requireStringValue(value, "from_xml mode");
    if (mode != XML_MODE_COMPACT && mode != XML_MODE_RAW)
        throw std::invalid_argument("from_xml mode must be 'compact' or 'raw'");
    return mode;
}

std::string parseXmlWriteMode(const Value& value)
{
    const std::string mode = requireStringValue(value, "to_xml mode");
    if (mode != XML_MODE_AUTO && mode != XML_MODE_COMPACT && mode != XML_MODE_RAW)
        throw std::invalid_argument("to_xml mode must be 'auto', 'compact' or 'raw'");
    return mode;
}

Value elementToRawValue(const pugi::xml_node& node, bool preserveWhitespace);
Value elementToCompactValue(const pugi::xml_node& node, bool preserveWhitespace);
XmlNode rawValueToXmlNode(const Value& value, const std::string& context);
XmlNode compactValueToXmlNode(const Value& value,
                              const std::optional<std::string>& tagHint,
                              const std::string& context);

Value elementToRawValue(const pugi::xml_node& node, bool preserveWhitespace)
{
    Value attrs = Value::dictVal();
    for (pugi::xml_attribute attr : node.attributes())
        asDict(attrs)->store(xmlStringValue(attr.name()), xmlStringValue(attr.value()));

    std::vector<Value> children;
    for (pugi::xml_node child : node.children()) {
        switch (child.type()) {
            case pugi::node_element:
                children.push_back(elementToRawValue(child, preserveWhitespace));
                break;
            case pugi::node_pcdata:
            case pugi::node_cdata: {
                std::string text = child.value();
                if (!preserveWhitespace && isWhitespaceOnly(text))
                    break;
                children.push_back(xmlStringValue(text));
                break;
            }
            default:
                break;
        }
    }

    Value result = Value::dictVal();
    dictStoreStringKey(result, "tag", xmlStringValue(node.name()));
    dictStoreStringKey(result, "attrs", attrs);
    dictStoreStringKey(result, "children", Value::listVal(children));
    return result;
}

Value elementToCompactValue(const pugi::xml_node& node, bool preserveWhitespace)
{
    Value result = Value::dictVal();
    dictStoreStringKey(result, "tag", xmlStringValue(node.name()));

    Value attrs = Value::dictVal();
    bool hasAttrs = false;
    for (pugi::xml_attribute attr : node.attributes()) {
        hasAttrs = true;
        asDict(attrs)->store(xmlStringValue(attr.name()), xmlStringValue(attr.value()));
    }
    if (hasAttrs)
        dictStoreStringKey(result, "attrs", attrs);

    std::string directText;
    for (pugi::xml_node child : node.children()) {
        switch (child.type()) {
            case pugi::node_element: {
                std::string childName = child.name();
                if (childName == "tag" || childName == "attrs" || childName == "text") {
                    throw std::invalid_argument(
                        "compact XML mode cannot represent child element named '" + childName +
                        "'; use raw mode");
                }

                Value childValue = elementToCompactValue(child, preserveWhitespace);
                Value keyValue = xmlStringValue(childName);
                if (!asDict(result)->contains(keyValue)) {
                    asDict(result)->store(keyValue, childValue);
                } else {
                    Value existing = asDict(result)->at(keyValue);
                    if (isList(existing)) {
                        asList(existing)->append(childValue);
                    } else {
                        std::vector<Value> grouped{existing, childValue};
                        asDict(result)->store(keyValue, Value::listVal(grouped));
                    }
                }
                break;
            }
            case pugi::node_pcdata:
            case pugi::node_cdata: {
                std::string text = child.value();
                if (!preserveWhitespace && isWhitespaceOnly(text))
                    break;
                directText += text;
                break;
            }
            default:
                break;
        }
    }

    if (!directText.empty())
        dictStoreStringKey(result, "text", xmlStringValue(directText));

    return result;
}

XmlChild makeTextChild(std::string text)
{
    XmlChild child;
    child.isText = true;
    child.text = std::move(text);
    return child;
}

XmlChild makeNodeChild(XmlNode node)
{
    XmlChild child;
    child.node = std::make_unique<XmlNode>(std::move(node));
    return child;
}

XmlNode rawValueToXmlNode(const Value& value, const std::string& context)
{
    requireExactKeys(value, {"tag", "attrs", "children"}, context);

    XmlNode node;
    node.tag = requireStringValue(dictGetStringKey(value, "tag").value(), context + ".tag");
    if (node.tag.empty())
        throw std::invalid_argument(context + ".tag must not be empty");

    Value attrsValue = dictGetStringKey(value, "attrs").value();
    if (!isDict(attrsValue))
        throw std::invalid_argument(context + ".attrs must be dict");
    for (const auto& kv : asDict(attrsValue)->items()) {
        if (!isString(kv.first))
            throw std::invalid_argument(context + ".attrs keys must be string");
        if (!isString(kv.second))
            throw std::invalid_argument(context + ".attrs values must be string");
        node.attrs.emplace_back(toUTF8StdString(asStringObj(kv.first)->s),
                                toUTF8StdString(asStringObj(kv.second)->s));
    }

    Value childrenValue = dictGetStringKey(value, "children").value();
    if (!isList(childrenValue))
        throw std::invalid_argument(context + ".children must be list");
    ObjList* list = asList(childrenValue);
    for (int i = 0; i < list->length(); ++i) {
        Value childValue = list->getElement(i);
        if (isString(childValue)) {
            node.children.push_back(makeTextChild(toUTF8StdString(asStringObj(childValue)->s)));
        } else if (isDict(childValue)) {
            node.children.push_back(
                makeNodeChild(rawValueToXmlNode(childValue,
                                                context + ".children[" + std::to_string(i) + "]")));
        } else {
            throw std::invalid_argument(context + ".children elements must be string or raw XML dict");
        }
    }

    return node;
}

XmlNode compactValueToXmlNode(const Value& value,
                              const std::optional<std::string>& tagHint,
                              const std::string& context)
{
    if (!isDict(value))
        throw std::invalid_argument(context + " must be dict");

    XmlNode node;
    auto tagValue = dictGetStringKey(value, "tag");
    if (tagValue.has_value()) {
        node.tag = requireStringValue(tagValue.value(), context + ".tag");
    } else if (tagHint.has_value()) {
        node.tag = *tagHint;
    } else {
        throw std::invalid_argument(context + " must contain string key 'tag'");
    }
    if (tagHint.has_value() && !node.tag.empty() && node.tag != *tagHint) {
        throw std::invalid_argument(context + ".tag '" + node.tag +
                                    "' does not match parent key '" + *tagHint + "'");
    }
    if (node.tag.empty())
        throw std::invalid_argument(context + ".tag must not be empty");

    auto attrsValue = dictGetStringKey(value, "attrs");
    if (attrsValue.has_value()) {
        if (!isDict(attrsValue.value()))
            throw std::invalid_argument(context + ".attrs must be dict");
        for (const auto& kv : asDict(attrsValue.value())->items()) {
            if (!isString(kv.first))
                throw std::invalid_argument(context + ".attrs keys must be string");
            if (!isString(kv.second))
                throw std::invalid_argument(context + ".attrs values must be string");
            node.attrs.emplace_back(toUTF8StdString(asStringObj(kv.first)->s),
                                    toUTF8StdString(asStringObj(kv.second)->s));
        }
    }

    auto textValue = dictGetStringKey(value, "text");
    if (textValue.has_value()) {
        if (!isString(textValue.value()))
            throw std::invalid_argument(context + ".text must be string");
        node.children.push_back(makeTextChild(toUTF8StdString(asStringObj(textValue.value())->s)));
    }

    for (const auto& kv : asDict(value)->items()) {
        if (!isString(kv.first))
            throw std::invalid_argument(context + " keys must be string");

        std::string key = toUTF8StdString(asStringObj(kv.first)->s);
        if (key == "tag" || key == "attrs" || key == "text")
            continue;

        if (isList(kv.second)) {
            ObjList* grouped = asList(kv.second);
            for (int i = 0; i < grouped->length(); ++i) {
                node.children.push_back(
                    makeNodeChild(compactValueToXmlNode(grouped->getElement(i),
                                                        key,
                                                        context + "." + key + "[" +
                                                        std::to_string(i) + "]")));
            }
        } else {
            node.children.push_back(
                makeNodeChild(compactValueToXmlNode(kv.second, key, context + "." + key)));
        }
    }

    return node;
}

void appendXmlNode(pugi::xml_node& parent, const XmlNode& node)
{
    pugi::xml_node element = parent.append_child(node.tag.c_str());
    if (!element)
        throw std::invalid_argument("invalid xml element name '" + node.tag + "'");

    for (const auto& attr : node.attrs) {
        if (attr.first.empty())
            throw std::invalid_argument("xml attribute names must not be empty");
        pugi::xml_attribute xmlAttr = element.append_attribute(attr.first.c_str());
        if (!xmlAttr)
            throw std::invalid_argument("invalid xml attribute name '" + attr.first + "'");
        xmlAttr.set_value(attr.second.c_str());
    }

    for (const auto& child : node.children) {
        if (child.isText) {
            pugi::xml_node textNode = element.append_child(pugi::node_pcdata);
            textNode.set_value(child.text.c_str());
        } else if (child.node) {
            appendXmlNode(element, *child.node);
        }
    }
}

#endif

std::string moduleDisplayName(ObjModuleType* module)
{
    if (!module || module->name.isEmpty())
        return "<anonymous>";
    return toUTF8StdString(module->name);
}

std::string describeTypeKind(const ObjObjectType* type)
{
    if (!type)
        return "type";
    if (type->isActor)
        return "actor";
    if (type->isInterface)
        return "interface";
    if (type->isEnumeration)
        return "enum";
    return "object";
}

std::string moduleHelpString(ObjModuleType* module)
{
    std::ostringstream out;
    out << "module " << moduleDisplayName(module) << "\n";
    out << formatSymbolEntries(collectModuleEntries(module), 2);
    return out.str();
}

std::string typeHelpString(ObjObjectType* type, bool isInstance = false)
{
    std::ostringstream out;
    out << "type " << toUTF8StdString(type->name) << " " << describeTypeKind(type);
    if (isInstance)
        out << " instance";
    out << "\n";
    out << "Properties:\n";
    out << formatSymbolEntries(collectPropertyEntries(type), 2, 100, "<none>");
    out << "Methods:\n";
    out << formatSymbolEntries(collectMethodEntries(type), 2, 100, "<none>");
    return out.str();
}

#ifdef _WIN32
std::time_t timegm_compat(std::tm* tm)
{
    return _mkgmtime(tm);
}
#else
std::time_t timegm_compat(std::tm* tm)
{
    return timegm(tm);
}
#endif

bool toCalendar(int64_t seconds, ClockZone zone, std::tm& out)
{
    std::time_t tt = static_cast<std::time_t>(seconds);
#ifdef _WIN32
    if (zone == ClockZone::UTC)
        return gmtime_s(&out, &tt) == 0;
    return localtime_s(&out, &tt) == 0;
#else
    if (zone == ClockZone::UTC)
        return gmtime_r(&tt, &out) != nullptr;
    return localtime_r(&tt, &out) != nullptr;
#endif
}

std::string formatWithMicros(const std::tm& tm, int32_t micros, const std::string& fmt)
{
    std::string result;
    std::string chunk;

    auto flushChunk = [&](const std::string& part) {
        if (part.empty())
            return;
        std::size_t size = part.size() + 64;
        std::vector<char> buffer(size);
        std::size_t written = std::strftime(buffer.data(), buffer.size(), part.c_str(), &tm);
        while (written == 0) {
            size *= 2;
            if (size > 8192)
                throw std::runtime_error("failed to format time with strftime");
            buffer.resize(size);
            written = std::strftime(buffer.data(), buffer.size(), part.c_str(), &tm);
        }
        result.append(buffer.data(), written);
    };

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && (i + 1) < fmt.size() && fmt[i + 1] == 'f') {
            flushChunk(chunk);
            chunk.clear();
            char buf[7];
            std::snprintf(buf, sizeof(buf), "%06d", micros);
            result.append(buf);
            ++i; // skip 'f'
        } else {
            chunk.push_back(fmt[i]);
        }
    }
    flushChunk(chunk);
    return result;
}

int64_t parseWallTime(const std::string& text, const std::string& format, ClockZone zone)
{
    std::string fmt = format;
    std::string working = text;
    int32_t micros = 0;

    std::size_t pos = fmt.find("%f");
    if (pos != std::string::npos) {
        if (fmt.find("%f", pos + 2) != std::string::npos)
            throw std::invalid_argument("format may contain at most one %f");
        if (pos == 0)
            throw std::invalid_argument("%f specifier must follow a character");
        if (fmt[pos - 1] != '.')
            throw std::invalid_argument("%f specifier must be preceded by '.'");
        std::size_t dotPos = working.rfind('.');
        if (dotPos == std::string::npos)
            throw std::invalid_argument("time string missing fractional seconds");
        std::size_t digitPos = dotPos + 1;
        if (digitPos >= working.size() ||
            !std::isdigit(static_cast<unsigned char>(working[digitPos])))
            throw std::invalid_argument("expected digits after '.' for fractional seconds");
        std::size_t scan = digitPos;
        int32_t microVal = 0;
        int digits = 0;
        while (scan < working.size() && std::isdigit(static_cast<unsigned char>(working[scan]))) {
            if (digits < 6)
                microVal = microVal * 10 + (working[scan] - '0');
            ++digits;
            ++scan;
        }
        if (digits == 0)
            throw std::invalid_argument("expected digits for fractional seconds");
        while (digits < 6) {
            microVal *= 10;
            ++digits;
        }
        micros = microVal;
        working.erase(dotPos, scan - dotPos);
        fmt.erase(pos, 2);
        fmt.erase(pos - 1, 1);
    }

    std::tm tm {};
    std::istringstream iss(working);
    iss.imbue(std::locale::classic());
    iss >> std::get_time(&tm, fmt.c_str());
    if (iss.fail())
        throw std::invalid_argument("time string does not match format");

    std::tm tmCopy = tm;
    std::time_t seconds = (zone == ClockZone::UTC)
        ? timegm_compat(&tmCopy)
        : std::mktime(&tmCopy);

    int64_t totalMicros = static_cast<int64_t>(seconds) * MICROS_PER_SECOND + micros;
    return totalMicros;
}

bool instanceOf(ObjectInstance* inst, ObjObjectType* type)
{
    if (!type)
        return false;
    ObjObjectType* current = asObjectType(inst->instanceType);
    while (current) {
        if (current == type)
            return true;
        if (current->superType.isNil())
            break;
        current = asObjectType(current->superType);
    }
    return false;
}

ObjectInstance* requireInstance(const Value& value, ObjObjectType* type,
                                const char* method, const char* expectedName)
{
    if (!isObjectInstance(value))
        throw std::invalid_argument(std::string(method) + " expects " + expectedName + " instance");
    ObjectInstance* inst = asObjectInstance(value);
    if (!instanceOf(inst, type))
        throw std::invalid_argument(std::string(method) + " expects " + expectedName + " instance");
    return inst;
}

int32_t readIntProperty(ObjectInstance* inst, const char* name)
{
    Value v = inst->getProperty(name);
    if (!v.isInt())
        throw std::runtime_error(std::string("expected int property '") + name + "'");
    return v.asInt();
}

bool readBoolProperty(ObjectInstance* inst, const char* name)
{
    Value v = inst->getProperty(name);
    if (!v.isBool())
        throw std::runtime_error(std::string("expected bool property '") + name + "'");
    return v.asBool();
}

double readNumberProperty(const Value& instanceValue, const char* name)
{
    if (!isObjectInstance(instanceValue))
        throw std::runtime_error(std::string("expected object instance for property '") + name + "'");
    Value v = asObjectInstance(instanceValue)->getProperty(name);
    if (!v.isNumber())
        throw std::runtime_error(std::string("expected numeric property '") + name + "'");
    return v.isReal() ? v.asReal() : static_cast<double>(v.asInt());
}

int32_t readListIntElement(const Value& listValue, int index, const char* name)
{
    if (!isList(listValue))
        throw std::runtime_error(std::string("expected list property '") + name + "'");
    ObjList* list = asList(listValue);
    if (index < 0 || index >= list->length())
        throw std::runtime_error(std::string("expected element in list property '") + name + "'");
    Value v = list->getElement(index);
    if (!v.isNumber())
        throw std::runtime_error(std::string("expected numeric element in list property '") + name + "'");
    return static_cast<int32_t>(v.asInt());
}

int64_t microsFromSeconds(double seconds, const char* context)
{
    if (!std::isfinite(seconds))
        throw std::invalid_argument(std::string(context) + " must be finite");
    long double totalMicros = static_cast<long double>(seconds) * static_cast<long double>(MICROS_PER_SECOND);
    if (totalMicros > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        totalMicros < static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        throw std::out_of_range(std::string(context) + " overflow");
    }
    return static_cast<int64_t>(std::llround(totalMicros));
}

int64_t quantityTimeMicros(const Value& instanceValue, const char* context = "wait duration")
{
    if (!isObjectInstance(instanceValue))
        throw std::runtime_error(std::string("expected object instance for ") + context);
    Value dimsValue = asObjectInstance(instanceValue)->getProperty("_d");
    if (!isList(dimsValue) || asList(dimsValue)->length() != 4)
        throw std::runtime_error("expected four-element list property '_d'");

    int32_t lenDim = readListIntElement(dimsValue, 0, "_d");
    int32_t timeDim = readListIntElement(dimsValue, 1, "_d");
    int32_t massDim = readListIntElement(dimsValue, 2, "_d");
    int32_t angleDim = readListIntElement(dimsValue, 3, "_d");
    if (lenDim != 0 || timeDim != 1 || massDim != 0 || angleDim != 0)
        throw std::invalid_argument(std::string(context) + " quantity must have time dimensions");

    return microsFromSeconds(readNumberProperty(instanceValue, "_v"), context);
}

int64_t waitFieldMicros(int64_t value, int64_t scale)
{
    if (value > 0 && value > std::numeric_limits<int64_t>::max() / scale)
        throw std::out_of_range("wait duration overflow");
    if (value < 0 && value < std::numeric_limits<int64_t>::min() / scale)
        throw std::out_of_range("wait duration overflow");
    return value * scale;
}

int64_t timeTotalMicros(ObjectInstance* inst)
{
    int64_t seconds = readIntProperty(inst, "_seconds");
    int64_t micros = readIntProperty(inst, "_micros");
    return seconds * MICROS_PER_SECOND + micros;
}

bool timeIsSteady(ObjectInstance* inst)
{
    return readBoolProperty(inst, "_steady");
}

int64_t spanTotalMicros(ObjectInstance* inst)
{
    int64_t seconds = readIntProperty(inst, "_seconds");
    int64_t micros = readIntProperty(inst, "_micros");
    return seconds * MICROS_PER_SECOND + micros;
}

// Extract total microseconds from a TimeSpan instance or time-dimensioned quantity.
// Throws if the value is neither.
int64_t otherToTimeMicros(const Value& val, ObjObjectType* timeSpanTypeObj,
                          ObjObjectType* quantityTypeObj, const char* opName)
{
    if (isObjectInstance(val)) {
        ObjectInstance* inst = asObjectInstance(val);
        if (instanceOf(inst, timeSpanTypeObj))
            return spanTotalMicros(inst);
        if (instanceOf(inst, quantityTypeObj))
            return quantityTimeMicros(val, opName);
    }
    throw std::invalid_argument(std::string(opName) + " expects TimeSpan or time quantity");
}

void assignTime(ObjectInstance* inst, int64_t totalMicros, bool steady)
{
    NormalizedParts parts = normalizeMicros(totalMicros);
    inst->setProperty("_seconds", Value::intVal(parts.seconds));
    inst->setProperty("_micros", Value::intVal(parts.micros));
    inst->setProperty("_steady", Value::boolVal(steady));
}

void assignSpan(ObjectInstance* inst, int64_t totalMicros)
{
    NormalizedParts parts = normalizeMicros(totalMicros);
    inst->setProperty("_seconds", Value::intVal(parts.seconds));
    inst->setProperty("_micros", Value::intVal(parts.micros));
}

Value newTimeInstance(const Value& typeValue, int64_t totalMicros, bool steady)
{
    Value inst = Value::objectInstanceVal(typeValue);
    assignTime(asObjectInstance(inst), totalMicros, steady);
    return inst;
}

Value newSpanInstance(const Value& typeValue, int64_t totalMicros)
{
    Value inst = Value::objectInstanceVal(typeValue);
    assignSpan(asObjectInstance(inst), totalMicros);
    return inst;
}

std::string defaultTimeString(ObjectInstance* inst)
{
    if (!inst)
        return std::string();

    if (timeIsSteady(inst))
        return std::string("steady ") + humanDurationString(timeTotalMicros(inst));

    int32_t seconds = readIntProperty(inst, "_seconds");
    int32_t micros = readIntProperty(inst, "_micros");

    std::tm tm {};
    if (!toCalendar(seconds, ClockZone::Local, tm))
        return std::string("object Time");

    try {
        return formatWithMicros(tm, micros, "%Y-%m-%d %H:%M:%S");
    } catch (...) {
        return std::string("object Time");
    }
}

std::string defaultSpanString(ObjectInstance* inst)
{
    if (!inst)
        return std::string();
    return humanDurationString(spanTotalMicros(inst));
}

// ============================================================================
// quantity string parsing
// ============================================================================
//
// Parses strings of the forms emitted by quantity's operator string() plus all
// @suffix-registered literal forms, plus "natural" caret variants (e.g.
// `m/s^3`) that aren't valid as code literals but are useful to type in JSON
// configs. Dimension vector indices: [Length, Time, Mass, Angle].

namespace qtyparse {

struct NamedUnit {
    icu::UnicodeString text;
    double scale;
    std::array<int32_t, 4> dims;
};

constexpr double kPi = 3.14159265358979323846;

// Broad named-unit table for prefix matching when the suffix contains no
// Unicode superscript. Compound `*/s` velocity/angular-velocity forms are in
// the exact-match table only; here we just supply named *singletons* that get
// composed via `/` and exponents.
const std::vector<NamedUnit>& broadTable()
{
    static const std::vector<NamedUnit> table = {
        // Length
        {toUnicodeString("um"),  1e-6,         {1,0,0,0}},
        {toUnicodeString("μm"), 1e-6,     {1,0,0,0}}, // μm
        {toUnicodeString("mm"),  1e-3,         {1,0,0,0}},
        {toUnicodeString("cm"),  1e-2,         {1,0,0,0}},
        {toUnicodeString("mil"), 2.54e-5,      {1,0,0,0}},
        {toUnicodeString("in"),  0.0254,       {1,0,0,0}},
        {toUnicodeString("ft"),  0.3048,       {1,0,0,0}},
        {toUnicodeString("m"),   1.0,          {1,0,0,0}},
        // Mass
        {toUnicodeString("mg"),  1e-6,         {0,0,1,0}},
        {toUnicodeString("kg"),  1.0,          {0,0,1,0}},
        {toUnicodeString("lb"),  0.45359237,   {0,0,1,0}},
        {toUnicodeString("oz"),  0.0283495231, {0,0,1,0}},
        {toUnicodeString("g"),   1e-3,         {0,0,1,0}},
        // Time
        {toUnicodeString("us"),  1e-6,         {0,1,0,0}},
        {toUnicodeString("μs"), 1e-6,     {0,1,0,0}}, // μs
        {toUnicodeString("ms"),  1e-3,         {0,1,0,0}},
        {toUnicodeString("min"), 60.0,         {0,1,0,0}},
        {toUnicodeString("hr"),  3600.0,       {0,1,0,0}},
        {toUnicodeString("s"),   1.0,          {0,1,0,0}},
        // Angle
        {toUnicodeString("rad"), 1.0,          {0,0,0,1}},
        {toUnicodeString("deg"), kPi/180.0,    {0,0,0,1}},
        {toUnicodeString("°"), kPi/180.0, {0,0,0,1}}, // °
        // Force
        {toUnicodeString("N"),   1.0,          {1,-2,1,0}},
    };
    return table;
}

// SI-restricted table used when the suffix contains Unicode superscript
// exponents (matches the dim_label() output exactly).
const std::vector<NamedUnit>& siTable()
{
    static const std::vector<NamedUnit> table = {
        {toUnicodeString("kg"),  1.0, {0,0,1,0}},
        {toUnicodeString("rad"), 1.0, {0,0,0,1}},
        {toUnicodeString("m"),   1.0, {1,0,0,0}},
        {toUnicodeString("s"),   1.0, {0,1,0,0}},
    };
    return table;
}

int superscriptDigitValue(UChar32 c)
{
    switch (c) {
        case 0x2070: return 0;  // ⁰
        case 0x00B9: return 1;  // ¹
        case 0x00B2: return 2;  // ²
        case 0x00B3: return 3;  // ³
        case 0x2074: return 4;  // ⁴
        case 0x2075: return 5;  // ⁵
        case 0x2076: return 6;  // ⁶
        case 0x2077: return 7;  // ⁷
        case 0x2078: return 8;  // ⁸
        case 0x2079: return 9;  // ⁹
        default:     return -1;
    }
}

bool containsSuperscriptOrMinus(const icu::UnicodeString& s)
{
    int32_t i = 0;
    int32_t len = s.length();
    while (i < len) {
        UChar32 c = s.char32At(i);
        if (superscriptDigitValue(c) >= 0 || c == 0x207B)
            return true;
        i += U16_LENGTH(c);
    }
    return false;
}

struct ExpResult {
    int value;
    int32_t consumed;
    bool present;
};

// Parse an optional exponent at position p of s. Recognises:
//   - Unicode superscripts: ⁻? [⁰¹²³⁴⁵⁶⁷⁸⁹]+
//   - Caret notation: ^[+-]?[0-9]+
// Returns {value=1, consumed=0, present=false} if no exponent is present.
// Throws on malformed exponent (e.g. lone '^', lone '⁻').
ExpResult parseExponent(const icu::UnicodeString& s, int32_t p)
{
    int32_t len = s.length();
    if (p >= len) return {1, 0, false};

    UChar32 c = s.char32At(p);

    if (c == U'^') {
        int32_t i = p + 1;
        int sign = 1;
        if (i < len && (s.char32At(i) == U'-' || s.char32At(i) == U'+')) {
            if (s.char32At(i) == U'-') sign = -1;
            ++i;
        }
        int val = 0;
        int digits = 0;
        while (i < len) {
            UChar32 d = s.char32At(i);
            if (d < U'0' || d > U'9') break;
            val = val * 10 + int(d - U'0');
            ++digits;
            ++i;
        }
        if (digits == 0)
            throw std::invalid_argument("malformed exponent after '^'");
        return {sign * val, i - p, true};
    }

    int sign = 1;
    int32_t i = p;
    if (c == 0x207B) {
        sign = -1;
        i += 1;
    }
    int val = 0;
    int digits = 0;
    while (i < len) {
        UChar32 d = s.char32At(i);
        int dv = superscriptDigitValue(d);
        if (dv < 0) break;
        val = val * 10 + dv;
        ++digits;
        i += U16_LENGTH(d);
    }
    if (digits == 0) {
        if (sign == -1)
            throw std::invalid_argument("dangling Unicode minus-sign superscript");
        return {1, 0, false};
    }
    return {sign * val, i - p, true};
}

struct PrefixMatch {
    const NamedUnit* unit;
    int32_t consumed;
};

// Find the longest named unit in `table` that is a prefix of s starting at p.
PrefixMatch longestPrefix(const std::vector<NamedUnit>& table,
                          const icu::UnicodeString& s, int32_t p)
{
    PrefixMatch best { nullptr, 0 };
    int32_t len = s.length();
    for (const auto& u : table) {
        int32_t ulen = u.text.length();
        if (p + ulen > len) continue;
        if (s.compare(p, ulen, u.text) == 0) {
            if (ulen > best.consumed) {
                best.unit = &u;
                best.consumed = ulen;
            }
        }
    }
    return best;
}

// Apply factor `unit^exp` to (scale, dims).
void applyUnit(double& scale, std::array<int32_t,4>& dims,
               const NamedUnit& unit, int exp)
{
    if (exp != 0) {
        // pow(scale, exp) for integer exponents is well-defined; std::pow
        // handles negative and zero scales correctly here (all our scales > 0).
        scale *= std::pow(unit.scale, exp);
        for (int i = 0; i < 4; ++i)
            dims[i] += exp * unit.dims[i];
    }
}

// Parse a unit expression — sequence of (named-unit, optional-exponent)
// separated by `·`, `*`, or whitespace, with at most one `/` that flips the
// exponent sign of everything after it. Returns false (and leaves output
// indeterminate) if parsing fails.
bool parseUnitExpression(const icu::UnicodeString& s, int32_t start,
                         const std::vector<NamedUnit>& table,
                         double& outScale, std::array<int32_t,4>& outDims)
{
    outScale = 1.0;
    outDims = {0,0,0,0};

    int32_t i = start;
    int32_t len = s.length();
    int sign = 1;
    bool any = false;

    while (i < len) {
        UChar32 c = s.char32At(i);
        if (c == U' ' || c == U'\t' || c == 0x00B7 /* · */ || c == U'*') {
            ++i;
            continue;
        }
        if (c == U'/') {
            if (sign == -1) return false; // only one '/' allowed
            sign = -1;
            ++i;
            continue;
        }

        PrefixMatch m = longestPrefix(table, s, i);
        if (m.unit == nullptr) return false;
        i += m.consumed;

        ExpResult e;
        try {
            e = parseExponent(s, i);
        } catch (...) {
            return false;
        }
        int exp = e.present ? e.value : 1;
        i += e.consumed;

        applyUnit(outScale, outDims, *m.unit, sign * exp);
        any = true;
    }

    return any;
}

} // namespace qtyparse

} // namespace

ObjObjectType* roxal::sysTimeType()
{
    return gSysTimeType;
}

ObjObjectType* roxal::sysTimeSpanType()
{
    return gSysTimeSpanType;
}

std::string roxal::sysTimeDefaultString(ObjectInstance* inst)
{
    return defaultTimeString(inst);
}

std::string roxal::sysTimeSpanDefaultString(ObjectInstance* inst)
{
    return defaultSpanString(inst);
}

ObjObjectType* roxal::sysQuantityType()
{
    return gSysQuantityType;
}

std::optional<double> roxal::sysTimeQuantitySeconds(const Value& v, const char* context)
{
    if (!isObjectInstance(v) || gSysQuantityType == nullptr ||
        !instanceOf(asObjectInstance(v), gSysQuantityType))
        return std::nullopt;
    return static_cast<double>(quantityTimeMicros(v, context)) / 1e6;
}

std::string roxal::sysQuantityDefaultString(ObjectInstance* inst)
{
    double v = readNumberProperty(Value::objRef(inst), "_v");
    Value dVal = inst->getProperty("_d");
    if (!isList(dVal) || asList(dVal)->length() != 4)
        return "quantity(?)";

    int32_t dL = readListIntElement(dVal, 0, "_d");
    int32_t dT = readListIntElement(dVal, 1, "_d");
    int32_t dM = readListIntElement(dVal, 2, "_d");
    int32_t dA = readListIntElement(dVal, 3, "_d");

    double av = std::abs(v);

    // Length [1,0,0,0]
    if (dL==1 && dT==0 && dM==0 && dA==0) {
        if (av >= 1.0)        return format("%g", v) + "m";
        if (av >= 0.01)       return format("%g", v * 100.0) + "cm";
        if (av >= 0.001)      return format("%g", v * 1000.0) + "mm";
        return format("%g", v * 1000000.0) + "um";
    }
    // Time [0,1,0,0]
    if (dL==0 && dT==1 && dM==0 && dA==0) {
        if (av >= 3600.0)     return format("%g", v / 3600.0) + "hr";
        if (av >= 60.0)       return format("%g", v / 60.0) + "min";
        if (av >= 1.0)        return format("%g", v) + "s";
        if (av >= 0.001)      return format("%g", v * 1000.0) + "ms";
        return format("%g", v * 1000000.0) + "us";
    }
    // Mass [0,0,1,0]
    if (dL==0 && dT==0 && dM==1 && dA==0) {
        if (av >= 1.0)        return format("%g", v) + "kg";
        if (av >= 0.001)      return format("%g", v * 1000.0) + "g";
        return format("%g", v * 1000000.0) + "mg";
    }
    // Angle [0,0,0,1]
    if (dL==0 && dT==0 && dM==0 && dA==1)
        return format("%g", v * 180.0 / M_PI) + "\u00B0";
    // Velocity [1,-1,0,0]
    if (dL==1 && dT==-1 && dM==0 && dA==0)
        return format("%g", v) + "m/s";
    // Acceleration [1,-2,0,0]
    if (dL==1 && dT==-2 && dM==0 && dA==0)
        return format("%g", v) + "ms\u207B\u00B2";
    // Force [1,-2,1,0]
    if (dL==1 && dT==-2 && dM==1 && dA==0)
        return format("%g", v) + "N";
    // Torque [2,-2,1,0]
    if (dL==2 && dT==-2 && dM==1 && dA==0)
        return format("%g", v) + "Nm";
    // Angular velocity [0,-1,0,1]
    if (dL==0 && dT==-1 && dM==0 && dA==1)
        return format("%g", v * 180.0 / M_PI) + "\u00B0/s";
    // Dimensionless [0,0,0,0]
    if (dL==0 && dT==0 && dM==0 && dA==0)
        return format("%g", v);
    // Unknown dimension — show as SI units with superscript exponents
    auto superscriptExp = [](int e) -> std::string {
        // Unicode superscript digits: ⁰¹²³⁴⁵⁶⁷⁸⁹  minus: ⁻
        static const char* sup[] = {
            "\u2070", "\u00B9", "\u00B2", "\u00B3", "\u2074",
            "\u2075", "\u2076", "\u2077", "\u2078", "\u2079"
        };
        std::string s;
        int ae = e;
        if (ae < 0) { s += "\u207B"; ae = -ae; }
        if (ae >= 10) s += sup[ae / 10];
        s += sup[ae % 10];
        return s;
    };

    std::string units;
    const char* names[] = {"m", "s", "kg", "rad"};
    int dims[] = {dL, dT, dM, dA};
    for (int i = 0; i < 4; ++i) {
        if (dims[i] != 0) {
            units += names[i];
            if (dims[i] != 1)
                units += superscriptExp(dims[i]);
        }
    }
    return format("%g", v) + units;
}

Value roxal::sysNewTimeSpan(int64_t totalMicros)
{
    if (!gSysTimeSpanType)
        throw std::runtime_error("sys.TimeSpan type not found");

    Value typeValue = Value::objRef(gSysTimeSpanType);
    Value span = Value::objectInstanceVal(typeValue);
    assignSpan(asObjectInstance(span), totalMicros);
    return span;
}

ModuleSys::ModuleSys()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("sys")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
    timeTypeValue = Value::nilVal();
    timeSpanTypeValue = Value::nilVal();
}

ModuleSys::~ModuleSys()
{
    destroyModuleType(moduleTypeValue);
}


Value ModuleSys::typeMethodDecl(const Value& typeValue, const std::string& methodName) const
{
    if (typeValue.isNil() || !isObjectType(typeValue))
        return Value::nilVal();

    ObjObjectType* type = asObjectType(typeValue);
    auto hash = toUnicodeString(methodName).hashCode();
    // First overload; sys helpers don't yet pick among overloads.
    auto* method = type->firstOverload(hash);
    if (method == nullptr)
        return Value::nilVal();

    Value closure = method->closure;
    if (!isClosure(closure))
        return Value::nilVal();

    ObjClosure* cl = asClosure(closure);
    Value functionValue = cl->function;
    if (!isFunction(functionValue))
        return Value::nilVal();

    return functionValue;
}


void ModuleSys::registerBuiltins(VM& vm)
{
    setVM(vm);

    auto addSys = [&](const std::string& name, NativeFn fn,
                      ptr<type::Type> funcType = nullptr,
                      std::vector<Value> defaults = {},
                      uint32_t resolveArgMask = 0){
        if (!vm.loadGlobal(toUnicodeString(name)).has_value())
            vm.defineNative(name, fn, funcType, defaults, resolveArgMask);
        link(name, fn, defaults, resolveArgMask);
    };

    if (!vm.loadGlobal(toUnicodeString("print")).has_value()) {
        std::vector<Value> pdefaults{
            Value::stringVal(toUnicodeString("")),
            Value::stringVal(toUnicodeString("\n")),
            Value::falseVal(),
            Value::falseVal()
        };
        // Construct funcType matching:
        // proc print(value:string='', end='\n', flush:bool=false, here:bool=false)
        // The :string type on value enables async user-defined conversion (operator->string)
        // for objects passed to print, via callNativeFn's NativeParamConversionState.
        ptr<type::Type> printType = make_ptr<type::Type>(type::BuiltinType::Func);
        printType->func = type::Type::FuncType();
        printType->func->isProc = true;
        auto printParams = BuiltinModule::constructParams({
            {"value", type::BuiltinType::String},
            {"end", type::BuiltinType::String},
            {"flush", type::BuiltinType::Bool},
            {"here", type::BuiltinType::Bool}
        }, pdefaults);
        printType->func->params.resize(printParams.size());
        for (size_t i = 0; i < printParams.size(); ++i) printType->func->params[i] = printParams[i];
        addSys("print", [this](VM& vm, ArgsView a){ return print_builtin(vm,a); }, printType, pdefaults, 0x1);
        addSys("len", [this](VM& vm, ArgsView a){ return len_builtin(vm,a); }, nullptr, {}, 0x1);
        addSys("help", [this](VM& vm, ArgsView a){ return help_builtin(vm,a); }, nullptr, {}, 0x1);
        addSys("clone", [this](VM& vm, ArgsView a){ return clone_builtin(vm,a); });
        {
            ptr<type::Type> t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            t->func->isProc = false;
            std::vector<Value> defaults { Value::nilVal(), Value::intVal(0), Value::intVal(0), Value::intVal(0), Value::intVal(0), Value::nilVal() };
            auto params = BuiltinModule::constructParams({ {"duration", std::nullopt},
                                           {"s", type::BuiltinType::Int},
                                           {"ms", type::BuiltinType::Int},
                                           {"us", type::BuiltinType::Int},
                                           {"ns", type::BuiltinType::Int},
                                           {"for", std::nullopt} },
                                         defaults);
            if (params.size() == defaults.size()) {
                params.front().hasDefault = true;
                params.back().hasDefault = true;
            }
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("wait", [this](VM& vm, ArgsView a){ return wait_builtin(vm,a); }, t, defaults);
        }
        // ignore(value): escape hatch for expressions whose value would
        // otherwise auto-trigger at statement position — i.e. a Future
        // (auto-await) or an instance of a type with a `statement action`
        // method (auto-execute). The parameter is untyped (std::nullopt)
        // so the value passes through without the implicit-await ToTypeSpec
        // coercion. ignore() raises if the argument has no statement-action
        // behaviour to suppress (see ignore_builtin).
        {
            ptr<type::Type> t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            t->func->isProc = true;
            std::vector<Value> defaults {};
            auto params = BuiltinModule::constructParams(
                { {"value", std::nullopt} }, defaults);
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("ignore", [this](VM& vm, ArgsView a){ return ignore_builtin(vm,a); },
                   t, defaults);
        }
        addSys("is_ready", [this](VM& vm, ArgsView a){ return is_ready_builtin(vm,a); });
        // sys.allof(...items) / sys.anyof(...items): combinators over
        // futures, event types, and bool signals. Each positional arg may
        // be an awaitable or a list of awaitables (flattened one level).
        // Result: a future. allof resolves to a list of values in arg order;
        // anyof resolves to a dict {"index": i, "value": v} for the first
        // resolved slot. No funcType so positional args pass through as a
        // raw ArgsView (variadic-via-typing isn't supported for natives).
        // resolveArgMask=0 because futures are first-class inputs and must
        // not be auto-resolved at the call site.
        addSys("allof", [this](VM& vm, ArgsView a){ return allof_builtin(vm,a); }, nullptr, {}, 0x0);
        addSys("anyof", [this](VM& vm, ArgsView a){ return anyof_builtin(vm,a); }, nullptr, {}, 0x0);
        addSys("_event_subscriber_count", [this](VM& vm, ArgsView a){ return event_subscriber_count_builtin(vm,a); });
        addSys("fork", [this](VM& vm, ArgsView a){ return fork_builtin(vm,a); });
        addSys("join", [this](VM& vm, ArgsView a){ return join_builtin(vm,a); }, nullptr, {}, 0x1);
        {
            ptr<type::Type> t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            t->func->isProc = true;
            std::vector<Value> defaults{ Value::intVal(0) };
            auto params = BuiltinModule::constructParams({{"ret", type::BuiltinType::Int}}, defaults);
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("exit", [this](VM& vm, ArgsView a){ return exit_builtin(vm,a); }, t, defaults, 0x1);
        }
        addSys("stacktrace", [this](VM& vm, ArgsView a){ return stacktrace_builtin(vm,a); });
        addSys("_threadid", [this](VM& vm, ArgsView a){ return threadid_builtin(vm,a); });
        addSys("_stackdepth", [this](VM& vm, ArgsView a){ return stackdepth_builtin(vm,a); });
        addSys("_runtests", [this](VM& vm, ArgsView a){ return runtests_builtin(vm,a); });
        addSys("_invoke_method", [this](VM& vm, ArgsView a){ return invoke_method_builtin(vm,a); });
        addSys("_watch_property", [this](VM& vm, ArgsView a){ return watch_property_builtin(vm,a); });
        addSys("_watch_count", [this](VM& vm, ArgsView a){ return watch_count_builtin(vm,a); });
        addSys("_weakref", [this](VM& vm, ArgsView a){ return weakref_builtin(vm,a); });
        addSys("_weak_alive", [this](VM& vm, ArgsView a){ return weak_alive_builtin(vm,a); });
        addSys("_strongref", [this](VM& vm, ArgsView a){ return strongref_builtin(vm,a); });
        addSys("_refcount", [this](VM& vm, ArgsView a){ return refcount_builtin(vm,a); });
        addSys("_list_repr", [this](VM& vm, ArgsView a){ return list_repr_builtin(vm,a); });
        addSys("_arity", [this](VM& vm, ArgsView a){ return arity_builtin(vm,a); });
        addSys("gc", [this](VM& vm, ArgsView a){ return gc_builtin(vm,a); });
        addSys("gc_config", [this](VM& vm, ArgsView a){ return gc_config_builtin(vm,a); });
        addSys("serialize", [this](VM& vm, ArgsView a){ return serialize_builtin(vm,a); }, nullptr, {}, 0x1);
        addSys("deserialize", [this](VM& vm, ArgsView a){ return deserialize_builtin(vm,a); }, nullptr, {}, 0x1);

        // Bit / byte conversion utilities. The explicit funcType + defaults
        // are needed for correct positional/named-argument binding at the
        // call site (the sys.rox @builtin declaration is the source-of-truth
        // for the user-visible signature, but callNativeFn's marshalArgs
        // path needs the C++-side funcType too when callers mix named and
        // positional args with omitted defaults in between).
        addSys("to_bytes",
               [this](VM& vm, ArgsView a){ return to_bytes_builtin(vm,a); },
               makeFuncType({
                    {"v", std::nullopt},
                    {"width", type::BuiltinType::Int},
                    {"endian", type::BuiltinType::String}
               }, {Value::nilVal(), Value::intVal(0), Value::stringVal(toUnicodeString("little"))}),
               {Value::nilVal(), Value::intVal(0), Value::stringVal(toUnicodeString("little"))},
               0x1);
        addSys("from_bytes",
               [this](VM& vm, ArgsView a){ return from_bytes_builtin(vm,a); },
               makeFuncType({
                    {"bytes", type::BuiltinType::List},
                    {"dtype", std::nullopt},  // accepts a type value or a string
                    {"endian", type::BuiltinType::String},
                    {"signed", type::BuiltinType::Bool}
               }, {Value::nilVal(),
                   Value::typeVal(ValueType::Real),
                   Value::stringVal(toUnicodeString("little")),
                   Value::trueVal()}),
               {Value::nilVal(),
                Value::typeVal(ValueType::Real),
                Value::stringVal(toUnicodeString("little")),
                Value::trueVal()},
               0x1);
        addSys("bits_to_bytes",
               [this](VM& vm, ArgsView a){ return bits_to_bytes_builtin(vm,a); },
               makeFuncType({
                    {"bits", type::BuiltinType::List},
                    {"msb_first", type::BuiltinType::Bool}
               }, {Value::nilVal(), Value::trueVal()}),
               {Value::nilVal(), Value::trueVal()},
               0x1);
        addSys("bytes_to_bits",
               [this](VM& vm, ArgsView a){ return bytes_to_bits_builtin(vm,a); },
               makeFuncType({
                    {"bytes", type::BuiltinType::List},
                    {"msb_first", type::BuiltinType::Bool}
               }, {Value::nilVal(), Value::trueVal()}),
               {Value::nilVal(), Value::trueVal()},
               0x1);
        addSys("lshift",
               [this](VM& vm, ArgsView a){ return lshift_builtin(vm,a); },
               makeFuncType({
                    {"v", type::BuiltinType::Int},
                    {"n", type::BuiltinType::Int}
               }),
               {}, 0x1);
        addSys("rshift",
               [this](VM& vm, ArgsView a){ return rshift_builtin(vm,a); },
               makeFuncType({
                    {"v", type::BuiltinType::Int},
                    {"n", type::BuiltinType::Int}
               }),
               {}, 0x1);

        addSys("to_json",
               [this](VM& vm, ArgsView a){ return to_json_builtin(vm,a); },
               makeFuncType({
                    {"value", std::nullopt},
                    {"indent", type::BuiltinType::Bool},
                    {"json5", type::BuiltinType::Bool}
               }, {Value::nilVal(), Value::trueVal(), Value::falseVal()}),
               {Value::nilVal(), Value::trueVal(), Value::falseVal()},
               0x1);
        addSys("from_json", [this](VM& vm, ArgsView a){ return from_json_builtin(vm,a); }, nullptr, {}, 0x1);
        addSys("to_xml",
               [this](VM& vm, ArgsView a){ return to_xml_builtin(vm,a); },
               makeFuncType({
                    {"value", std::nullopt},
                    {"indent", type::BuiltinType::Bool},
                    {"mode", type::BuiltinType::String}
               }, {Value::nilVal(), Value::trueVal(), Value::stringVal(toUnicodeString(XML_MODE_AUTO))}),
               {Value::nilVal(), Value::trueVal(), Value::stringVal(toUnicodeString(XML_MODE_AUTO))},
               0x1);
        addSys("from_xml",
               [this](VM& vm, ArgsView a){ return from_xml_builtin(vm,a); },
               makeFuncType({
                    {"xml", type::BuiltinType::String},
                    {"mode", type::BuiltinType::String},
                    {"preserve_whitespace", type::BuiltinType::Bool}
               }, {Value::nilVal(),
                   Value::stringVal(toUnicodeString(XML_MODE_COMPACT)),
                   Value::falseVal()}),
               {Value::nilVal(),
                Value::stringVal(toUnicodeString(XML_MODE_COMPACT)),
                Value::falseVal()},
               0x1);
        addSys("upper",
               [](VM& vm, ArgsView a){ return vm.string_upper_builtin(a); },
               makeFuncType({{"s", type::BuiltinType::String}}),
               {}, 0x1);
        addSys("lower",
               [](VM& vm, ArgsView a){ return vm.string_lower_builtin(a); },
               makeFuncType({{"s", type::BuiltinType::String}}),
               {}, 0x1);
        addSys("capitalize",
               [](VM& vm, ArgsView a){ return vm.string_capitalize_builtin(a); },
               makeFuncType({{"s", type::BuiltinType::String}}),
               {}, 0x1);
        addSys("title",
               [](VM& vm, ArgsView a){ return vm.string_title_builtin(a); },
               makeFuncType({{"s", type::BuiltinType::String}}),
               {}, 0x1);
        // filter, map, reduce are now implemented in pure Roxal in sys.rox
    }

    if (!vm.loadGlobal(toUnicodeString("_clock")).has_value()) {
        addSys("_clock", [this](VM& vm, ArgsView a){ return clock_native(vm,a); });
        {
            ptr<type::Type> t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            std::vector<Value> defaults{ Value::nilVal(), Value::stringVal(toUnicodeString("")) };
            auto params = BuiltinModule::constructParams({
                    {"freq", type::BuiltinType::Int},
                    {"name", type::BuiltinType::String}},
                    defaults);
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("clock", [this](VM& vm, ArgsView a){ return clock_signal_native(vm,a); }, t, {});
        }
        addSys("_engine_stop", [this](VM& vm, ArgsView a){ return engine_stop_native(vm,a); });
        addSys("typeof", [this](VM& vm, ArgsView a){ return typeof_native(vm,a); });
        addSys("_df_graph", [this](VM& vm, ArgsView a){ return df_graph_native(vm,a); });
        addSys("_df_islands", [this](VM& vm, ArgsView a){ return df_islands_native(vm,a); });
        addSys("_dataflow_tick", [this](VM& vm, ArgsView a){ return df_tick_native(vm,a); });
        addSys("_df_graphdot", [this](VM& vm, ArgsView a){ return df_graphdot_native(vm,a); });
        addSys("loadlib", [this](VM& vm, ArgsView a){ return loadlib_native(vm,a); }, nullptr, {}, 0x1);
        addSys("source_dir", [this](VM& vm, ArgsView a){ return source_dir_native(vm,a); });
        addSys("module_paths", [this](VM& vm, ArgsView a){ return module_paths_native(vm,a); });

    }

    auto maybeTime = asModuleType(moduleType())->vars.load(toUnicodeString("Time"));
    if (!maybeTime.has_value() || !isObjectType(maybeTime.value()))
        throw std::runtime_error("sys.Time type not found");
    timeTypeValue = maybeTime.value();
    timeTypeObj = asObjectType(timeTypeValue);
    gSysTimeType = timeTypeObj;
    if (!vm.loadGlobal(toUnicodeString("Time")).has_value())
        vm.globals.storeGlobal(toUnicodeString("Time"), timeTypeValue);

    auto maybeSpan = asModuleType(moduleType())->vars.load(toUnicodeString("TimeSpan"));
    if (!maybeSpan.has_value() || !isObjectType(maybeSpan.value()))
        throw std::runtime_error("sys.TimeSpan type not found");
    timeSpanTypeValue = maybeSpan.value();
    timeSpanTypeObj = asObjectType(timeSpanTypeValue);
    gSysTimeSpanType = timeSpanTypeObj;
    if (!vm.loadGlobal(toUnicodeString("TimeSpan")).has_value())
        vm.globals.storeGlobal(toUnicodeString("TimeSpan"), timeSpanTypeValue);

    auto maybeQuantity = asModuleType(moduleType())->vars.load(toUnicodeString("quantity"));
    if (!maybeQuantity.has_value() || !isObjectType(maybeQuantity.value()))
        throw std::runtime_error("sys.quantity type not found");
    quantityTypeValue = maybeQuantity.value();
    quantityTypeObj = asObjectType(quantityTypeValue);
    gSysQuantityType = quantityTypeObj;
    if (!vm.loadGlobal(toUnicodeString("quantity")).has_value())
        vm.globals.storeGlobal(toUnicodeString("quantity"), quantityTypeValue);

    std::vector<Value> timeInitDefaults{
        Value::stringVal(toUnicodeString("wall")),
        Value::stringVal(toUnicodeString("local"))
    };
    linkMethod("Time", "init", [this](VM& vm, ArgsView a){ return time_init_native(vm,a); }, timeInitDefaults);
    linkMethod("Time", "kind", [this](VM& vm, ArgsView a){ return time_kind_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Time", "is_steady", [this](VM& vm, ArgsView a){ return time_is_steady_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Time", "seconds", [this](VM& vm, ArgsView a){ return time_seconds_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Time", "microseconds", [this](VM& vm, ArgsView a){ return time_micros_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Time", "diff", [this](VM& vm, ArgsView a){ return time_diff_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Time", "since", [this](VM& vm, ArgsView a){ return time_since_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Time", "until_time", [this](VM& vm, ArgsView a){ return time_until_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    std::vector<Value> timeFormatDefaults{
        Value::stringVal(toUnicodeString("%Y-%m-%d %H:%M:%S")),
        Value::stringVal(toUnicodeString("local"))
    };
    linkMethod("Time", "format", [this](VM& vm, ArgsView a){ return time_format_native(vm,a); }, timeFormatDefaults, 0, /*noMutateSelf=*/true);
    std::vector<Value> timeComponentsDefaults{
        Value::stringVal(toUnicodeString("local"))
    };
    linkMethod("Time", "components", [this](VM& vm, ArgsView a){ return time_components_native(vm,a); }, timeComponentsDefaults, 0, /*noMutateSelf=*/true);

    std::vector<Value> spanInitDefaults{
        Value::intVal(0), Value::intVal(0), Value::intVal(0),
        Value::intVal(0), Value::intVal(0), Value::intVal(0)
    };
    linkMethod("TimeSpan", "init", [this](VM& vm, ArgsView a){ return timespan_init_native(vm,a); }, spanInitDefaults);
    linkMethod("TimeSpan", "seconds", [this](VM& vm, ArgsView a){ return timespan_seconds_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "microseconds", [this](VM& vm, ArgsView a){ return timespan_micros_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "split", [this](VM& vm, ArgsView a){ return timespan_split_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "total_days", [this](VM& vm, ArgsView a){ return timespan_total_days_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "total_hours", [this](VM& vm, ArgsView a){ return timespan_total_hours_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "total_minutes", [this](VM& vm, ArgsView a){ return timespan_total_minutes_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "total_seconds", [this](VM& vm, ArgsView a){ return timespan_total_seconds_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "total_millis", [this](VM& vm, ArgsView a){ return timespan_total_millis_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "total_micros", [this](VM& vm, ArgsView a){ return timespan_total_micros_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("TimeSpan", "human", [this](VM& vm, ArgsView a){ return timespan_human_native(vm,a); }, {}, 0, /*noMutateSelf=*/true);

    linkMethod("quantity", "set", [this](VM& vm, ArgsView a){ return quantity_set_builtin(vm,a); });

    {
        std::vector<Value> defaults{ Value::stringVal(toUnicodeString("local")) };
        auto funcType = makeFuncType({{"tz", type::BuiltinType::String}}, defaults);
        vm.defineBuiltinMethod(ValueType::Type, "wall_now",
                               [this](VM& vm, ArgsView a){ return time_type_wall_now(vm,a); },
                               false, funcType, defaults,
                               typeMethodDecl(timeTypeValue, "wall_now"));
    }

    {
        auto funcType = makeFuncType({});
        vm.defineBuiltinMethod(ValueType::Type, "steady_now",
                               [this](VM& vm, ArgsView a){ return time_type_steady_now(vm,a); },
                               false, funcType, {},
                               typeMethodDecl(timeTypeValue, "steady_now"));
    }

    {
        std::vector<Value> defaults{
            Value::nilVal(),
            Value::stringVal(toUnicodeString("%Y-%m-%d %H:%M:%S")),
            Value::stringVal(toUnicodeString("local"))
        };
        auto funcType = makeFuncType({
            {"text", type::BuiltinType::String},
            {"format", type::BuiltinType::String},
            {"tz", type::BuiltinType::String}
        }, defaults);
        vm.defineBuiltinMethod(ValueType::Type, "parse",
                               [this](VM& vm, ArgsView a){ return time_type_parse(vm,a); },
                               false, funcType, defaults,
                               typeMethodDecl(timeTypeValue, "parse"));
    }

    {
        std::vector<Value> defaults{
            Value::nilVal(),
            Value::intVal(0),
            Value::stringVal(toUnicodeString("wall"))
        };
        auto funcType = makeFuncType({
            {"seconds", type::BuiltinType::Int},
            {"micros", type::BuiltinType::Int},
            {"kind", type::BuiltinType::String}
        }, defaults);
        vm.defineBuiltinMethod(ValueType::Type, "from_parts",
                               [this](VM& vm, ArgsView a){ return time_type_from_parts(vm,a); },
                               false, funcType, defaults,
                               typeMethodDecl(timeTypeValue, "from_parts"));
    }

    {
        std::vector<Value> defaults{
            Value::intVal(0), Value::intVal(0), Value::intVal(0),
            Value::intVal(0), Value::intVal(0), Value::intVal(0)
        };
        auto funcType = makeFuncType({
            {"days", type::BuiltinType::Int},
            {"hours", type::BuiltinType::Int},
            {"minutes", type::BuiltinType::Int},
            {"seconds", type::BuiltinType::Int},
            {"millis", type::BuiltinType::Int},
            {"micros", type::BuiltinType::Int}
        }, defaults);
        vm.defineBuiltinMethod(ValueType::Type, "from_fields",
                               [this](VM& vm, ArgsView a){ return timespan_type_from_fields(vm,a); },
                               false, funcType, defaults,
                               typeMethodDecl(timeSpanTypeValue, "from_fields"));
    }

    // List methods filter/map/reduce — read-only on self
    vm.defineBuiltinMethod(ValueType::List, "filter",
                           [this](VM& vm, ArgsView a){ return list_filter_builtin(vm, a); },
                           false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);
    vm.defineBuiltinMethod(ValueType::List, "map",
                           [this](VM& vm, ArgsView a){ return list_map_builtin(vm, a); },
                           false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);
    vm.defineBuiltinMethod(ValueType::List, "reduce",
                           [this](VM& vm, ArgsView a){ return list_reduce_builtin(vm, a); },
                           false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x3);
}

Value ModuleSys::print_builtin(VM& vm, ArgsView args)
{
    if(args.size() > 4)
        throw std::invalid_argument("print expects at most 4 arguments");

    // value param is typed :string — async user-defined conversions (operator->string)
    // are handled by callNativeFn's NativeParamConversionState before we get here.
    std::string valueStr = "";
    std::string endStr = "\n";
    bool flush = false;
    bool here = false;

    if(args.size() >= 1)
        valueStr = toString(args[0]);

    if(args.size() == 2 && args[1].isBool()) {
        flush = args[1].asBool();
    } else {
        if(args.size() >= 2)
            endStr = toString(args[1]);
        if(args.size() >= 3)
            flush = toType(ValueType::Bool, args[2], false).asBool();
        if(args.size() >= 4)
            here = toType(ValueType::Bool, args[3], false).asBool();
    }

#ifdef ROXAL_COMPUTE_SERVER
    VM::emitPrintOutput(valueStr + endStr, flush, here);
#else
    (void)vm;
    (void)here;
    std::cout << valueStr << endStr;
    if(flush)
        std::cout << std::flush;
#endif
    return Value::nilVal();
}

Value ModuleSys::len_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("len expects single argument");

    Value v (args[0]);
    int32_t len {1};

    switch (v.type()) {
        case ValueType::String: len = asStringObj(v)->length(); break;
        case ValueType::List: len = asList(v)->length(); break;
        case ValueType::Dict: len = asDict(v)->length(); break;
        case ValueType::Vector: len = asVector(v)->length(); break;
        case ValueType::Tensor: len = asTensor(v)->numel(); break;
        case ValueType::Range: {
            len = asRange(v)->length();
            if (len<0) return Value::nilVal(); // has no defined length
        } break;
        default:
#ifdef DEBUG_BUILD
        std::cerr << "Unhandled type in len():" << v.typeName() << std::endl;
#endif
        ;
    }

    return Value::intVal(len);
}

Value ModuleSys::help_builtin(VM& vm, ArgsView args)
{
    if (args.size() == 0) {
        ObjModuleType* module = vm.moduleType();
        auto entries = collectModuleEntries(module);
        std::string listing = formatSymbolEntries(entries, 0, 100, "<no symbols>");
        // When called at the REPL with no user-defined symbols yet, the
        // bare placeholder isn't very informative. Hint at where to find
        // help on the REPL itself.
        if (entries.empty() && vm.replModuleType() == module) {
            listing += "\n(use /help for REPL commands)";
        }
        return Value::stringVal(toUnicodeString(listing));
    }

    if (args.size() != 1)
        throw std::invalid_argument("help expects zero or one argument");

    Value target = args[0];

    if (isCallableValue(target)) {
        CallableInfo info = describeCallable(target);
        std::string result = info.signature.value_or("");
        if (!info.doc.empty()) {
            if (!result.empty())
                result += "\n";
            result += info.doc;
        }
        return Value::stringVal(toUnicodeString(result));
    }

    if (isModuleType(target)) {
        return Value::stringVal(toUnicodeString(moduleHelpString(asModuleType(target))));
    }

    bool isInstance = false;
    if (isObjectInstance(target)) {
        target = asObjectInstance(target)->instanceType;
        isInstance = true;
    } else if (isActorInstance(target)) {
        target = asActorInstance(target)->instanceType;
        isInstance = true;
    }

    if (isObjectType(target)) {
        return Value::stringVal(toUnicodeString(typeHelpString(asObjectType(target), isInstance)));
    }

    SymbolEntry entry;
    entry.type = describeValueType(target, &entry.doc);
    std::string formatted = formatSymbolEntries(std::vector<SymbolEntry>{entry}, 0);
    return Value::stringVal(toUnicodeString(formatted));
}

Value ModuleSys::clone_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("clone takes a single argument (the value to deep-copy)");

    ptr<CloneContext> ctx = make_ptr<CloneContext>();
    return args[0].clone(ctx);
}

Value ModuleSys::ignore_builtin(VM& vm, ArgsView args)
{
    // ignore(value) suppresses statement-position auto-triggering for the
    // argument. After we return nil, the surrounding StmtAction sees nil
    // and pops without invoking either auto-await (futures) or a
    // statement-action method (user types).
    //
    // Strict check: the argument must be either a future, an instance of a
    // type that has a `statement action` method (possibly inherited), or
    // nil.
    //
    // Nil is accepted silently: actor proc calls currently return nil
    // internally and we don't want a refactor that flips a func to a proc
    // to break every `ignore(...)` call site.
    (void)vm;
    if (args.size() != 1)
        throw std::invalid_argument("ignore expects exactly one argument");

    const Value& v = args[0];
    bool ok = false;

    if (v.isNil() || isFuture(v)) {
        ok = true;
    } else {
        ObjObjectType* otype = nullptr;
        if (isObjectInstance(v))
            otype = asObjectType(asObjectInstance(v)->instanceType);
        else if (isActorInstance(v))
            otype = asObjectType(asActorInstance(v)->instanceType);
        if (otype) {
            for (ObjObjectType* t = otype; t; ) {
                if (t->statementActionMethodHash >= 0) {
                    ok = true;
                    break;
                }
                t = t->superType.isNil() ? nullptr : asObjectType(t->superType);
            }
        }
    }

    if (!ok) {
        throw std::runtime_error(
            "ignore() argument has type " + v.typeName() +
            " — which has no statement-action behaviour to suppress; remove ignore(...)");
    }

    return Value::nilVal();
}

Value ModuleSys::wait_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 6)
        throw std::invalid_argument("wait expects 6 arguments");

    auto* thread = VM::thread.get();
    if (!thread)
        throw std::runtime_error("wait requires an active thread");

    Value duration = args[0];
    int64_t s = toType(ValueType::Int, args[1], false).asInt();
    int64_t ms = toType(ValueType::Int, args[2], false).asInt();
    int64_t us = toType(ValueType::Int, args[3], false).asInt();
    int64_t ns = toType(ValueType::Int, args[4], false).asInt();
    Value waitTarget = args[5];

    bool hasDuration = !duration.isNil();
    if (hasDuration && (s != 0 || ms != 0 || us != 0 || ns != 0))
        throw std::invalid_argument("wait duration cannot be combined with s, ms, us, or ns");

    int64_t totalus = 0;
    if (hasDuration) {
        if (duration.isNumber()) {
            double seconds = duration.isReal() ? duration.asReal() : static_cast<double>(duration.asInt());
            totalus = microsFromSeconds(seconds, "wait duration");
        } else if (isObjectInstance(duration) && ::instanceOf(asObjectInstance(duration), quantityTypeObj)) {
            totalus = quantityTimeMicros(duration);
        } else {
            throw std::invalid_argument("wait duration must be nil, int, real, or a time quantity");
        }
    } else {
        totalus = addChecked(totalus, waitFieldMicros(s, MICROS_PER_SECOND));
        totalus = addChecked(totalus, waitFieldMicros(ms, MICROS_PER_MILLISECOND));
        totalus = addChecked(totalus, us);
        totalus = addChecked(totalus, ns / 1000);
    }

    auto microSecs { TimeDuration::microSecs(totalus) };
    bool hasDelay = microSecs.microSecs() > 0;

    thread->threadSleep = false;
    thread->pendingWaitFor = Value::nilVal();
    thread->waitSuspension.clear();

    // wait() suspends via the VM dispatcher rather than blocking in native code.
    auto suspendWait = [&](Thread::WaitSuspension::ResultMode mode, Value storedValue = Value::nilVal()) {
        thread->waitSuspension.active = true;
        thread->waitSuspension.resultMode = mode;
        thread->waitSuspension.storedValue = storedValue;
    };

    if (hasDelay) {
        thread->threadSleepUntil = TimePoint::currentTime() + microSecs;
        thread->threadSleep = true;
    }

    if (waitTarget.isNil()) {
        if (hasDelay)
            suspendWait(Thread::WaitSuspension::ResultMode::Nil);
        return Value::nilVal();
    }

    if (isFuture(waitTarget)) {
        if (!hasDelay) {
            auto status = vm.tryResolveValue(waitTarget);
            if (status == FutureStatus::Error)
                return Value::nilVal();
            if (status == FutureStatus::Resolved)
                return waitTarget;
        }

        thread->pendingWaitFor = waitTarget;
        suspendWait(Thread::WaitSuspension::ResultMode::PendingWaitTarget);
        return Value::nilVal();
    }

    if (hasDelay) {
        suspendWait(Thread::WaitSuspension::ResultMode::StoredValue, waitTarget);
        return Value::nilVal();
    }

    return waitTarget;
}

Value ModuleSys::is_ready_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("is_ready expects 1 argument");

    const Value& futValue = args[0];
    if (!isFuture(futValue))
        throw std::invalid_argument("is_ready expects a future argument");

    ObjFuture* fut = asFuture(futValue);
    if (!fut->future.valid())
        return Value::falseVal();

    auto status = fut->future.wait_for(std::chrono::microseconds(0));
    return status == std::future_status::ready ? Value::trueVal() : Value::falseVal();
}

// Helpers for sys.allof / sys.anyof.
//
// Acceptable awaitable kinds: future, event type, bool signal. (A signal
// returned by a comparison expression like `c > 20` is itself an ObjSignal
// whose dataflow output is bool.)
static bool isAwaitable(const Value& v)
{
    return isFuture(v) || isEventType(v) || isSignal(v);
}

// Walk args and build a single flat list of awaitables. Each top-level arg
// can be an awaitable directly, or a list of awaitables (flattened one level).
static std::vector<Value> flattenAwaitables(ArgsView args, const char* fnName)
{
    std::vector<Value> awaitables;

    auto append = [&](const Value& v) {
        if (!isAwaitable(v)) {
            throw std::runtime_error(std::string(fnName) +
                ": each argument must be a future, event type, or signal — got " +
                v.typeName());
        }
        awaitables.push_back(v);
    };

    // ArgsView when registered as a single variadic param yields one List
    // value containing all positional args. Detect both shapes.
    auto walk = [&](const Value& v) {
        if (isList(v)) {
            ObjList* list = asList(v);
            for (int32_t i = 0; i < list->length(); i++) {
                Value elem = list->getElement(i);
                if (isList(elem)) {
                    ObjList* inner = asList(elem);
                    for (int32_t j = 0; j < inner->length(); j++)
                        append(inner->getElement(j));
                } else {
                    append(elem);
                }
            }
        } else {
            append(v);
        }
    };

    for (size_t i = 0; i < args.size(); i++)
        walk(args[i]);

    return awaitables;
}

// Wire one slot of a freshly-constructed combinator to its input awaitable.
// `combinatorVal` must be a strong Value reference to the combinator (used
// to derive a weak ref for storage in the future-waiter or
// HandlerRegistration). The combinator itself is also passed for direct
// access to its slots.
static void wireCombinatorSlot(VM& vm, Thread* thread,
                               ObjCombinator* combinator,
                               const Value& combinatorVal,
                               uint32_t slotIndex, const Value& input)
{
    auto& slot = combinator->slots[slotIndex];
    slot.input = input;

    if (isFuture(input)) {
        slot.kind = ObjCombinator::SlotKind::Future;
        ObjFuture* fut = asFuture(input);
        fut->addCombinatorWaiter(combinatorVal.weakRef(), slotIndex);
        // Synchronous already-ready check: wakeWaiters fires on set_value but
        // if the future was already resolved before addCombinatorWaiter we'd
        // miss it. tryResolveValue is non-blocking and returns Resolved when
        // the future is ready, replacing `sample` with the resolved Value.
        Value sample = input;
        auto status = vm.tryResolveValue(sample);
        if (status == FutureStatus::Resolved) {
            combinator->notifySlotReady(slotIndex, sample);
        }
        return;
    }

    // Event-type or signal. Both register a HandlerRegistration with a
    // fresh closure wrapping the combinator-relay sentinel function so the
    // dispatcher can recognise the relay and route to notifySlotReady.
    Value eventVal;
    std::optional<Value> matchValue;
    if (isSignal(input)) {
        slot.kind = ObjCombinator::SlotKind::Signal;
        ObjSignal* sigObj = asSignal(input);
        matchValue = Value::trueVal();  // bool signal predicates fire on true
        sigObj->ensureChangeEventType();
        eventVal = sigObj->changeEventType;
        thread->eventToSignal[eventVal.weakRef()] = input.weakRef();
    } else {
        slot.kind = ObjCombinator::SlotKind::EventType;
        eventVal = input;
    }

    Value relayFn = vm.getCombinatorRelayFunction();
    if (relayFn.isNil())
        throw std::runtime_error("combinator relay function not initialised");
    Value relayClosure = Value::closureVal(relayFn);
    ObjClosure* cl = asClosure(relayClosure);
    cl->handlerThread = thread->ptr_from_this();

    // Combinator slot holds the closure strongly so cancel() can drop it.
    // HandlerRegistration also holds a strong ref so the closure survives
    // until the registration is removed (either at fire-time cleanup or by
    // pruneEventRegistrations seeing a fulfilled combinatorTarget).
    slot.relayClosure = relayClosure;

    Thread::HandlerRegistration reg;
    reg.closure = relayClosure;
    reg.matchValue = matchValue;
    reg.combinatorTarget = combinatorVal.weakRef();
    reg.combinatorSlot = slotIndex;
    reg.oneShot = true;

    Value key = eventVal.weakRef();
    thread->eventHandlers[key].push_back(std::move(reg));

    ObjEventType* ev = asEventType(eventVal);
    ev->subscribers.push_back(relayClosure.weakRef());

    // Synchronous already-true check for signal predicates.
    if (slot.kind == ObjCombinator::SlotKind::Signal) {
        ObjSignal* sigObj = asSignal(input);
        if (sigObj->signal) {
            Value cur = sigObj->signal->lastValue();
            if (cur.isBool() && cur.asBool()) {
                combinator->notifySlotReady(slotIndex, cur);
            }
        }
    }
}

Value ModuleSys::allof_builtin(VM& vm, ArgsView args)
{
    auto* thread = VM::thread.get();
    if (!thread)
        throw std::runtime_error("allof requires an active thread");

    std::vector<Value> awaitables = flattenAwaitables(args, "allof");

    // allof() with zero awaitables resolves immediately to [].
    if (awaitables.empty()) {
        std::promise<Value> p;
        p.set_value(Value::listVal());
        return Value::objVal(newFutureObj(p.get_future().share()));
    }

    Value combinatorVal = Value::objVal(newCombinatorObj(ObjCombinator::Mode::All, awaitables.size()));
    ObjCombinator* combinator = asCombinator(combinatorVal);

    auto outFut = newFutureObj(combinator->sharedFuture());
    outFut->producer = combinatorVal;
    Value futureVal = Value::objVal(std::move(outFut));
    combinator->outputFuture = futureVal.weakRef();

    for (size_t i = 0; i < awaitables.size(); i++) {
        wireCombinatorSlot(vm, thread, combinator, combinatorVal,
                           static_cast<uint32_t>(i), awaitables[i]);
    }

    return futureVal;
}

Value ModuleSys::anyof_builtin(VM& vm, ArgsView args)
{
    auto* thread = VM::thread.get();
    if (!thread)
        throw std::runtime_error("anyof requires an active thread");

    std::vector<Value> awaitables = flattenAwaitables(args, "anyof");

    if (awaitables.empty())
        throw std::runtime_error("anyof requires at least one awaitable");

    Value combinatorVal = Value::objVal(newCombinatorObj(ObjCombinator::Mode::Any, awaitables.size()));
    ObjCombinator* combinator = asCombinator(combinatorVal);

    auto outFut = newFutureObj(combinator->sharedFuture());
    outFut->producer = combinatorVal;
    Value futureVal = Value::objVal(std::move(outFut));
    combinator->outputFuture = futureVal.weakRef();

    for (size_t i = 0; i < awaitables.size(); i++) {
        wireCombinatorSlot(vm, thread, combinator, combinatorVal,
                           static_cast<uint32_t>(i), awaitables[i]);
    }

    return futureVal;
}

// Diagnostic helper: returns the number of entries in an event type's
// subscribers list (alive + dead weak refs combined). Used by tests that
// verify combinator one-shot subscriptions get cleaned up.
Value ModuleSys::event_subscriber_count_builtin(VM& /*vm*/, ArgsView args)
{
    if (args.size() != 1 || !isEventType(args[0]))
        throw std::runtime_error("_event_subscriber_count expects an event type argument");
    ObjEventType* ev = asEventType(args[0]);
    return Value::intVal(static_cast<int32_t>(ev->subscribers.size()));
}

Value ModuleSys::fork_builtin(VM& vm, ArgsView args)
{
    if ((args.size() != 1) || !isClosure(args[0]))
        throw std::invalid_argument("fork expects single callable argument (e.g. func or proc)");

    ObjClosure* closure = asClosure(args[0]);

    // Check if closure captures any outer variables (has upvalues)
    if (!closure->upvalues.empty()) {
        throw std::runtime_error("fork cannot execute functions that capture variables from outer scopes. "
                                "The function must only use its parameters and global variables.");
    }

    ptr<Thread> newThread = make_ptr<Thread>();
    vm.threads.store(newThread->id(), newThread);
    newThread->spawn(args[0]);

    int32_t id = int32_t(newThread->id());
    return Value::intVal(id);
}

Value ModuleSys::join_builtin(VM& vm, ArgsView args)
{
    if ((args.size() != 1) || !args[0].isNumber())
        throw std::invalid_argument("join expects single numeric argument (thread id)");

    uint64_t id = args[0].asInt(); // FIXME: id is uint64

    auto count = vm.threads.erase_and_apply(id, [](ptr<Thread> t){
        t->join();
    });

    return count > 0 ? Value::trueVal() : Value::falseVal();
}

Value ModuleSys::exit_builtin(VM& vm, ArgsView args)
{
    if (args.size() > 1)
        throw std::invalid_argument("exit expects zero or one numeric argument");
    int32_t code = 0;
    if (args.size() == 1) {
        if (!args[0].isNumber())
            throw std::invalid_argument("exit code must be numeric");
        code = args[0].asInt();
    }
    vm.requestExit(code);
    return Value::nilVal();
}

Value ModuleSys::threadid_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_threadid takes no arguments");

    int32_t id = int32_t(VM::thread->id()); // FIXME: id is uint64
    return Value::intVal(id);
}

Value ModuleSys::stacktrace_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("stacktrace takes no arguments");

    return vm.captureStacktrace();
}

Value ModuleSys::stackdepth_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_stackdepth takes no arguments");

    int32_t depth = int32_t(VM::thread->stackTop - VM::thread->stack.begin());
    return Value::intVal(depth);
}

// Private/internal: call a Roxal method by name on an object, with args, via
// VM::invokeMethod. Exercises the receiver-aware method-invoke path (and is handy
// for dynamic dispatch). Mirrors how module C++ calls a Roxal method from native code.
Value ModuleSys::invoke_method_builtin(VM& vm, ArgsView args)
{
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("_invoke_method(obj, name, args=nil) expects an object and a string method name");
    const Value& receiver = args[0];
    icu::UnicodeString name = asStringObj(args[1])->s;
    std::vector<Value> callArgs;
    if (args.size() >= 3 && !args[2].isNil()) {
        if (!isList(args[2]))
            throw std::invalid_argument("_invoke_method: the third argument must be a list of args");
        ObjList* l = asList(args[2]);
        for (int32_t i = 0; i < l->length(); ++i)
            callArgs.push_back(l->getElement(static_cast<size_t>(i)));
    }
    auto [status, result] = vm.invokeMethod(receiver, name, callArgs);
    if (status != ExecutionStatus::OK)
        throw std::runtime_error("_invoke_method: failed to invoke method '" + toUTF8StdString(name) + "'");
    return result;
}

// Test-only counters for the ChangeNotifier path (driven by _watch_property/_watch_count).
// Each entry is a small shared counter captured by the property's change callback.
static std::vector<std::shared_ptr<std::atomic<int>>> s_watchCounters;

Value ModuleSys::watch_property_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() != 2 || !isObjectInstance(args[0]) || !isString(args[1]))
        throw std::invalid_argument("_watch_property(obj, name) expects an object and a property name");
    ObjectInstance* obj = asObjectInstance(args[0]);
    icu::UnicodeString name = asStringObj(args[1])->s;

    auto counter = std::make_shared<std::atomic<int>>(0);
    int32_t id = static_cast<int32_t>(s_watchCounters.size());
    s_watchCounters.push_back(counter);

    // Observe via the lightweight ChangeNotifier (no dataflow signal is created).
    obj->observePropertyChange(name.hashCode(), toUTF8StdString(name),
        [counter](TimePoint, ptr<df::Signal>, const Value&) {
            counter->fetch_add(1, std::memory_order_relaxed);
        });
    return Value(id);
}

Value ModuleSys::watch_count_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() != 1 || !args[0].isInt())
        throw std::invalid_argument("_watch_count(id) expects an integer watch id");
    int32_t id = static_cast<int32_t>(args[0].asInt());
    if (id < 0 || id >= static_cast<int32_t>(s_watchCounters.size()))
        throw std::invalid_argument("_watch_count: invalid watch id");
    return Value(static_cast<int32_t>(s_watchCounters[id]->load(std::memory_order_relaxed)));
}

Value ModuleSys::runtests_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("_runtests expects single string argument");

    auto suite = toUTF8StdString(asStringObj(args[0])->s);

    if (suite == "gc_coordination") {
        const bool pass = SimpleMarkSweepGC::instance().runCoordinationSelfTest();
        std::cout << "GC coordination self-test "
                  << (pass ? "PASSED" : "FAILED") << std::endl;
    }
    else if (suite == "gc_scanner") {
        const bool pass = SimpleMarkSweepGC::instance().runScannerRecallSelfTest();
        std::cout << "GC scanner recall self-test "
                  << (pass ? "PASSED" : "FAILED") << std::endl;
    }
    else if (suite == "dataflow") {
        // TODO: Dataflow tests have been moved out - need to implement new roxal-based tests
        std::cout << "Dataflow tests temporarily disabled during Func class elimination" << std::endl;
        if (auto engine = df::DataflowEngine::instance(false))
            engine->clear();
    }
    else if (suite == "conversions") {
        auto results = testConversions();

        int passes = 0;
        int fails = 0;
        for (const auto& result : results) {
            std::cout << "Test: " << std::get<0>(result) << " ";
            bool passed = std::get<1>(result);
            if (passed) {
                std::cout << "passed";
                passes++;
            }
            else {
                std::cout << "failed";
                fails++;
            }
            std::cout << " " << std::get<2>(result) << std::endl;
        }

        std::cout << "Passed " << passes << " failed " << fails << std::endl;
    }
    else if (suite == "serialize") {
        auto results = testValueSerialization();

        int passes = 0;
        int fails = 0;
        for (const auto& result : results) {
            std::cout << "Test: " << std::get<0>(result) << " ";
            bool passed = std::get<1>(result);
            if (passed) {
                std::cout << "passed";
                passes++;
            }
            else {
                std::cout << "failed";
                fails++;
            }
            std::cout << " " << std::get<2>(result) << std::endl;
        }

        std::cout << "Passed " << passes << " failed " << fails << std::endl;
    }
    else if (suite == "orient") {
        auto results = testOrientConversions();

        int passes = 0;
        int fails = 0;
        for (const auto& result : results) {
            std::cout << "Test: " << std::get<0>(result) << " ";
            bool passed = std::get<1>(result);
            if (passed) {
                std::cout << "passed";
                passes++;
            }
            else {
                std::cout << "failed";
                fails++;
            }
            std::cout << " " << std::get<2>(result) << std::endl;
        }

        std::cout << "Passed " << passes << " failed " << fails << std::endl;
    }
    else if (suite == "rt_execution") {
        // RT Execution tests for tickFor() deadline-aware execution
        int passes = 0;
        int fails = 0;

        auto reportTest = [&](const std::string& name, bool passed, const std::string& detail = "") {
            std::cout << "Test: " << name << " " << (passed ? "passed" : "FAILED");
            if (!detail.empty()) std::cout << " - " << detail;
            std::cout << std::endl;
            if (passed) passes++; else fails++;
        };

        // Clear synchronous-execution guard so runFor() works from within
        // this test builtin (no FC/RoxalLoop running during tests).
        vm.setSynchronousExecution(false);

        auto& engine = *df::DataflowEngine::instance();
        engine.clear();

        // Test 1: hasYieldedWork accessor - initially no work
        {
            bool initiallyNoWork = !engine.hasYieldedWork();
            reportTest("hasYieldedWork_initial", initiallyNoWork,
                initiallyNoWork ? "" : "Expected no yielded work initially");
        }

        // Test 2: tickFor with native func that takes time
        {
            engine.clear();

            // Create a simple signal at 100Hz
            auto inputSignal = df::Signal::newSourceSignal(100.0, Value::intVal(0), "test_input");

            // Create a native func that busy-waits for a bit
            auto slowNativeFunc = [](const df::Values& inputs) -> df::Values {
                // Busy wait for about 1ms
                auto start = TimePoint::currentTime();
                volatile int sum = 0;
                while ((TimePoint::currentTime() - start) < TimeDuration::milliSecs(1)) {
                    sum = sum + 1;  // Avoid deprecated ++ on volatile
                }
                return { inputs.empty() ? Value::intVal(0) : inputs[0] };
            };

            ptr<df::FuncNode> funcNode = make_ptr<df::FuncNode>(
                "slowNativeFunc",
                slowNativeFunc,
                std::vector<std::string>{"x"},
                df::FuncNode::ConstArgMap{},
                std::vector<ptr<df::Signal>>{ inputSignal },
                df::Names{"result"}
            );
            funcNode->addToEngine();

            // Tick with reasonable budget - native funcs don't yield, so should complete
            auto result1 = engine.tickFor(TimeDuration::milliSecs(10));
            bool completed = (result1 == df::DataflowEngine::TickResult::Complete);
            reportTest("tickFor_native_func", completed,
                "Native func result: " + std::to_string(static_cast<int>(result1)));
        }

        // Test 3: tickFor with empty network returns Complete
        {
            engine.clear();
            auto result = engine.tickFor(TimeDuration::milliSecs(1));
            bool completed = (result == df::DataflowEngine::TickResult::Complete);
            reportTest("tickFor_empty_network", completed,
                completed ? "" : "Expected Complete for empty network");
        }

        // Test 4: Network modification during yield returns Error
        // (Simulate by manually setting yield state)
        {
            engine.clear();

            // Create a signal to make tick period non-zero
            auto sig = df::Signal::newSourceSignal(100.0, Value::intVal(0), "mod_test");

            // First tick to set up state
            engine.tickFor(TimeDuration::milliSecs(1));

            // Now test: if we had yielded and network was modified, resume returns error
            // We can't easily force a yield with native funcs, but we can verify
            // that hasYieldedWork returns false after a complete tick
            bool noYieldAfterComplete = !engine.hasYieldedWork();
            reportTest("no_yield_after_complete", noYieldAfterComplete,
                noYieldAfterComplete ? "" : "Expected no yielded work after complete tick");
        }

        // Test 5: Multiple ticks work correctly
        {
            engine.clear();

            auto sig = df::Signal::newSourceSignal(100.0, Value::intVal(1), "multi_tick_test");

            auto nativeFunc = [](const df::Values& inputs) -> df::Values {
                return { inputs.empty() ? Value::intVal(0) : inputs[0] };
            };

            ptr<df::FuncNode> funcNode = make_ptr<df::FuncNode>(
                "multiTickFunc",
                nativeFunc,
                std::vector<std::string>{"x"},
                df::FuncNode::ConstArgMap{},
                std::vector<ptr<df::Signal>>{ sig },
                df::Names{"result"}
            );
            funcNode->addToEngine();

            // Do multiple ticks
            bool allComplete = true;
            for (int i = 0; i < 3; i++) {
                auto result = engine.tickFor(TimeDuration::milliSecs(10));
                if (result != df::DataflowEngine::TickResult::Complete) {
                    allComplete = false;
                    break;
                }
            }
            reportTest("multiple_ticks", allComplete,
                allComplete ? "" : "One of the ticks didn't complete");
        }

        // Test 6: Verify TickResult enum values exist
        {
            bool enumsExist = true;
            auto complete = df::DataflowEngine::TickResult::Complete;
            auto yielded = df::DataflowEngine::TickResult::Yielded;
            auto overrun = df::DataflowEngine::TickResult::Overrun;
            auto error = df::DataflowEngine::TickResult::Error;
            (void)complete; (void)yielded; (void)overrun; (void)error;
            reportTest("tick_result_enum", enumsExist);
        }

        // Test 7: Direct invokeClosure yields on short deadline
        // Test the VM's invokeClosure directly rather than through FuncNode
        {
            // Save the current thread state - setup() will replace it
            auto savedThread = VM::thread;

            // Compile and execute a script that defines a slow function
            std::stringstream source;
            source << "func slowFunc(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<10000):\n"
                   << "    sum = sum + i\n"
                   << "  return sum + x\n";

            auto setupResult = vm.setup(source, "rt_closure_yield");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("invokeClosure_yields", false, "Failed to compile");
            } else {
                // Get module type from the thread's frame BEFORE execute completes
                // After setup(), thread has one frame with the main closure
                ObjModuleType* modType = vm.moduleType();

                // Execute the script to define the function
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("invokeClosure_yields", false,
                        "execute failed: " + std::to_string(static_cast<int>(execResult)));
                } else {
                    // Get the function from saved module type's vars
                    auto closureOpt = modType->vars.load(toUnicodeString("slowFunc"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("invokeClosure_yields", false,
                            "closure not found in moduleVars, hasValue=" + std::to_string(closureOpt.has_value()));
                    } else {
                        Value closureVal = closureOpt.value();
                        // Now invoke the closure with a short deadline
                        auto deadline = TimePoint::currentTime() + TimeDuration::microSecs(50);
                        auto [result, retVal] = vm.invokeClosure(asClosure(closureVal), {Value::intVal(42)}, deadline);

                        bool yielded = (result == ExecutionStatus::Yielded);
                        VM::thread = savedThread;
                        reportTest("invokeClosure_yields", yielded,
                            "result=" + std::to_string(static_cast<int>(result)));
                    }
                }
            }
        }

        // Test 8: invokeClosure resume completes
        {
            auto savedThread = VM::thread;

            std::stringstream source;
            source << "func slowFunc2(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<10000):\n"
                   << "    sum = sum + i\n"
                   << "  return sum + x\n";

            auto setupResult = vm.setup(source, "rt_closure_resume");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("invokeClosure_resume_completes", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("invokeClosure_resume_completes", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("slowFunc2"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("invokeClosure_resume_completes", false, "closure not found");
                    } else {
                        Value closureVal = closureOpt.value();
                        // First invoke with short deadline - should yield
                        auto deadline1 = TimePoint::currentTime() + TimeDuration::microSecs(50);
                        auto [result1, retVal1] = vm.invokeClosure(asClosure(closureVal), {Value::intVal(42)}, deadline1);

                        if (result1 != ExecutionStatus::Yielded) {
                            VM::thread = savedThread;
                            reportTest("invokeClosure_resume_completes", true, "Completed without yield (fast)");
                        } else {
                            // Resume with generous deadline - should complete
                            // 10000 iterations may take 200-500ms, so give plenty of time
                            auto [remaining, _] = vm.runFor(TimeDuration::milliSecs(1000));
                            bool completed = (remaining == ExecutionStatus::OK);

                            VM::thread = savedThread;
                            reportTest("invokeClosure_resume_completes", completed,
                                "resume_result=" + std::to_string(static_cast<int>(remaining)));
                        }
                    }
                }
            }
        }

        // Test 9: Multi-resume with many small time slices
        {
            auto savedThread = VM::thread;

            std::stringstream source;
            source << "func slowFunc3(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<10000):\n"  // Same as other tests
                   << "    sum = sum + i\n"
                   << "  return sum + x\n";

            auto setupResult = vm.setup(source, "rt_multi_resume");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("invokeClosure_multi_resume", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("invokeClosure_multi_resume", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("slowFunc3"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("invokeClosure_multi_resume", false, "closure not found");
                    } else {
                        Value closureVal = closureOpt.value();
                        // First invoke with short deadline
                        auto deadline1 = TimePoint::currentTime() + TimeDuration::microSecs(50);
                        auto [result, retVal] = vm.invokeClosure(asClosure(closureVal), {Value::intVal(1)}, deadline1);

                        int yieldCount = (result == ExecutionStatus::Yielded) ? 1 : 0;
                        const int maxIterations = 10000; // 50000 iterations needs more resume cycles

                        for (int i = 0; i < maxIterations && result == ExecutionStatus::Yielded; ++i) {
                            auto [res, _] = vm.runFor(TimeDuration::microSecs(100)); // Give more time per cycle
                            result = res;
                            if (result == ExecutionStatus::Yielded)
                                yieldCount++;
                        }

                        bool completed = (result == ExecutionStatus::OK);
                        bool multipleYields = (yieldCount > 1);

                        VM::thread = savedThread;
                        // Output deterministic result (yieldCount varies by machine speed)
                        reportTest("invokeClosure_multi_resume", completed && multipleYields,
                            "multipleYields=" + std::to_string(multipleYields) + ", completed=" + std::to_string(completed));
                    }
                }
            }
        }

        // Test 10: Closure state preserved across yields (correct sum)
        {
            auto savedThread = VM::thread;

            std::stringstream source;
            source << "func computeSum(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<1000):\n"
                   << "    sum = sum + i\n"
                   << "  return sum\n";

            auto setupResult = vm.setup(source, "rt_state_preserve");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("closure_state_preserved", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("closure_state_preserved", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("computeSum"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("closure_state_preserved", false, "closure not found");
                    } else {
                        Value closureVal = closureOpt.value();
                        // Invoke with short deadline
                        auto deadline = TimePoint::currentTime() + TimeDuration::microSecs(50);
                        auto [result, returnVal] = vm.invokeClosure(asClosure(closureVal), {Value::intVal(0)}, deadline);

                        const int maxIterations = 1000;
                        for (int i = 0; i < maxIterations && result == ExecutionStatus::Yielded; ++i) {
                            auto [res, val] = vm.runFor(TimeDuration::microSecs(100));
                            result = res;
                            returnVal = val;  // Capture return value when completed
                        }

                        if (result != ExecutionStatus::OK) {
                            VM::thread = savedThread;
                            reportTest("closure_state_preserved", false,
                                "Did not complete, result=" + std::to_string(static_cast<int>(result)));
                        } else {
                            // Return value is now captured from runFor()
                            int64_t expectedSum = 499500; // sum of 0..999

                            bool correctValue = returnVal.isInt() && returnVal.asInt() == expectedSum;
                            VM::thread = savedThread;
                            reportTest("closure_state_preserved", correctValue,
                                "output=" + (returnVal.isInt() ? std::to_string(returnVal.asInt()) : "non-int") +
                                ", expected=" + std::to_string(expectedSum));
                        }
                    }
                }
            }
        }

        // =========================================================================
        // Tests 11-14: Full DataflowEngine → FuncNode → VM path with closures
        // These tests verify that tickFor() correctly yields and resumes when
        // FuncNodes use Roxal closures instead of native functions.
        // =========================================================================

        // Test 11: FuncNode with closure yields on short deadline via tickFor
        {
            engine.clear();
            auto savedThread = VM::thread;

            // Compile a slow Roxal function
            std::stringstream source;
            source << "func dfSlowFunc(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<10000):\n"
                   << "    sum = sum + i\n"
                   << "  return sum + x\n";

            auto setupResult = vm.setup(source, "df_closure_yield");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("df_closure_yields", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("df_closure_yields", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("dfSlowFunc"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("df_closure_yields", false, "closure not found");
                    } else {
                        Value closureVal = closureOpt.value();
                        // DON'T restore savedThread here - let engine operations use fresh thread

                        // Create input signal at 100Hz
                        auto inputSignal = df::Signal::newSourceSignal(100.0, Value::intVal(42), "df_closure_input");

                        // Create FuncNode with the closure (not a native function)
                        ptr<df::FuncNode> funcNode = make_ptr<df::FuncNode>(
                            "dfSlowClosureFunc",
                            closureVal,  // Roxal closure
                            df::FuncNode::ConstArgMap{},
                            std::vector<ptr<df::Signal>>{ inputSignal }
                        );
                        funcNode->addToEngine();

                        // Tick with very short budget - should yield mid-closure
                        auto result = engine.tickFor(TimeDuration::microSecs(50));
                        bool yielded = (result == df::DataflowEngine::TickResult::Yielded);
                        bool hasWork = engine.hasYieldedWork();

                        // Restore thread AFTER engine operations
                        VM::thread = savedThread;
                        reportTest("df_closure_yields", yielded && hasWork,
                            "result=" + std::to_string(static_cast<int>(result)) +
                            ", hasWork=" + std::to_string(hasWork));
                    }
                }
            }
        }

        // Test 12: FuncNode closure resumes and completes via tickFor
        {
            engine.clear();
            auto savedThread = VM::thread;

            std::stringstream source;
            source << "func dfSlowFunc2(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<10000):\n"
                   << "    sum = sum + i\n"
                   << "  return sum + x\n";

            auto setupResult = vm.setup(source, "df_closure_resume");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("df_closure_resume_completes", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("df_closure_resume_completes", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("dfSlowFunc2"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("df_closure_resume_completes", false, "closure not found");
                    } else {
                        Value closureVal = closureOpt.value();
                        // DON'T restore savedThread here - let engine operations use fresh thread

                        auto inputSignal = df::Signal::newSourceSignal(100.0, Value::intVal(42), "df_resume_input");

                        ptr<df::FuncNode> funcNode = make_ptr<df::FuncNode>(
                            "dfResumeClosureFunc",
                            closureVal,
                            df::FuncNode::ConstArgMap{},
                            std::vector<ptr<df::Signal>>{ inputSignal }
                        );
                        funcNode->addToEngine();

                        // First tick with short budget - should yield
                        auto result = engine.tickFor(TimeDuration::microSecs(50));

                        if (result != df::DataflowEngine::TickResult::Yielded) {
                            // Completed immediately (very fast machine)
                            VM::thread = savedThread;
                            reportTest("df_closure_resume_completes", true, "Completed without yield");
                        } else {
                            // Resume with generous budget - should complete
                            result = engine.tickFor(TimeDuration::milliSecs(1000));
                            bool completed = (result == df::DataflowEngine::TickResult::Complete);
                            bool noMoreWork = !engine.hasYieldedWork();

                            VM::thread = savedThread;
                            reportTest("df_closure_resume_completes", completed && noMoreWork,
                                "result=" + std::to_string(static_cast<int>(result)) +
                                ", noMoreWork=" + std::to_string(noMoreWork));
                        }
                    }
                }
            }
        }

        // Test 13: FuncNode closure multi-resume with small time slices
        {
            engine.clear();
            auto savedThread = VM::thread;

            std::stringstream source;
            // Use enough work and small enough slices to force multiple resumptions.
            source << "func dfSlowFunc3(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<10000):\n"
                   << "    sum = sum + i\n"
                   << "  return sum + x\n";

            auto setupResult = vm.setup(source, "df_multi_resume");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("df_closure_multi_resume", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("df_closure_multi_resume", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("dfSlowFunc3"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("df_closure_multi_resume", false, "closure not found");
                    } else {
                        Value closureVal = closureOpt.value();
                        // DON'T restore savedThread here - let engine operations use fresh thread

                        // 1Hz (1s period): the assertion is that a sliced
                        // closure completes across MULTIPLE resumes, not that
                        // the work fits a tight period or slice count.  Fixed
                        // per-slice overhead dominates 500us slices in a
                        // debug build (only a few dozen script iterations of
                        // progress per slice), so both the period and the
                        // resume cap below carry generous headroom -- this
                        // subtest sat exactly at the old caps and flipped
                        // with unrelated binary-layout changes.
                        auto inputSignal = df::Signal::newSourceSignal(1.0, Value::intVal(1), "df_multi_input");

                        ptr<df::FuncNode> funcNode = make_ptr<df::FuncNode>(
                            "dfMultiResumeFunc",
                            closureVal,
                            df::FuncNode::ConstArgMap{},
                            std::vector<ptr<df::Signal>>{ inputSignal }
                        );
                        funcNode->addToEngine();

                        // First tick with a very short budget so the closure must yield.
                        auto result = engine.tickFor(TimeDuration::microSecs(100));
                        int yieldCount = (result == df::DataflowEngine::TickResult::Yielded) ? 1 : 0;

                        // Keep resume slices short enough that completion requires multiple passes.
                        const int maxIterations = 1500;
                        for (int i = 0; i < maxIterations && result == df::DataflowEngine::TickResult::Yielded; ++i) {
                            result = engine.tickFor(TimeDuration::microSecs(500));
                            if (result == df::DataflowEngine::TickResult::Yielded)
                                yieldCount++;
                        }

                        bool completed = (result == df::DataflowEngine::TickResult::Complete);
                        bool multipleYields = (yieldCount > 1);

                        VM::thread = savedThread;
                        reportTest("df_closure_multi_resume", completed && multipleYields,
                            "multipleYields=" + std::to_string(multipleYields) +
                            ", completed=" + std::to_string(completed));
                    }
                }
            }
        }

        // Test 14: FuncNode closure output signal has correct value after yield/resume
        {
            engine.clear();
            auto savedThread = VM::thread;

            std::stringstream source;
            source << "func dfComputeSum(x: int) -> int:\n"
                   << "  var sum = 0\n"
                   << "  for i in range(..<1000):\n"
                   << "    sum = sum + i\n"
                   << "  return sum + x\n";

            auto setupResult = vm.setup(source, "df_output_check");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("df_closure_output_correct", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("df_closure_output_correct", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("dfComputeSum"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("df_closure_output_correct", false, "closure not found");
                    } else {
                        Value closureVal = closureOpt.value();
                        // DON'T restore savedThread here - let engine operations use fresh thread

                        int64_t inputValue = 42;
                        // Use 10Hz (100ms period) to allow time for yield/resume without overrun
                        auto inputSignal = df::Signal::newSourceSignal(10.0, Value::intVal(inputValue), "df_output_input");

                        ptr<df::FuncNode> funcNode = make_ptr<df::FuncNode>(
                            "dfOutputCheckFunc",
                            closureVal,
                            df::FuncNode::ConstArgMap{},
                            std::vector<ptr<df::Signal>>{ inputSignal }
                        );
                        funcNode->addToEngine();

                        // Get output signal
                        auto outputs = funcNode->outputs();
                        if (outputs.empty()) {
                            VM::thread = savedThread;
                            reportTest("df_closure_output_correct", false, "No output signals");
                        } else {
                            auto outputSignal = outputs[0];

                            // Tick with short budget, then resume until complete
                            // Use reasonable time budgets to complete within tick period (100ms)
                            auto result = engine.tickFor(TimeDuration::microSecs(500));
                            const int maxIterations = 100;
                            for (int i = 0; i < maxIterations && result == df::DataflowEngine::TickResult::Yielded; ++i) {
                                result = engine.tickFor(TimeDuration::milliSecs(2));
                            }

                            if (result != df::DataflowEngine::TickResult::Complete) {
                                VM::thread = savedThread;
                                reportTest("df_closure_output_correct", false,
                                    "Did not complete, result=" + std::to_string(static_cast<int>(result)));
                            } else {
                                // Check output signal value
                                // Expected: sum of 0..999 (499500) + input (42) = 499542
                                int64_t expectedOutput = 499500 + inputValue;
                                Value outputVal = outputSignal->lastValue();

                                bool correctValue = outputVal.isInt() &&
                                                    outputVal.asInt() == expectedOutput;

                                VM::thread = savedThread;
                                reportTest("df_closure_output_correct", correctValue,
                                    "output=" + (outputVal.isInt()
                                        ? std::to_string(outputVal.asInt())
                                        : "non-int") +
                                    ", expected=" + std::to_string(expectedOutput));
                            }
                        }
                    }
                }
            }
        }

        // Test 15: wait(for=pendingFuture) yields under deadline and resumes with value
        {
            auto savedThread = VM::thread;

            std::stringstream source;
            source << "type WaitWorker actor:\n"
                   << "  func delayed(v: int, delay_ms: int=2) -> int:\n"
                   << "    wait(ms=delay_ms)\n"
                   << "    return v\n"
                   << "\n"
                   << "func waitImmediateFuture() -> int:\n"
                   << "  var w = WaitWorker()\n"
                   << "  return wait(for=w.delayed(123, 2))\n";

            auto setupResult = vm.setup(source, "rt_wait_future");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("wait_future_yields", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("wait_future_yields", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("waitImmediateFuture"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("wait_future_yields", false, "closure not found");
                    } else {
                        auto deadline = TimePoint::currentTime() + TimeDuration::microSecs(50);
                        auto [result, returnVal] = vm.invokeClosure(asClosure(closureOpt.value()), {}, deadline);

                        bool yielded = (result == ExecutionStatus::Yielded);
                        if (!yielded) {
                            VM::thread = savedThread;
                            reportTest("wait_future_yields", false,
                                "result=" + std::to_string(static_cast<int>(result)));
                        } else {
                            ExecutionStatus resumeResult = result;
                            Value resumeVal = returnVal;
                            const int maxIterations = 200;
                            for (int i = 0; i < maxIterations && resumeResult == ExecutionStatus::Yielded; ++i) {
                                auto [res, val] = vm.runFor(TimeDuration::milliSecs(10));
                                resumeResult = res;
                                resumeVal = val;
                                if (resumeResult == ExecutionStatus::Yielded)
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }
                            bool completed = (resumeResult == ExecutionStatus::OK);
                            bool correctValue = resumeVal.isInt() && resumeVal.asInt() == 123;
                            VM::thread = savedThread;
                            reportTest("wait_future_yields", completed && correctValue,
                                "completed=" + std::to_string(completed) +
                                ", result=" + std::to_string(static_cast<int>(resumeResult)) +
                                ", value=" + (resumeVal.isInt() ? std::to_string(resumeVal.asInt()) : "non-int"));
                        }
                    }
                }
            }
        }

        // Test 16: wait(delay, for=pendingFuture) yields under deadline and resumes with value
        {
            auto savedThread = VM::thread;

            std::stringstream source;
            source << "type WaitWorker2 actor:\n"
                   << "  func delayed(v: int, delay_ms: int=2) -> int:\n"
                   << "    wait(ms=delay_ms)\n"
                   << "    return v\n"
                   << "\n"
                   << "func waitDelayedFuture() -> int:\n"
                   << "  var w = WaitWorker2()\n"
                   << "  return wait(1ms, for=w.delayed(234, 2))\n";

            auto setupResult = vm.setup(source, "rt_wait_delay_future");
            if (setupResult != ExecutionStatus::OK) {
                VM::thread = savedThread;
                reportTest("wait_delay_future_yields", false, "Failed to compile");
            } else {
                ObjModuleType* modType = vm.moduleType();
                auto [execResult, _] = vm.execute();
                if (execResult != ExecutionStatus::OK) {
                    VM::thread = savedThread;
                    reportTest("wait_delay_future_yields", false, "execute failed");
                } else {
                    auto closureOpt = modType->vars.load(toUnicodeString("waitDelayedFuture"));
                    if (!closureOpt.has_value() || !isClosure(closureOpt.value())) {
                        VM::thread = savedThread;
                        reportTest("wait_delay_future_yields", false, "closure not found");
                    } else {
                        auto deadline = TimePoint::currentTime() + TimeDuration::microSecs(50);
                        auto [result, returnVal] = vm.invokeClosure(asClosure(closureOpt.value()), {}, deadline);

                        bool yielded = (result == ExecutionStatus::Yielded);
                        if (!yielded) {
                            VM::thread = savedThread;
                            reportTest("wait_delay_future_yields", false,
                                "result=" + std::to_string(static_cast<int>(result)));
                        } else {
                            ExecutionStatus resumeResult = result;
                            Value resumeVal = returnVal;
                            const int maxIterations = 200;
                            for (int i = 0; i < maxIterations && resumeResult == ExecutionStatus::Yielded; ++i) {
                                auto [res, val] = vm.runFor(TimeDuration::milliSecs(10));
                                resumeResult = res;
                                resumeVal = val;
                                if (resumeResult == ExecutionStatus::Yielded)
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }
                            bool completed = (resumeResult == ExecutionStatus::OK);
                            bool correctValue = resumeVal.isInt() && resumeVal.asInt() == 234;
                            VM::thread = savedThread;
                            reportTest("wait_delay_future_yields", completed && correctValue,
                                "completed=" + std::to_string(completed) +
                                ", result=" + std::to_string(static_cast<int>(resumeResult)) +
                                ", value=" + (resumeVal.isInt() ? std::to_string(resumeVal.asInt()) : "non-int"));
                        }
                    }
                }
            }
        }

        engine.clear();
        vm.setSynchronousExecution(true); // restore guard
        std::cout << "RT Execution tests: Passed " << passes << " failed " << fails << std::endl;
    }

    return Value::nilVal();
}

Value ModuleSys::weakref_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("weakref expects single argument");

    return args[0].weakRef();
}

Value ModuleSys::weak_alive_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("weak_alive expects single argument");

    return args[0].isAlive() ? Value::trueVal() : Value::falseVal();
}

Value ModuleSys::strongref_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("strongref expects single argument");

    return args[0].strongRef();
}

Value ModuleSys::refcount_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("_refcount expects single argument");

    Value v = args[0];
    if (!v.isObj()) return Value::intVal(-1); // non-object
    Obj* obj = v.asObj();
    if (!obj || !obj->control) return Value::intVal(-1);
    return Value::intVal(obj->control->strong.load(std::memory_order_relaxed));
}

Value ModuleSys::list_repr_builtin(VM& vm, ArgsView args)
{
    // Test/introspection helper: reports a list's internal storage representation.
    // Returns "packed" for the raw-byte representation, "boxed" otherwise.
    if (args.size() != 1)
        throw std::invalid_argument("_list_repr expects a single list argument");
    Value v = args[0];
    if (isFuture(v))
        v.resolveFuture();  // e.g. an async fileio.read result
    if (!isList(v))
        throw std::invalid_argument("_list_repr expects a single list argument");
    return Value::stringVal(icu::UnicodeString(asList(v)->isPackedBytes() ? "packed" : "boxed"));
}

Value ModuleSys::arity_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("_arity expects single argument");

    Value fn = args[0];
    if (isClosure(fn)) {
        return Value::intVal(asFunction(asClosure(fn)->function)->arity);
    }
    if (isBoundMethod(fn)) {
        ObjBoundMethod* bm = asBoundMethod(fn);
        if (isClosure(bm->method)) {
            return Value::intVal(asFunction(asClosure(bm->method)->function)->arity);
        }
    }
    throw std::invalid_argument("_arity expects a function or closure");
}

Value ModuleSys::gc_builtin(VM& vm, ArgsView args)
{
    if (!args.empty())
        throw std::invalid_argument("gc expects no arguments");

    SimpleMarkSweepGC& collector = SimpleMarkSweepGC::instance();
    collector.requestCollect();

    // From inside an RT GC-yield section (a host RT slice executing script
    // via runFor), gc() is ASYNCHRONOUS: parking at the safepoint here would
    // deadlock against the collection barrier waiting on our own section.
    // The request is made; the slice yields right after this call and the
    // collection runs off-RT.  Returns 0 (nothing freed synchronously).
    if (SimpleMarkSweepGC::inGCYieldSectionOnThisThread()) {
        return Value::intVal(0);
    }

    if (VM::thread) {
        collector.safepoint(*VM::thread);
    }

    // Deterministic quiesce for scripts: the safepoint above already waits
    // out the collection AND its reclamation (the reclaim fence); actor
    // teardown additionally passes through the lifecycle thread (worker
    // join + destruction), so wait that out too.  gc() thus means
    // "collected, reclaimed, and actor teardown complete" -- tests and
    // teardown-sensitive scripts rely on it.  Only this script-facing
    // builtin waits on the lifecycle; the collector never does.
    ThreadManager::instance().waitLifecycleIdle();

    size_t freed = collector.lastCollectionFreed();
    size_t clamped = std::min(freed, static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    return Value::intVal(static_cast<int32_t>(clamped));
}

Value ModuleSys::gc_config_builtin(VM& vm, ArgsView args)
{
    SimpleMarkSweepGC& collector = SimpleMarkSweepGC::instance();

    if (args.empty()) {
        std::uint64_t bytes = collector.autoTriggerThreshold();
        if (bytes == 0) {
            return Value::nilVal();
        }
        std::uint64_t kilobytes = (bytes + 1023) / 1024;
        std::uint64_t clamped = std::min<std::uint64_t>(kilobytes, static_cast<std::uint64_t>(std::numeric_limits<int32_t>::max()));
        return Value::intVal(static_cast<int32_t>(clamped));
    }

    if (args.size() != 1) {
        throw std::invalid_argument("gc_config expects zero or one argument");
    }

    const Value& arg = args[0];
    if (arg.isNil()) {
        collector.setAutoTriggerThreshold(0);
        return Value::nilVal();
    }

    if (!arg.isInt()) {
        throw std::invalid_argument("gc_config threshold (kilobytes) must be an int or nil");
    }

    int32_t threshold = arg.asInt();
    if (threshold < 0) {
        throw std::invalid_argument("gc_config threshold (kilobytes) must be non-negative");
    }

    collector.setAutoTriggerThreshold(static_cast<std::uint64_t>(threshold) * 1024ull);
    if (threshold == 0) {
        return Value::nilVal();
    }
    return Value::intVal(threshold);
}

// serialize() output carries a small self-describing header (magic byte +
// version) so a version/format mismatch is caught with a clear error rather
// than misparsing. The format is transient (like Python pickle): deserialize()
// requires the header and only accepts the current version — it does not read
// headerless or older data. The magic byte 0x52 ('R') is not a valid leading
// byte of a raw writeValue stream (always a ValueType tag <= ~30 or the Boxed
// sentinel 0xff), so a foreign/headerless stream is cleanly rejected.
static constexpr uint8_t SerializeMagic = 0x52;        // 'R'
static constexpr uint32_t SerializeFormatVersion = 3;

Value ModuleSys::serialize_builtin(VM& vm, ArgsView args)
{
    if(args.size() < 1 || args.size() > 2)
        throw std::invalid_argument("serialize expects value and optional protocol string");
    std::string protocol = "default";
    if(args.size() == 2) {
        if(!isString(args[1]))
            throw std::invalid_argument("serialize protocol must be string");
        protocol = toUTF8StdString(asStringObj(args[1])->s);
    }
    if(protocol != "default")
        throw std::invalid_argument("unknown serialization protocol");

    std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
    // Header: magic byte + little-endian format version.
    uint8_t magic = SerializeMagic;
    uint32_t version = SerializeFormatVersion;
    ss.write(reinterpret_cast<char*>(&magic), 1);
    ss.write(reinterpret_cast<char*>(&version), 4);
    ptr<SerializationContext> ctx = make_ptr<SerializationContext>();
    writeValue(ss, args[0], ctx);
    std::string data = ss.str();
    // Emit the bytes as a packed byte list (memcpy-fast, ~1 byte/elem on disk).
    std::vector<uint8_t> bytes(data.begin(), data.end());
    return Value::listVal(std::move(bytes));
}

Value ModuleSys::deserialize_builtin(VM& vm, ArgsView args)
{
    if(args.size() < 1 || args.size() > 2 || !isList(args[0]))
        throw std::invalid_argument("deserialize expects list of bytes and optional protocol string");

    std::string protocol = "default";
    if(args.size() == 2) {
        if(!isString(args[1]))
            throw std::invalid_argument("deserialize protocol must be string");
        protocol = toUTF8StdString(asStringObj(args[1])->s);
    }
    if(protocol != "default")
        throw std::invalid_argument("unknown serialization protocol");

    ObjList* lst = asList(args[0]);
    std::string data;
    if(const std::vector<uint8_t>* pb = lst->packedBytes()) {
        // Fast path: a packed byte list copies out in one shot.
        data.assign(pb->begin(), pb->end());
    } else {
        data.reserve(lst->length());
        for(int i=0;i<lst->length();i++) {
            Value v = lst->getElement(i);
            uint8_t b;
            if(v.isByte()) {
                b = v.asByte();
            } else if(v.isInt()) {
                int iv = v.asInt();
                if(iv < 0 || iv > 255)
                    throw std::runtime_error("deserialize int out of byte range");
                b = static_cast<uint8_t>(iv);
            } else {
                throw std::invalid_argument("deserialize expects list of bytes or ints");
            }
            data.push_back(static_cast<char>(b));
        }
    }

    std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
    ss.write(data.data(), data.size());
    ss.seekg(0);
    // The serialize() format is transient (like Python pickle): every stream
    // carries a magic byte + version header, and deserialize() requires it. We
    // do not attempt to read headerless or older-version data — the version
    // check exists to reject mixing, not to migrate.
    if(data.size() < 5 || static_cast<uint8_t>(data[0]) != SerializeMagic)
        throw std::runtime_error("deserialize: not a valid serialized stream (missing header)");
    uint32_t version = 0;
    std::memcpy(&version, data.data() + 1, 4);
    if(version != SerializeFormatVersion)
        throw std::runtime_error("deserialize: unsupported serialization format version "
                                 + std::to_string(version) + " (this build writes version "
                                 + std::to_string(SerializeFormatVersion) + ")");
    ss.seekg(5);
    ptr<SerializationContext> ctx = make_ptr<SerializationContext>();
    return readValue(ss, ctx);
}

// ----------------------------------------------------------------------------
// Bit / byte conversion builtins
//
// to_bytes(v, width=0, endian='little')
//   v :int|real|byte|bool|string  -> list of byte
//   Width semantics:
//     int  : 1/2/4/8 bytes (two's complement); default 8.
//     real : 4 (IEEE float32 downcast) or 8 (IEEE double); default 8.
//     bool : 1 byte (0 or 1); width must be 0 or 1.
//     byte : 1 byte; width must be 0 or 1.
//     string: UTF-8 byte sequence; width must be 0.
//   Default endian='little' matches host memory on x86/ARM and most modern
//   robotics protocols (EtherCAT, CANopen, EIP/CIP, OPC UA, ROS, DDS).
//
// from_bytes(bytes :list, dtype=real, endian :string='little', signed :bool=true)
//   bytes -> int|real|bool|string  (result is always a Roxal builtin type)
//   dtype is a type value (preferred) or a string. Accepted values:
//     int    : integer with width = len(bytes); supports 1/2/4/8.
//              signed=true (default) sign-extends; signed=false zero-extends.
//              Result is always Roxal int (signed int64).
//     real   : len 4 (IEEE float32, upcast to double) or len 8 (IEEE double)
//     bool   : len 1; nonzero -> true
//     string : UTF-8 decode
//   `signed` is only meaningful when dtype=int.
//
// bits_to_bytes(bits :list, msb_first :bool=true) -> list of byte
//   bits is a list of bool (or 0/1 int). Output length = ceil(len/8);
//   final byte zero-padded. msb_first=true (default) matches the byte([8 bits])
//   constructor convention.
//
// bytes_to_bits(bytes :list, msb_first :bool=true) -> list of bool
//   Output length = 8 * len(bytes). msb_first=true emits each byte's MSB first.
//
// lshift(v :int, n :int) -> int       arithmetic left shift; 0 <= n < 64
// rshift(v :int, n :int) -> int       arithmetic right shift; 0 <= n < 64
// ----------------------------------------------------------------------------

namespace {

// Extract a byte value from a Value (accepts byte or int in 0..255).
uint8_t valueAsByte(const Value& v, const char* ctx) {
    if (v.isByte())
        return v.asByte();
    if (v.isInt()) {
        int64_t iv = v.asInt();
        if (iv < 0 || iv > 255)
            throw std::runtime_error(std::string(ctx) + ": int out of byte range (0..255): " + std::to_string(iv));
        return static_cast<uint8_t>(iv);
    }
    throw std::invalid_argument(std::string(ctx) + ": expected byte or int element");
}

// Extract a bit value from a Value (accepts bool or int 0/1).
bool valueAsBit(const Value& v, const char* ctx) {
    if (v.isBool())
        return v.asBool();
    if (v.isInt()) {
        int64_t iv = v.asInt();
        if (iv != 0 && iv != 1)
            throw std::runtime_error(std::string(ctx) + ": int bit must be 0 or 1, got " + std::to_string(iv));
        return iv != 0;
    }
    if (v.isByte()) {
        uint8_t bv = v.asByte();
        if (bv != 0 && bv != 1)
            throw std::runtime_error(std::string(ctx) + ": byte bit must be 0 or 1");
        return bv != 0;
    }
    throw std::invalid_argument(std::string(ctx) + ": expected bool or 0/1 element");
}

// Read an optional string-valued arg with a default; throw on wrong type.
std::string optStringArg(const ArgsView& args, size_t idx, const char* name, const std::string& dflt) {
    if (idx >= args.size())
        return dflt;
    if (args[idx].isNil())
        return dflt;
    if (!isString(args[idx]))
        throw std::invalid_argument(std::string(name) + " must be a string");
    return toUTF8StdString(asStringObj(args[idx])->s);
}

// Read an optional dtype arg. Accepts a type value (preferred — e.g. `real`, `int`)
// or a string ('int'/'real'/'bool'/'string') for convenience. Returns the
// normalized lowercase string name used internally for dispatch.
std::string optDtypeArg(const ArgsView& args, size_t idx, const std::string& dflt) {
    if (idx >= args.size() || args[idx].isNil())
        return dflt;
    const Value& v = args[idx];
    if (v.type() == ValueType::Type) {
        switch (v.asType()) {
            case ValueType::Int:    return "int";
            case ValueType::Real:   return "real";
            case ValueType::Bool:   return "bool";
            case ValueType::String: return "string";
            default:
                throw std::invalid_argument("dtype must be one of the types int, real, bool, or string");
        }
    }
    if (isString(v))
        return toUTF8StdString(asStringObj(v)->s);
    throw std::invalid_argument("dtype must be a type value (int, real, bool, string) or a string");
}

// Read an optional int-valued arg with a default; throw on wrong type.
int64_t optIntArg(const ArgsView& args, size_t idx, const char* name, int64_t dflt) {
    if (idx >= args.size())
        return dflt;
    if (args[idx].isNil())
        return dflt;
    if (args[idx].isInt())
        return args[idx].asInt();
    if (args[idx].isByte())
        return static_cast<int64_t>(args[idx].asByte());
    throw std::invalid_argument(std::string(name) + " must be an int");
}

// Read an optional bool-valued arg with a default; throw on wrong type.
bool optBoolArg(const ArgsView& args, size_t idx, const char* name, bool dflt) {
    if (idx >= args.size())
        return dflt;
    if (args[idx].isNil())
        return dflt;
    if (args[idx].isBool())
        return args[idx].asBool();
    throw std::invalid_argument(std::string(name) + " must be a bool");
}

// Parse endian string. Returns true for big-endian, false for little-endian.
// 'network' is an alias for 'big' (RFC 791 / 1700 network byte order).
bool parseEndian(const std::string& s) {
    if (s == "little" || s == "LE" || s == "le")
        return false;
    if (s == "big" || s == "BE" || s == "be" || s == "network")
        return true;
    throw std::invalid_argument("endian must be 'little', 'big', or 'network', got '" + s + "'");
}

// Push uint64 as `width` little-endian bytes, optionally reverse for big-endian.
void packBytes(std::vector<Value>& out, uint64_t bits, int width, bool bigEndian) {
    size_t startIdx = out.size();
    for (int i = 0; i < width; ++i) {
        out.push_back(Value::byteVal(static_cast<uint8_t>(bits & 0xFF)));
        bits >>= 8;
    }
    if (bigEndian)
        std::reverse(out.begin() + startIdx, out.end());
}

// Read `width` bytes from list starting at offset, return as uint64 (little-endian assembly).
uint64_t unpackBytes(ObjList* lst, int offset, int width, bool bigEndian, const char* ctx) {
    if (offset + width > lst->length())
        throw std::runtime_error(std::string(ctx) + ": not enough bytes");
    uint64_t bits = 0;
    for (int i = 0; i < width; ++i) {
        int idx = bigEndian ? (offset + width - 1 - i) : (offset + i);
        uint8_t b = valueAsByte(lst->getElement(idx), ctx);
        bits |= static_cast<uint64_t>(b) << (8 * i);
    }
    return bits;
}

} // anonymous namespace

Value ModuleSys::to_bytes_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() < 1)
        throw std::invalid_argument("to_bytes expects at least 1 argument");
    int64_t width = optIntArg(args, 1, "width", 0);
    bool bigEndian = parseEndian(optStringArg(args, 2, "endian", "little"));

    const Value& v = args[0];
    std::vector<Value> out;

    // Use type() so heap-boxed wide ints / bools / reals are also accepted.
    ValueType vt = v.type();

    // bool -> 1 byte
    if (vt == ValueType::Bool) {
        if (width != 0 && width != 1)
            throw std::runtime_error("to_bytes(bool): width must be 0 or 1");
        out.push_back(Value::byteVal(v.asBool() ? 1 : 0));
        return Value::listVal(out);
    }

    // byte -> 1 byte
    if (vt == ValueType::Byte) {
        if (width != 0 && width != 1)
            throw std::runtime_error("to_bytes(byte): width must be 0 or 1");
        out.push_back(Value::byteVal(v.asByte()));
        return Value::listVal(out);
    }

    // int -> width bytes (default 8). Accepts boxed wide ints.
    if (vt == ValueType::Int) {
        if (width == 0) width = 8;
        if (width != 1 && width != 2 && width != 4 && width != 8)
            throw std::runtime_error("to_bytes(int): width must be 1, 2, 4, or 8");
        int64_t iv = v.asInt();
        // Range check for narrow widths (signed)
        if (width < 8) {
            int64_t maxV = (int64_t{1} << (8*width - 1)) - 1;
            int64_t minV = -(int64_t{1} << (8*width - 1));
            // also accept the equivalent unsigned range (e.g. 255 for width=1)
            int64_t maxU = (int64_t{1} << (8*width)) - 1;
            if (iv < minV || iv > maxU)
                throw std::runtime_error("to_bytes(int): value " + std::to_string(iv)
                    + " does not fit in " + std::to_string(width) + " byte(s)");
            (void)maxV;
        }
        uint64_t bits = static_cast<uint64_t>(iv);
        out.reserve(width);
        packBytes(out, bits, static_cast<int>(width), bigEndian);
        return Value::listVal(out);
    }

    // real -> 4 (float32) or 8 (double) bytes
    if (vt == ValueType::Real) {
        if (width == 0) width = 8;
        if (width != 4 && width != 8)
            throw std::runtime_error("to_bytes(real): width must be 4 (float32) or 8 (double)");
        double d = v.asReal();
        uint64_t bits = 0;
        if (width == 8) {
            std::memcpy(&bits, &d, 8);
        } else {
            float f = static_cast<float>(d);
            uint32_t b32;
            std::memcpy(&b32, &f, 4);
            bits = b32;
        }
        out.reserve(width);
        packBytes(out, bits, static_cast<int>(width), bigEndian);
        return Value::listVal(out);
    }

    // string -> UTF-8 bytes
    if (vt == ValueType::String) {
        if (width != 0)
            throw std::runtime_error("to_bytes(string): width must be omitted");
        std::string utf8 = toUTF8StdString(asStringObj(v)->s);
        out.reserve(utf8.size());
        for (unsigned char c : utf8)
            out.push_back(Value::byteVal(c));
        return Value::listVal(out);
    }

    throw std::invalid_argument("to_bytes: unsupported source type (expected bool, byte, int, real, or string)");
}

Value ModuleSys::from_bytes_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() < 1 || !isList(args[0]))
        throw std::invalid_argument("from_bytes expects a list of bytes as first argument");
    std::string typeStr = optDtypeArg(args, 1, "real");
    bool bigEndian = parseEndian(optStringArg(args, 2, "endian", "little"));
    bool isSigned = optBoolArg(args, 3, "signed", true);

    ObjList* lst = asList(args[0]);
    int len = lst->length();

    if (typeStr == "int") {
        if (len != 1 && len != 2 && len != 4 && len != 8)
            throw std::runtime_error("from_bytes(int): bytes length must be 1, 2, 4, or 8 (got " + std::to_string(len) + ")");
        uint64_t bits = unpackBytes(lst, 0, len, bigEndian, "from_bytes(int)");
        // Roxal int is signed int64. With signed=true (default), sign-extend
        // from len*8 bits. With signed=false, zero-extend (the raw bit pattern
        // is returned; for len=8 the top bit may still produce a negative
        // Roxal int because there is no unsigned int64 representation).
        int64_t resultVal;
        if (len == 8 || !isSigned) {
            // No sign-extension needed: either already 64-bit or unsigned.
            resultVal = static_cast<int64_t>(bits);
        } else {
            uint64_t signMask = uint64_t{1} << (8*len - 1);
            if (bits & signMask) {
                uint64_t extension = ~uint64_t{0} << (8*len);
                resultVal = static_cast<int64_t>(bits | extension);
            } else {
                resultVal = static_cast<int64_t>(bits);
            }
        }
        return Value::intVal(resultVal);
    }

    if (typeStr == "real") {
        if (len != 4 && len != 8)
            throw std::runtime_error("from_bytes(real): bytes length must be 4 (float32) or 8 (double), got " + std::to_string(len));
        uint64_t bits = unpackBytes(lst, 0, len, bigEndian, "from_bytes(real)");
        double d;
        if (len == 8) {
            std::memcpy(&d, &bits, 8);
        } else {
            uint32_t b32 = static_cast<uint32_t>(bits);
            float f;
            std::memcpy(&f, &b32, 4);
            d = static_cast<double>(f);
        }
        return Value::realVal(d);
    }

    if (typeStr == "bool") {
        if (len != 1)
            throw std::runtime_error("from_bytes(bool): expects exactly 1 byte, got " + std::to_string(len));
        uint8_t b = valueAsByte(lst->getElement(0), "from_bytes(bool)");
        return Value::boolVal(b != 0);
    }

    if (typeStr == "string") {
        std::string s;
        s.reserve(len);
        for (int i = 0; i < len; ++i)
            s.push_back(static_cast<char>(valueAsByte(lst->getElement(i), "from_bytes(string)")));
        return Value::stringVal(toUnicodeString(s));
    }

    throw std::invalid_argument("from_bytes: unknown dtype '" + typeStr + "' (expected 'int', 'real', 'bool', or 'string')");
}

Value ModuleSys::bits_to_bytes_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() < 1 || !isList(args[0]))
        throw std::invalid_argument("bits_to_bytes expects a list of bool/0/1 as first argument");
    bool msbFirst = optBoolArg(args, 1, "msb_first", true);

    ObjList* lst = asList(args[0]);
    int n = lst->length();
    int byteCount = (n + 7) / 8;
    std::vector<Value> out;
    out.reserve(byteCount);

    for (int byteIdx = 0; byteIdx < byteCount; ++byteIdx) {
        uint8_t b = 0;
        for (int j = 0; j < 8; ++j) {
            int bitIdx = byteIdx * 8 + j;
            if (bitIdx >= n) break;
            bool bit = valueAsBit(lst->getElement(bitIdx), "bits_to_bytes");
            if (bit) {
                int shift = msbFirst ? (7 - j) : j;
                b |= static_cast<uint8_t>(1u << shift);
            }
        }
        out.push_back(Value::byteVal(b));
    }
    return Value::listVal(out);
}

Value ModuleSys::bytes_to_bits_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() < 1 || !isList(args[0]))
        throw std::invalid_argument("bytes_to_bits expects a list of bytes as first argument");
    bool msbFirst = optBoolArg(args, 1, "msb_first", true);

    ObjList* lst = asList(args[0]);
    int n = lst->length();
    std::vector<Value> out;
    out.reserve(n * 8);

    for (int i = 0; i < n; ++i) {
        uint8_t b = valueAsByte(lst->getElement(i), "bytes_to_bits");
        for (int j = 0; j < 8; ++j) {
            int shift = msbFirst ? (7 - j) : j;
            bool bit = ((b >> shift) & 1u) != 0;
            out.push_back(Value::boolVal(bit));
        }
    }
    return Value::listVal(out);
}

Value ModuleSys::lshift_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() != 2)
        throw std::invalid_argument("lshift expects exactly 2 arguments (v, n)");
    // Use type() (not isInt() / isByte()) so heap-boxed wide ints are accepted.
    ValueType vt = args[0].type();
    ValueType nt = args[1].type();
    if (vt != ValueType::Int && vt != ValueType::Byte)
        throw std::invalid_argument("lshift: v must be int or byte");
    if (nt != ValueType::Int && nt != ValueType::Byte)
        throw std::invalid_argument("lshift: n must be int");
    int64_t v = args[0].asInt();
    int64_t n = args[1].asInt();
    if (n < 0 || n >= 64)
        throw std::runtime_error("lshift: shift amount must be in 0..63, got " + std::to_string(n));
    // Shift via unsigned to avoid UB on signed left-shift overflow
    uint64_t r = static_cast<uint64_t>(v) << n;
    // Value::intVal() auto-boxes to a heap int when the result doesn't fit in int32.
    return Value::intVal(static_cast<int64_t>(r));
}

Value ModuleSys::rshift_builtin(VM& vm, ArgsView args)
{
    (void)vm;
    if (args.size() != 2)
        throw std::invalid_argument("rshift expects exactly 2 arguments (v, n)");
    ValueType vt = args[0].type();
    ValueType nt = args[1].type();
    if (vt != ValueType::Int && vt != ValueType::Byte)
        throw std::invalid_argument("rshift: v must be int or byte");
    if (nt != ValueType::Int && nt != ValueType::Byte)
        throw std::invalid_argument("rshift: n must be int");
    int64_t v = args[0].asInt();
    int64_t n = args[1].asInt();
    if (n < 0 || n >= 64)
        throw std::runtime_error("rshift: shift amount must be in 0..63, got " + std::to_string(n));
    // Arithmetic right shift on signed: implementation-defined in C++ but
    // universally arithmetic on modern compilers. Use it directly.
    return Value::intVal(v >> n);
}

// Direct writer that walks Roxal Value without going through json11::Json,
// so dict insertion order (preserved by ObjDict::items()) survives to the
// emitted text. Supports JSON (default) and JSON5 (unquoted identifier keys,
// NaN/Infinity literals) output.
struct JsonWriter {
    std::string& out;
    bool indent;   // pretty-print with newlines + 2-space indent
    bool json5;    // emit unquoted identifier keys; emit NaN/Infinity literals

    static bool isIdentifier(const std::string& s) {
        if(s.empty()) return false;
        auto isStart = [](unsigned char c){
            return (c>='A' && c<='Z') || (c>='a' && c<='z') || c=='_' || c=='$';
        };
        auto isCont = [&](unsigned char c){
            return isStart(c) || (c>='0' && c<='9');
        };
        if(!isStart(static_cast<unsigned char>(s[0]))) return false;
        for(size_t i=1;i<s.size();++i)
            if(!isCont(static_cast<unsigned char>(s[i]))) return false;
        return true;
    }

    void writeIndent(int depth) {
        if(indent) out.append(size_t(depth*2), ' ');
    }
    void writeNewline() { if(indent) out += '\n'; }

    // Escape table mirrors core/json5.cpp dump(string&) — handles \b\f\n\r\t,
    // \uXXXX for <0x20, and U+2028/U+2029.
    void writeStringRaw(const std::string& s) {
        out += '"';
        for(size_t i=0;i<s.length();++i) {
            const char ch = s[i];
            if(ch == '\\') out += "\\\\";
            else if(ch == '"') out += "\\\"";
            else if(ch == '\b') out += "\\b";
            else if(ch == '\f') out += "\\f";
            else if(ch == '\n') out += "\\n";
            else if(ch == '\r') out += "\\r";
            else if(ch == '\t') out += "\\t";
            else if(static_cast<uint8_t>(ch) <= 0x1f) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", ch);
                out += buf;
            } else if(static_cast<uint8_t>(ch) == 0xe2
                      && i+2 < s.length()
                      && static_cast<uint8_t>(s[i+1]) == 0x80
                      && static_cast<uint8_t>(s[i+2]) == 0xa8) {
                out += "\\u2028";
                i += 2;
            } else if(static_cast<uint8_t>(ch) == 0xe2
                      && i+2 < s.length()
                      && static_cast<uint8_t>(s[i+1]) == 0x80
                      && static_cast<uint8_t>(s[i+2]) == 0xa9) {
                out += "\\u2029";
                i += 2;
            } else {
                out += ch;
            }
        }
        out += '"';
    }

    void writeKey(const std::string& s) {
        if(json5 && isIdentifier(s))
            out += s;
        else
            writeStringRaw(s);
    }

    void writeNumber(double d) {
        if(std::isfinite(d)) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
        } else if(json5) {
            if(std::isnan(d)) out += "NaN";
            else if(d > 0)    out += "Infinity";
            else              out += "-Infinity";
        } else {
            throw std::invalid_argument("to_json: non-finite number requires json5=true");
        }
    }

    void writeValue(const Value& v, int depth) {
        switch(v.type()) {
            case ValueType::Nil:  out += "null"; return;
            case ValueType::Bool: out += v.asBool() ? "true" : "false"; return;
            case ValueType::Byte: {
                char buf[8]; snprintf(buf, sizeof buf, "%d", int(v.asByte()));
                out += buf; return;
            }
            case ValueType::Int: {
                char buf[16]; snprintf(buf, sizeof buf, "%d", int(v.asInt()));
                out += buf; return;
            }
            case ValueType::Real: writeNumber(v.asReal()); return;
            case ValueType::String: writeStringRaw(toUTF8StdString(asStringObj(v)->s)); return;
            case ValueType::List: {
                ObjList* lst = asList(v);
                int n = lst->length();
                if(n == 0) { out += "[]"; return; }
                out += '[';
                writeNewline();
                int nDepth = depth + 1;
                for(int i=0;i<n;++i) {
                    writeIndent(nDepth);
                    writeValue(lst->getElement(i), nDepth);
                    if(i+1 < n) {
                        out += ',';
                        if(indent) out += '\n';
                        else       out += ' ';
                    }
                }
                writeNewline();
                writeIndent(depth);
                out += ']';
                return;
            }
            case ValueType::Dict: {
                const auto items = asDict(v)->items();
                if(items.empty()) { out += "{}"; return; }
                out += '{';
                writeNewline();
                int nDepth = depth + 1;
                for(size_t i=0;i<items.size();++i) {
                    const auto& kv = items[i];
                    if(!isString(kv.first))
                        throw std::runtime_error("dict key not string");
                    writeIndent(nDepth);
                    writeKey(toUTF8StdString(asStringObj(kv.first)->s));
                    out += ": ";
                    writeValue(kv.second, nDepth);
                    if(i+1 < items.size()) {
                        out += ',';
                        if(indent) out += '\n';
                        else       out += ' ';
                    }
                }
                writeNewline();
                writeIndent(depth);
                out += '}';
                return;
            }
            default:
                if(isObjectInstance(v) || isActorInstance(v)) {
                    writeValue(toType(ValueType::Dict, v, false), depth);
                    return;
                }
                throw std::runtime_error("unsupported type for to_json");
        }
    }
};

Value ModuleSys::to_json_builtin(VM& vm, ArgsView args)
{
    if(args.size() < 1 || args.size() > 3)
        throw std::invalid_argument("to_json expects value and optional indent bool and json5 bool");

    bool indent = true;
    if(args.size() >= 2)
        indent = toType(ValueType::Bool, args[1], false).asBool();

    bool json5 = false;
    if(args.size() >= 3)
        json5 = toType(ValueType::Bool, args[2], false).asBool();

    std::string out;
    JsonWriter w{out, indent, json5};
    w.writeValue(args[0], 0);
    return Value::stringVal(toUnicodeString(out));
}

static Value jsonToValue(const json11::Json& j) {
    using json11::Json;
    switch(j.type()) {
        case Json::NUL: return Value::nilVal();
        case Json::BOOL: return Value::boolVal(j.bool_value());
        case Json::NUMBER: {
            double n = j.number_value();
            if(std::floor(n) == n && n >= std::numeric_limits<int32_t>::min() && n <= std::numeric_limits<int32_t>::max())
                return Value::intVal(static_cast<int32_t>(n));
            return Value::realVal(n);
        }
        case Json::STRING: return Value::stringVal(toUnicodeString(j.string_value()));
        case Json::ARRAY: {
            std::vector<Value> elts; elts.reserve(j.array_items().size());
            for(const auto& it : j.array_items()) elts.push_back(jsonToValue(it));
            return Value::listVal(elts);
        }
        case Json::OBJECT: {
            Value d { Value::dictVal() };
            const auto& items = j.object_items();
            const auto& order = j.keys_in_order();
            if(!order.empty()) {
                // Preserve original parse-order for JSON5 round-trips.
                for(const auto& k : order) {
                    auto it = items.find(k);
                    if(it != items.end())
                        asDict(d)->store(Value::stringVal(toUnicodeString(k)), jsonToValue(it->second));
                }
            } else {
                for(const auto& kv : items) {
                    asDict(d)->store(Value::stringVal(toUnicodeString(kv.first)), jsonToValue(kv.second));
                }
            }
            return d;
        }
    }
    return Value::nilVal();
}

Value ModuleSys::from_json_builtin(VM& vm, ArgsView args)
{
    if(args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("from_json expects json string");

    std::string s = toUTF8StdString(asStringObj(args[0])->s);
    std::string err;
    json11::Json j = json11::Json::parse(s, err, json11::JsonParse::JSON5);
    if(!err.empty())
        throw std::invalid_argument(std::string("invalid json: ")+err);
    return jsonToValue(j);
}

Value ModuleSys::from_xml_builtin(VM& vm, ArgsView args)
{
    (void)vm;
#ifndef ROXAL_ENABLE_XML
    (void)args;
    throw std::runtime_error("XML support not enabled in this build");
#else
    if (args.size() < 1 || args.size() > 3 || !isString(args[0]))
        throw std::invalid_argument(
            "from_xml expects xml string and optional mode string and preserve_whitespace bool");

    std::string mode = XML_MODE_COMPACT;
    if (args.size() >= 2)
        mode = parseXmlReadMode(args[1]);

    bool preserveWhitespace = false;
    if (args.size() >= 3)
        preserveWhitespace = toType(ValueType::Bool, args[2], false).asBool();

    const std::string xml = toUTF8StdString(asStringObj(args[0])->s);
    pugi::xml_document doc;
    const unsigned int parseFlags = preserveWhitespace
        ? static_cast<unsigned int>(pugi::parse_default | pugi::parse_ws_pcdata)
        : static_cast<unsigned int>(pugi::parse_default);
    pugi::xml_parse_result result = doc.load_string(xml.c_str(), parseFlags);
    if (!result) {
        auto [line, col] = xmlLineColFromOffset(xml, result.offset);
        throw std::invalid_argument("invalid xml: line " + std::to_string(line) +
                                    ", col " + std::to_string(col) + ": " +
                                    std::string(result.description()));
    }

    pugi::xml_node root = doc.document_element();
    if (!root)
        throw std::invalid_argument("invalid xml: document has no root element");

    if (mode == XML_MODE_RAW)
        return elementToRawValue(root, preserveWhitespace);
    return elementToCompactValue(root, preserveWhitespace);
#endif
}

Value ModuleSys::to_xml_builtin(VM& vm, ArgsView args)
{
    (void)vm;
#ifndef ROXAL_ENABLE_XML
    (void)args;
    throw std::runtime_error("XML support not enabled in this build");
#else
    if (args.size() < 1 || args.size() > 3)
        throw std::invalid_argument("to_xml expects value and optional indent bool and mode string");

    bool indent = true;
    if (args.size() >= 2)
        indent = toType(ValueType::Bool, args[1], false).asBool();

    std::string mode = XML_MODE_AUTO;
    if (args.size() >= 3)
        mode = parseXmlWriteMode(args[2]);

    if (!isDict(args[0]))
        throw std::invalid_argument("to_xml expects XML-shaped dict value");

    if (mode == XML_MODE_AUTO)
        mode = isExactRawNodeShape(args[0]) ? XML_MODE_RAW : XML_MODE_COMPACT;

    XmlNode root = (mode == XML_MODE_RAW)
        ? rawValueToXmlNode(args[0], "raw XML root")
        : compactValueToXmlNode(args[0], std::nullopt, "compact XML root");

    pugi::xml_document doc;
    appendXmlNode(doc, root);

    std::ostringstream out;
    const unsigned int flags = indent
        ? static_cast<unsigned int>(pugi::format_default | pugi::format_no_declaration)
        : static_cast<unsigned int>(pugi::format_raw | pugi::format_no_declaration);
    doc.save(out, indent ? "  " : "", flags, pugi::encoding_utf8);
    std::string xml = out.str();
    if (!xml.empty() && xml.back() == '\n')
        xml.pop_back();
    return Value::stringVal(toUnicodeString(xml));
 #endif
}

Value ModuleSys::time_init_native(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 3)
        throw std::invalid_argument("Time.init expects optional kind and tz");

    ObjectInstance* inst = requireInstance(args[0], timeTypeObj, "Time.init", "Time");

    std::string kind = "wall";
    if (args.size() >= 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("Time.init kind must be string");
        kind = toString(args[1]);
    }

    std::string tz = "local";
    if (args.size() >= 3) {
        if (!isString(args[2]))
            throw std::invalid_argument("Time.init tz must be string");
        tz = toString(args[2]);
    }

    TimeKind tk = parseKind(kind);
    if (tk == TimeKind::Steady) {
        auto now = std::chrono::steady_clock::now();
        int64_t total = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        assignTime(inst, total, true);
    } else {
        (void)parseZone(tz);
        auto now = std::chrono::system_clock::now();
        int64_t total = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        assignTime(inst, total, false);
    }

    return Value::nilVal();
}

Value ModuleSys::time_kind_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("Time.kind expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeTypeObj, "Time.kind", "Time");
    return Value::stringVal(toUnicodeString(timeIsSteady(inst) ? "steady" : "wall"));
}

Value ModuleSys::time_is_steady_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("Time.is_steady expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeTypeObj, "Time.is_steady", "Time");
    return timeIsSteady(inst) ? Value::trueVal() : Value::falseVal();
}

Value ModuleSys::time_seconds_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("Time.seconds expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeTypeObj, "Time.seconds", "Time");
    return Value::intVal(readIntProperty(inst, "_seconds"));
}

Value ModuleSys::time_micros_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("Time.microseconds expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeTypeObj, "Time.microseconds", "Time");
    return Value::intVal(readIntProperty(inst, "_micros"));
}

Value ModuleSys::time_diff_native(VM& vm, ArgsView args)
{
    if (args.size() != 2)
        throw std::invalid_argument("Time.diff expects one argument");

    ObjectInstance* self = requireInstance(args[0], timeTypeObj, "Time.diff", "Time");
    ObjectInstance* other = requireInstance(args[1], timeTypeObj, "Time.diff", "Time");
    bool steadySelf = timeIsSteady(self);
    bool steadyOther = timeIsSteady(other);
    if (steadySelf != steadyOther)
        throw std::invalid_argument("Time.diff requires both times from the same clock");

    int64_t total = timeTotalMicros(self) - timeTotalMicros(other);
    return newSpanInstance(timeSpanTypeValue, total);
}

Value ModuleSys::time_since_native(VM& vm, ArgsView args)
{
    return time_diff_native(vm, args);
}

Value ModuleSys::time_until_native(VM& vm, ArgsView args)
{
    if (args.size() != 2)
        throw std::invalid_argument("Time.until expects one argument");

    ObjectInstance* self = requireInstance(args[0], timeTypeObj, "Time.until", "Time");
    ObjectInstance* other = requireInstance(args[1], timeTypeObj, "Time.until", "Time");
    bool steadySelf = timeIsSteady(self);
    bool steadyOther = timeIsSteady(other);
    if (steadySelf != steadyOther)
        throw std::invalid_argument("Time.until requires both times from the same clock");

    int64_t total = timeTotalMicros(other) - timeTotalMicros(self);
    return newSpanInstance(timeSpanTypeValue, total);
}

Value ModuleSys::time_format_native(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 3)
        throw std::invalid_argument("Time.format expects optional format and tz");

    ObjectInstance* inst = requireInstance(args[0], timeTypeObj, "Time.format", "Time");
    if (timeIsSteady(inst))
        throw std::invalid_argument("Time.format is only valid for wall-clock times");

    std::string format = "%Y-%m-%d %H:%M:%S";
    if (args.size() >= 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("Time.format format must be string");
        format = toString(args[1]);
    }

    std::string tz = "local";
    if (args.size() >= 3) {
        if (!isString(args[2]))
            throw std::invalid_argument("Time.format tz must be string");
        tz = toString(args[2]);
    }

    ClockZone zone = parseZone(tz);
    NormalizedParts parts = normalizeMicros(timeTotalMicros(inst));
    std::tm tm {};
    if (!toCalendar(parts.seconds, zone, tm))
        throw std::runtime_error("time value out of range");

    std::string out = formatWithMicros(tm, parts.micros, format);
    return Value::stringVal(toUnicodeString(out));
}

Value ModuleSys::time_components_native(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2)
        throw std::invalid_argument("Time.components expects optional tz");

    ObjectInstance* inst = requireInstance(args[0], timeTypeObj, "Time.components", "Time");
    if (timeIsSteady(inst))
        throw std::invalid_argument("Time.components is only valid for wall-clock times");

    std::string tz = "local";
    if (args.size() == 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("Time.components tz must be string");
        tz = toString(args[1]);
    }

    ClockZone zone = parseZone(tz);
    NormalizedParts parts = normalizeMicros(timeTotalMicros(inst));
    std::tm tm {};
    if (!toCalendar(parts.seconds, zone, tm))
        throw std::runtime_error("time value out of range");

    Value dict { Value::dictVal() };
    auto* d = asDict(dict);
    d->store(Value::stringVal(toUnicodeString("year")), Value::intVal(tm.tm_year + 1900));
    d->store(Value::stringVal(toUnicodeString("month")), Value::intVal(tm.tm_mon + 1));
    d->store(Value::stringVal(toUnicodeString("day")), Value::intVal(tm.tm_mday));
    d->store(Value::stringVal(toUnicodeString("hour")), Value::intVal(tm.tm_hour));
    d->store(Value::stringVal(toUnicodeString("minute")), Value::intVal(tm.tm_min));
    d->store(Value::stringVal(toUnicodeString("second")), Value::intVal(tm.tm_sec));
    d->store(Value::stringVal(toUnicodeString("microsecond")), Value::intVal(parts.micros));
    d->store(Value::stringVal(toUnicodeString("weekday")), Value::intVal(tm.tm_wday));
    d->store(Value::stringVal(toUnicodeString("yearday")), Value::intVal(tm.tm_yday + 1));

    return dict;
}

Value ModuleSys::timespan_init_native(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 7)
        throw std::invalid_argument("TimeSpan.init expects up to six numeric arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.init", "TimeSpan");

    auto readArg = [&](size_t index, const char* name) -> int64_t {
        if (index >= args.size())
            return 0;
        if (!args[index].isNumber())
            throw std::invalid_argument(std::string("TimeSpan.init ") + name + " must be int");
        return toType(ValueType::Int, args[index], false).asInt();
    };

    int64_t days = readArg(1, "days");
    int64_t hours = readArg(2, "hours");
    int64_t minutes = readArg(3, "minutes");
    int64_t seconds = readArg(4, "seconds");
    int64_t millis = readArg(5, "millis");
    int64_t micros = readArg(6, "micros");

    int64_t total = durationFromFields(days, hours, minutes, seconds, millis, micros);
    assignSpan(inst, total);
    return Value::nilVal();
}

Value ModuleSys::timespan_seconds_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.seconds expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.seconds", "TimeSpan");
    return Value::intVal(readIntProperty(inst, "_seconds"));
}

Value ModuleSys::timespan_micros_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.microseconds expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.microseconds", "TimeSpan");
    return Value::intVal(readIntProperty(inst, "_micros"));
}

Value ModuleSys::timespan_split_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.split expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.split", "TimeSpan");
    int64_t total = spanTotalMicros(inst);
    bool negative = total < 0;
    int64_t remaining = negative ? -total : total;

    int32_t days = static_cast<int32_t>(remaining / MICROS_PER_DAY);
    remaining %= MICROS_PER_DAY;
    int32_t hours = static_cast<int32_t>(remaining / MICROS_PER_HOUR);
    remaining %= MICROS_PER_HOUR;
    int32_t minutes = static_cast<int32_t>(remaining / MICROS_PER_MINUTE);
    remaining %= MICROS_PER_MINUTE;
    int32_t seconds = static_cast<int32_t>(remaining / MICROS_PER_SECOND);
    remaining %= MICROS_PER_SECOND;
    int32_t millis = static_cast<int32_t>(remaining / MICROS_PER_MILLISECOND);
    int32_t micros = static_cast<int32_t>(remaining % MICROS_PER_MILLISECOND);

    Value dict { Value::dictVal() };
    auto* d = asDict(dict);
    d->store(Value::stringVal(toUnicodeString("days")), Value::intVal(days));
    d->store(Value::stringVal(toUnicodeString("hours")), Value::intVal(hours));
    d->store(Value::stringVal(toUnicodeString("minutes")), Value::intVal(minutes));
    d->store(Value::stringVal(toUnicodeString("seconds")), Value::intVal(seconds));
    d->store(Value::stringVal(toUnicodeString("millis")), Value::intVal(millis));
    d->store(Value::stringVal(toUnicodeString("micros")), Value::intVal(micros));
    d->store(Value::stringVal(toUnicodeString("negative")), Value::boolVal(negative));

    return dict;
}

Value ModuleSys::timespan_total_days_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.total_days expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.total_days", "TimeSpan");
    return Value::realVal(static_cast<double>(spanTotalMicros(inst)) / static_cast<double>(MICROS_PER_DAY));
}

Value ModuleSys::timespan_total_hours_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.total_hours expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.total_hours", "TimeSpan");
    return Value::realVal(static_cast<double>(spanTotalMicros(inst)) / static_cast<double>(MICROS_PER_HOUR));
}

Value ModuleSys::timespan_total_minutes_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.total_minutes expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.total_minutes", "TimeSpan");
    return Value::realVal(static_cast<double>(spanTotalMicros(inst)) / static_cast<double>(MICROS_PER_MINUTE));
}

Value ModuleSys::timespan_total_seconds_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.total_seconds expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.total_seconds", "TimeSpan");
    return Value::realVal(static_cast<double>(spanTotalMicros(inst)) / static_cast<double>(MICROS_PER_SECOND));
}

Value ModuleSys::timespan_total_millis_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.total_millis expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.total_millis", "TimeSpan");
    return Value::realVal(static_cast<double>(spanTotalMicros(inst)) / static_cast<double>(MICROS_PER_MILLISECOND));
}

Value ModuleSys::timespan_total_micros_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.total_micros expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.total_micros", "TimeSpan");
    return Value::realVal(static_cast<double>(spanTotalMicros(inst)));
}

Value ModuleSys::timespan_human_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("TimeSpan.human expects no arguments");

    ObjectInstance* inst = requireInstance(args[0], timeSpanTypeObj, "TimeSpan.human", "TimeSpan");
    std::string out = humanDurationString(spanTotalMicros(inst));
    return Value::stringVal(toUnicodeString(out));
}

Value ModuleSys::quantity_set_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2)
        throw std::invalid_argument("quantity.set expects (s :string)");

    ObjectInstance* inst = requireInstance(args[0], quantityTypeObj, "quantity.set", "quantity");

    if (!isString(args[1]))
        throw std::invalid_argument("quantity.set expects a string argument");

    icu::UnicodeString raw = asStringObj(args[1])->s;

    // Trim leading/trailing ASCII whitespace.
    int32_t lo = 0;
    int32_t hi = raw.length();
    while (lo < hi) {
        UChar32 c = raw.char32At(lo);
        if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r') ++lo;
        else break;
    }
    while (hi > lo) {
        UChar32 c = raw.char32At(hi - 1);
        if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r') --hi;
        else break;
    }
    if (lo >= hi)
        throw std::invalid_argument("quantity.set: empty string");

    icu::UnicodeString trimmed = raw.tempSubString(lo, hi - lo);

    // Parse the numeric prefix via strtod on a UTF-8 copy.
    std::string utf8;
    trimmed.toUTF8String(utf8);
    const char* cstr = utf8.c_str();
    char* endp = nullptr;
    double number = std::strtod(cstr, &endp);
    if (endp == cstr)
        throw std::invalid_argument("quantity.set: missing numeric prefix in '" + utf8 + "'");

    // Skip an optional single space between number and unit.
    while (*endp == ' ' || *endp == '\t')
        ++endp;

    std::string suffixUtf8(endp);
    icu::UnicodeString suffix = toUnicodeString(suffixUtf8);

    double scale = 1.0;
    std::array<int32_t, 4> dims = {0, 0, 0, 0};

    if (suffix.isEmpty()) {
        // Dimensionless quantity.
    } else {
        // Tier 1: exact match against the table of named units (covers all
        // single-token literal forms — `m`, `kg`, `deg`, `s`, `N` etc. — and
        // compound forms `m/s`, `deg/s`, `Nm`, `N·m`, `m/s^2`, `m/s²`).
        const auto& broad = qtyparse::broadTable();
        bool matched = false;
        // Exact match search: any entry whose text equals the entire suffix.
        // (Used as a fast path; the general parser below would also succeed.)
        for (const auto& u : broad) {
            if (u.text == suffix) {
                scale = u.scale;
                dims = u.dims;
                matched = true;
                break;
            }
        }
        // Common compound forms not in broadTable (which only holds singletons).
        if (!matched) {
            struct Pair { const char* text; double scale; std::array<int32_t,4> dims; };
            static const Pair kCompound[] = {
                {"mm/s", 1e-3,         {1,-1,0,0}},
                {"cm/s", 1e-2,         {1,-1,0,0}},
                {"m/s",  1.0,          {1,-1,0,0}},
                {"m/s^2", 1.0,         {1,-2,0,0}},
                {"m/s²",  1.0,         {1,-2,0,0}},
                // Jerk (linear)
                {"m/s^3",  1.0,        {1,-3,0,0}},
                {"m/s³",   1.0,        {1,-3,0,0}},
                {"mm/s^3", 1e-3,       {1,-3,0,0}},
                {"mm/s³",  1e-3,       {1,-3,0,0}},
                {"Nm",   1.0,          {2,-2,1,0}},
                {"N·m",  1.0,          {2,-2,1,0}},
                {"rad/s", 1.0,         {0,-1,0,1}},
                {"deg/s", qtyparse::kPi/180.0, {0,-1,0,1}},
                {"°/s",  qtyparse::kPi/180.0, {0,-1,0,1}},
                // Angular acceleration
                {"rad/s^2", 1.0,                 {0,-2,0,1}},
                {"rad/s²",  1.0,                 {0,-2,0,1}},
                {"deg/s^2", qtyparse::kPi/180.0, {0,-2,0,1}},
                {"deg/s²",  qtyparse::kPi/180.0, {0,-2,0,1}},
                // Angular jerk
                {"rad/s^3", 1.0,                 {0,-3,0,1}},
                {"rad/s³",  1.0,                 {0,-3,0,1}},
                {"deg/s^3", qtyparse::kPi/180.0, {0,-3,0,1}},
                {"deg/s³",  qtyparse::kPi/180.0, {0,-3,0,1}},
            };
            for (const auto& p : kCompound) {
                if (suffix == toUnicodeString(p.text)) {
                    scale = p.scale;
                    dims = p.dims;
                    matched = true;
                    break;
                }
            }
        }
        // Tier 2: general parser.
        if (!matched) {
            const auto& table = qtyparse::containsSuperscriptOrMinus(suffix)
                ? qtyparse::siTable()
                : qtyparse::broadTable();
            try {
                if (!qtyparse::parseUnitExpression(suffix, 0, table, scale, dims))
                    throw std::invalid_argument(
                        "quantity.set: unknown unit suffix '" + suffixUtf8 + "'");
            } catch (const std::exception& ex) {
                throw std::invalid_argument(
                    std::string("quantity.set: failed to parse '") + utf8 + "': " + ex.what());
            }
        }
    }

    double si = number * scale;
    inst->setProperty("_v", Value::realVal(si));

    Value dimsListVal = Value::listVal();
    ObjList* dimsList = asList(dimsListVal);
    for (int i = 0; i < 4; ++i)
        dimsList->append(Value::intVal(static_cast<int64_t>(dims[i])));
    inst->setProperty("_d", dimsListVal);

    return Value::nilVal();
}

Value ModuleSys::time_type_wall_now(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2)
        throw std::invalid_argument("Time.wall_now expects optional tz");

    if (!isObjectType(args[0]) || asObjectType(args[0]) != timeTypeObj)
        throw std::invalid_argument("Time.wall_now must be called on sys.Time");

    std::string tz = "local";
    if (args.size() == 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("Time.wall_now tz must be string");
        tz = toString(args[1]);
    }

    (void)parseZone(tz);
    auto now = std::chrono::system_clock::now();
    int64_t total = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return newTimeInstance(timeTypeValue, total, false);
}

Value ModuleSys::time_type_steady_now(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("Time.steady_now expects no arguments");

    if (!isObjectType(args[0]) || asObjectType(args[0]) != timeTypeObj)
        throw std::invalid_argument("Time.steady_now must be called on sys.Time");

    auto now = std::chrono::steady_clock::now();
    int64_t total = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return newTimeInstance(timeTypeValue, total, true);
}

Value ModuleSys::time_type_parse(VM& vm, ArgsView args)
{
    if (args.size() < 2 || args.size() > 4)
        throw std::invalid_argument("Time.parse expects text, optional format and tz");

    if (!isObjectType(args[0]) || asObjectType(args[0]) != timeTypeObj)
        throw std::invalid_argument("Time.parse must be called on sys.Time");
    if (!isString(args[1]))
        throw std::invalid_argument("Time.parse text must be string");

    std::string text = toString(args[1]);
    std::string format = "%Y-%m-%d %H:%M:%S";
    if (args.size() >= 3) {
        if (!isString(args[2]))
            throw std::invalid_argument("Time.parse format must be string");
        format = toString(args[2]);
    }
    std::string tz = "local";
    if (args.size() == 4) {
        if (!isString(args[3]))
            throw std::invalid_argument("Time.parse tz must be string");
        tz = toString(args[3]);
    }

    ClockZone zone = parseZone(tz);
    int64_t total = parseWallTime(text, format, zone);
    return newTimeInstance(timeTypeValue, total, false);
}

Value ModuleSys::time_type_from_parts(VM& vm, ArgsView args)
{
    if (args.size() < 2 || args.size() > 4)
        throw std::invalid_argument("Time.from_parts expects seconds, optional micros and kind");

    if (!isObjectType(args[0]) || asObjectType(args[0]) != timeTypeObj)
        throw std::invalid_argument("Time.from_parts must be called on sys.Time");
    if (!args[1].isNumber())
        throw std::invalid_argument("Time.from_parts seconds must be int");

    int32_t seconds = toType(ValueType::Int, args[1], false).asInt();
    int32_t micros = 0;
    if (args.size() >= 3) {
        if (!args[2].isNumber())
            throw std::invalid_argument("Time.from_parts micros must be int");
        micros = toType(ValueType::Int, args[2], false).asInt();
    }

    std::string kind = "wall";
    if (args.size() == 4) {
        if (!isString(args[3]))
            throw std::invalid_argument("Time.from_parts kind must be string");
        kind = toString(args[3]);
    }

    TimeKind tk = parseKind(kind);
    int64_t total = static_cast<int64_t>(seconds) * MICROS_PER_SECOND + micros;
    return newTimeInstance(timeTypeValue, total, tk == TimeKind::Steady);
}

Value ModuleSys::timespan_type_from_fields(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 7)
        throw std::invalid_argument("TimeSpan.from_fields expects up to six numeric arguments");

    if (!isObjectType(args[0]) || asObjectType(args[0]) != timeSpanTypeObj)
        throw std::invalid_argument("TimeSpan.from_fields must be called on sys.TimeSpan");

    auto readArg = [&](size_t index, const char* name) -> int64_t {
        if (index >= args.size())
            return 0;
        if (!args[index].isNumber())
            throw std::invalid_argument(std::string("TimeSpan.from_fields ") + name + " must be int");
        return toType(ValueType::Int, args[index], false).asInt();
    };

    int64_t days = readArg(1, "days");
    int64_t hours = readArg(2, "hours");
    int64_t minutes = readArg(3, "minutes");
    int64_t seconds = readArg(4, "seconds");
    int64_t millis = readArg(5, "millis");
    int64_t micros = readArg(6, "micros");

    int64_t total = durationFromFields(days, hours, minutes, seconds, millis, micros);
    return newSpanInstance(timeSpanTypeValue, total);
}

Value ModuleSys::clock_native(VM& vm, ArgsView args)
{
    return Value::realVal(double(clock())/CLOCKS_PER_SEC);
}

Value ModuleSys::clock_signal_native(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !args[0].isNumber())
        throw std::invalid_argument("clock expects frequency and optional name");

    double freq = args[0].asReal();
    std::string nameStr;
    if (args.size() >= 2)
        nameStr = toString(args[1]);

    std::string autoName = df::DataflowEngine::uniqueFuncName("clock("+ std::to_string(int(freq)) + ")");
    std::string finalName = nameStr.empty() ? autoName : nameStr;

    auto sig = df::Signal::newClockSignal(freq, finalName);
    return Value::signalVal(sig);
}

Value ModuleSys::engine_stop_native(VM& vm, ArgsView args)
{
    if (auto engine = df::DataflowEngine::instance(false))
        engine->stop();
    return Value::nilVal();
}

Value ModuleSys::typeof_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("typeof expects single argument");

    Value val = args[0];
    bool isConst = val.isConst();
    ValueType valueType;

    // Determine the ValueType of the argument

    if (val.isNil()) {
        valueType = ValueType::Nil;
    } else if (val.isBool()) {
        valueType = ValueType::Bool;
    } else if (val.isByte()) {
        valueType = ValueType::Byte;
    } else if (val.isInt()) {
        valueType = ValueType::Int;
    } else if (val.isReal()) {
        valueType = ValueType::Real;
    } else if (val.isEnum()) {
        valueType = ValueType::Enum;
    } else if (val.isType()) {
        valueType = ValueType::Type;
    } else if (isSignal(val)) {
        valueType = ValueType::Signal;
    } else if (isEventType(val)) {
        valueType = ValueType::Event;
    } else if (val.isObj()) {
        Obj* obj = val.asObj();
        if (obj->type == ObjType::Instance) {
            Value result = asObjectInstance(val)->instanceType;
            return isConst ? result.constRef() : result;
        }
        if (obj->type == ObjType::Actor) {
            Value result = asActorInstance(val)->instanceType;
            return isConst ? result.constRef() : result;
        }
        if (obj->type == ObjType::Exception) {
            ObjException* ex = asException(val);
            if (!ex->exType.isNil())
                return ex->exType;
            // fall back to builtin 'exception' type if somehow missing
            auto maybe = vm.globals.load(toUnicodeString("exception"));
            if (maybe.has_value())
                return maybe.value();
            return Value::typeSpecVal(ValueType::Object);
        }

        // For primitive object wrappers like strings
        valueType = obj->valueType();
        auto typeObj = newTypeSpecObj(valueType);
        Value result = Value::objVal(std::move(typeObj));
        return isConst ? result.constRef() : result;
    } else {
        // Fallback
        valueType = ValueType::Nil;
    }

    return Value::typeSpecVal(valueType);
}

Value ModuleSys::df_graph_native(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_df_graph has no arguments");

    auto engine = df::DataflowEngine::instance();
    auto str = engine->graph();
    return Value::stringVal(toUnicodeString(str));
}

Value ModuleSys::df_tick_native(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_dataflow_tick has no arguments");

    // Single-step the dataflow engine: the tick executes on the ENGINE
    // thread (the sole periodic driver); this thread waits it out.
    df::DataflowEngine::instance()->requestTickAndWait();
    return Value::nilVal();
}

Value ModuleSys::df_islands_native(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_df_islands has no arguments");

    auto engine = df::DataflowEngine::instance();
    auto snapshot = engine->islandDebugSnapshot();

    Value islandsList = Value::listVal();
    ObjList* islandsObj = asList(islandsList);

    for (const auto& island : snapshot) {
        Value dictVal = Value::dictVal();
        ObjDict* dict = asDict(dictVal);

        Value signalsList = Value::listVal();
        ObjList* signalsObj = asList(signalsList);
        for (const auto& name : island.signals)
            signalsObj->append(Value::stringVal(toUnicodeString(name)));

        dict->store(Value::stringVal(toUnicodeString("signals")), signalsList);
        dict->store(Value::stringVal(toUnicodeString("tick_us")),
                    Value::intVal(static_cast<int32_t>(island.tickPeriod.microSecs())));
        dict->store(Value::stringVal(toUnicodeString("event_driven_only")),
                    Value::boolVal(island.eventDrivenOnly));

        islandsObj->append(dictVal);
    }

    return islandsList;
}

Value ModuleSys::df_graphdot_native(VM& vm, ArgsView args)
{
    std::string title;
    if (args.size() > 1)
        throw std::invalid_argument("_df_graphdot expects zero or one title :string argument");
    if (args.size() == 1) {
        if (!isString(args[0]))
            throw std::invalid_argument("_df_graphdot expects string argument");
        title = toUTF8StdString(asStringObj(args[0])->s);
    }

    auto engine = df::DataflowEngine::instance();
    auto dot = engine->graphDot(title, engine->signalValues());
    return Value::stringVal(toUnicodeString(dot));
}

Value ModuleSys::loadlib_native(VM& vm, ArgsView args)
{
    return roxal::loadlib_native(args);
}

Value ModuleSys::source_dir_native(VM& vm, ArgsView args)
{
    return roxal::source_dir_native(args);
}

Value ModuleSys::module_paths_native(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("module_paths expects no arguments");
    std::vector<Value> paths;
    for (const auto& p : vm.getModulePaths())
        paths.push_back(Value::stringVal(toUnicodeString(p)));
    return Value::listVal(paths);
}

Value ModuleSys::list_filter_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2 || !isList(args[0]))
        throw std::invalid_argument("list.filter expects single predicate argument");

    if (!isClosure(args[1]))
        throw std::invalid_argument("list.filter: argument must be a function");

    ObjList* inputList = asList(args[0]);
    ObjClosure* predicate = asClosure(args[1]);

    if (inputList->empty())
        return Value::listVal();

    int arity = asFunction(predicate->function)->arity;

    // Continuation state: {list, pred, result, index, arity}
    Value state = Value::dictVal();
    asDict(state)->store(Value::stringVal(toUnicodeString("list")), args[0]);
    asDict(state)->store(Value::stringVal(toUnicodeString("pred")), args[1]);
    asDict(state)->store(Value::stringVal(toUnicodeString("result")), Value::listVal());
    asDict(state)->store(Value::stringVal(toUnicodeString("index")), Value::intVal(0));
    asDict(state)->store(Value::stringVal(toUnicodeString("arity")), Value::intVal(arity));

    auto& cont = vm.thread->pushContinuation();
    cont.state = state;
    cont.resultSlotIndex = static_cast<ptrdiff_t>((vm.thread->stackTop - vm.thread->stack.begin()) - args.size());
    cont.stackBaseIndex = cont.resultSlotIndex + 1;
    cont.onComplete = [](VM& vm, Value callbackResult) -> bool {
        auto& st = vm.thread->currentContinuation().state;
        auto* d = asDict(st);
        ObjList* list = asList(d->at(Value::stringVal(toUnicodeString("list"))));
        ObjList* result = asList(d->at(Value::stringVal(toUnicodeString("result"))));
        int idx = d->at(Value::stringVal(toUnicodeString("index"))).asInt();
        int arity = d->at(Value::stringVal(toUnicodeString("arity"))).asInt();

        auto elts = list->getElements();
        if (isTruthy(callbackResult))
            result->append(elts[idx]);

        idx++;
        d->store(Value::stringVal(toUnicodeString("index")), Value::intVal(idx));

        if (static_cast<size_t>(idx) < elts.size()) {
            ObjClosure* pred = asClosure(d->at(Value::stringVal(toUnicodeString("pred"))));
            std::vector<Value> callArgs;
            callArgs.push_back(elts[idx]);
            if (arity >= 2)
                callArgs.push_back(Value::intVal(idx));
            return vm.pushContinuationCall(pred, callArgs);
        }

        vm.push(d->at(Value::stringVal(toUnicodeString("result"))));
        return true;
    };

    auto elts = inputList->getElements();
    std::vector<Value> callArgs;
    callArgs.push_back(elts[0]);
    if (arity >= 2)
        callArgs.push_back(Value::intVal(0));

    if (!vm.pushContinuationCall(predicate, callArgs))
        throw std::runtime_error("list.filter: failed to invoke predicate");

    return Value::nilVal();
}

Value ModuleSys::list_map_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2 || !isList(args[0]))
        throw std::invalid_argument("list.map expects single transform argument");

    if (!isClosure(args[1]))
        throw std::invalid_argument("list.map: argument must be a function");

    ObjList* inputList = asList(args[0]);
    ObjClosure* transform = asClosure(args[1]);

    if (inputList->empty())
        return Value::listVal();

    int arity = asFunction(transform->function)->arity;

    // Continuation state: {list, transform, result, index, arity}
    Value state = Value::dictVal();
    asDict(state)->store(Value::stringVal(toUnicodeString("list")), args[0]);
    asDict(state)->store(Value::stringVal(toUnicodeString("transform")), args[1]);
    asDict(state)->store(Value::stringVal(toUnicodeString("result")), Value::listVal());
    asDict(state)->store(Value::stringVal(toUnicodeString("index")), Value::intVal(0));
    asDict(state)->store(Value::stringVal(toUnicodeString("arity")), Value::intVal(arity));

    auto& cont = vm.thread->pushContinuation();
    cont.state = state;
    cont.resultSlotIndex = static_cast<ptrdiff_t>((vm.thread->stackTop - vm.thread->stack.begin()) - args.size());
    cont.stackBaseIndex = cont.resultSlotIndex + 1;
    cont.onComplete = [](VM& vm, Value callbackResult) -> bool {
        auto& st = vm.thread->currentContinuation().state;
        auto* d = asDict(st);
        ObjList* list = asList(d->at(Value::stringVal(toUnicodeString("list"))));
        ObjList* result = asList(d->at(Value::stringVal(toUnicodeString("result"))));
        int idx = d->at(Value::stringVal(toUnicodeString("index"))).asInt();
        int arity = d->at(Value::stringVal(toUnicodeString("arity"))).asInt();

        result->append(callbackResult);

        idx++;
        d->store(Value::stringVal(toUnicodeString("index")), Value::intVal(idx));

        auto elts = list->getElements();
        if (static_cast<size_t>(idx) < elts.size()) {
            ObjClosure* transform = asClosure(d->at(Value::stringVal(toUnicodeString("transform"))));
            std::vector<Value> callArgs;
            callArgs.push_back(elts[idx]);
            if (arity >= 2)
                callArgs.push_back(Value::intVal(idx));
            return vm.pushContinuationCall(transform, callArgs);
        }

        vm.push(d->at(Value::stringVal(toUnicodeString("result"))));
        return true;
    };

    auto elts = inputList->getElements();
    std::vector<Value> callArgs;
    callArgs.push_back(elts[0]);
    if (arity >= 2)
        callArgs.push_back(Value::intVal(0));

    if (!vm.pushContinuationCall(transform, callArgs))
        throw std::runtime_error("list.map: failed to invoke transform");

    return Value::nilVal();
}

Value ModuleSys::list_reduce_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 3 || !isList(args[0]))
        throw std::invalid_argument("list.reduce expects reducer function and initial value");

    if (!isClosure(args[1]))
        throw std::invalid_argument("list.reduce: first argument must be a function");

    ObjList* inputList = asList(args[0]);
    ObjClosure* reducer = asClosure(args[1]);

    if (inputList->empty())
        return args[2];

    int arity = asFunction(reducer->function)->arity;

    // Continuation state: {list, reducer, accumulator, index, arity}
    Value state = Value::dictVal();
    asDict(state)->store(Value::stringVal(toUnicodeString("list")), args[0]);
    asDict(state)->store(Value::stringVal(toUnicodeString("reducer")), args[1]);
    asDict(state)->store(Value::stringVal(toUnicodeString("accumulator")), args[2]);
    asDict(state)->store(Value::stringVal(toUnicodeString("index")), Value::intVal(0));
    asDict(state)->store(Value::stringVal(toUnicodeString("arity")), Value::intVal(arity));

    auto& cont = vm.thread->pushContinuation();
    cont.state = state;
    cont.resultSlotIndex = static_cast<ptrdiff_t>((vm.thread->stackTop - vm.thread->stack.begin()) - args.size());
    cont.stackBaseIndex = cont.resultSlotIndex + 1;
    cont.onComplete = [](VM& vm, Value callbackResult) -> bool {
        auto& st = vm.thread->currentContinuation().state;
        auto* d = asDict(st);
        ObjList* list = asList(d->at(Value::stringVal(toUnicodeString("list"))));
        int idx = d->at(Value::stringVal(toUnicodeString("index"))).asInt();
        int arity = d->at(Value::stringVal(toUnicodeString("arity"))).asInt();

        d->store(Value::stringVal(toUnicodeString("accumulator")), callbackResult);

        idx++;
        d->store(Value::stringVal(toUnicodeString("index")), Value::intVal(idx));

        auto elts = list->getElements();
        if (static_cast<size_t>(idx) < elts.size()) {
            ObjClosure* reducer = asClosure(d->at(Value::stringVal(toUnicodeString("reducer"))));
            std::vector<Value> callArgs;
            callArgs.push_back(callbackResult);
            callArgs.push_back(elts[idx]);
            if (arity >= 3)
                callArgs.push_back(Value::intVal(idx));
            return vm.pushContinuationCall(reducer, callArgs);
        }

        vm.push(callbackResult);
        return true;
    };

    auto elts = inputList->getElements();
    std::vector<Value> callArgs;
    callArgs.push_back(args[2]);
    callArgs.push_back(elts[0]);
    if (arity >= 3)
        callArgs.push_back(Value::intVal(0));

    if (!vm.pushContinuationCall(reducer, callArgs))
        throw std::runtime_error("list.reduce: failed to invoke reducer");

    return Value::nilVal();
}
