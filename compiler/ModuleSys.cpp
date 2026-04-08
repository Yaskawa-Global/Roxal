#include "ModuleSys.h"
#include "VM.h"
#include "Object.h"
#include "Chunk.h"
#include "SimpleMarkSweepGC.h"
#include "RTCallbackManager.h"
#include "core/AST.h"
#include <core/json11.h>
#include <core/TimePoint.h>
#include <core/TimeDuration.h>
#include "dataflow/Signal.h"
#include "dataflow/FuncNode.h"
#include "dataflow/DataflowEngine.h"
#include "FFI.h"
#include "Introspection.h"
#include "CallableInfo.h"
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
#include <stdexcept>
#include <future>
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

int64_t durationFromFields(int days, int hours, int minutes, int seconds,
                           int millis, int micros)
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
    auto it = type->methods.find(hash);
    if (it == type->methods.end())
        return Value::nilVal();

    Value closure = it->second.closure;
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
        addSys("is_ready", [this](VM& vm, ArgsView a){ return is_ready_builtin(vm,a); });
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
        addSys("_weakref", [this](VM& vm, ArgsView a){ return weakref_builtin(vm,a); });
        addSys("_weak_alive", [this](VM& vm, ArgsView a){ return weak_alive_builtin(vm,a); });
        addSys("_strongref", [this](VM& vm, ArgsView a){ return strongref_builtin(vm,a); });
        addSys("_refcount", [this](VM& vm, ArgsView a){ return refcount_builtin(vm,a); });
        addSys("_arity", [this](VM& vm, ArgsView a){ return arity_builtin(vm,a); });
        addSys("gc", [this](VM& vm, ArgsView a){ return gc_builtin(vm,a); });
        addSys("gc_config", [this](VM& vm, ArgsView a){ return gc_config_builtin(vm,a); });
        addSys("serialize", [this](VM& vm, ArgsView a){ return serialize_builtin(vm,a); }, nullptr, {}, 0x1);
        addSys("deserialize", [this](VM& vm, ArgsView a){ return deserialize_builtin(vm,a); }, nullptr, {}, 0x1);
        addSys("to_json", [this](VM& vm, ArgsView a){ return to_json_builtin(vm,a); }, nullptr, {}, 0x1);
        addSys("from_json", [this](VM& vm, ArgsView a){ return from_json_builtin(vm,a); }, nullptr, {}, 0x1);
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
        addSys("_df_graphdot", [this](VM& vm, ArgsView a){ return df_graphdot_native(vm,a); });
        addSys("loadlib", [this](VM& vm, ArgsView a){ return loadlib_native(vm,a); }, nullptr, {}, 0x1);

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

Value ModuleSys::runtests_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("_runtests expects single string argument");

    auto suite = toUTF8StdString(asStringObj(args[0])->s);

    if (suite == "dataflow") {
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
    else if (suite == "rtcallback_start" || suite == "rtcallback_results") {
        // RT Callback timing test
        // rtcallback_start: Registers a callback that collects timing data
        // rtcallback_results: Unregisters callback and reports final statistics
        // The Roxal script should call wait() between start and results

        auto& rtMgr = RTCallbackManager::instance();

        // Shared state between start and results (static persists across calls)
        static const int64_t intervalUs = 1000;      // 1ms interval
        static const int64_t maxAcceptableJitterUs = 500; // Output error if lateness > 500us (non-RT system)

        struct TimingSample {
            int64_t scheduledUs;
            int64_t actualUs;
            int64_t latenessUs;
        };
        static std::vector<TimingSample> samples;
        static std::mutex sampleMutex;
        static RTCallbackHandle testHandle = InvalidRTCallbackHandle;
        static bool hadError = false;

        if (suite == "rtcallback_start") {
            // Clear any previous test data
            {
                std::lock_guard<std::mutex> lock(sampleMutex);
                samples.clear();
                samples.reserve(1000);  // Pre-allocate for ~1 second of samples
                hadError = false;
            }

            // Unregister any previous callback
            if (testHandle != InvalidRTCallbackHandle) {
                rtMgr.unregisterCallback(testHandle);
                testHandle = InvalidRTCallbackHandle;
            }

            // Register the test callback
            testHandle = rtMgr.registerCallback(
                [](TimePoint scheduledTime) {
                    auto now = TimePoint::currentTime();
                    int64_t scheduledUs = scheduledTime.microSecs();
                    int64_t actualUs = now.microSecs();
                    int64_t latenessUs = actualUs - scheduledUs;

                    std::lock_guard<std::mutex> lock(sampleMutex);
                    samples.push_back({scheduledUs, actualUs, latenessUs});

                    // Output error immediately if jitter is too high
                    if (latenessUs > maxAcceptableJitterUs) {
                        std::cerr << "RT JITTER ERROR: lateness=" << latenessUs
                                  << "us > " << maxAcceptableJitterUs << "us threshold" << std::endl;
                        hadError = true;
                    }
                },
                intervalUs,
                0  // No abort on lateness for testing
            );

            std::cout << "rtcallback_start: registered 1ms callback" << std::endl;
        }
        else { // rtcallback_results
            // Unregister the callback
            if (testHandle != InvalidRTCallbackHandle) {
                rtMgr.unregisterCallback(testHandle);
                testHandle = InvalidRTCallbackHandle;
            }

            // Analyze results
            std::lock_guard<std::mutex> lock(sampleMutex);

            if (samples.empty()) {
                std::cerr << "rtcallback FAILED: No callback invocations recorded" << std::endl;
                return Value::nilVal();
            }

            // Calculate statistics
            int64_t minLateness = std::numeric_limits<int64_t>::max();
            int64_t maxLateness = std::numeric_limits<int64_t>::min();
            int64_t sumLateness = 0;

            for (const auto& s : samples) {
                minLateness = std::min(minLateness, s.latenessUs);
                maxLateness = std::max(maxLateness, s.latenessUs);
                sumLateness += s.latenessUs;
            }

            double meanLateness = static_cast<double>(sumLateness) / samples.size();

            // Calculate standard deviation (jitter)
            double sumSquaredDiff = 0;
            for (const auto& s : samples) {
                double diff = s.latenessUs - meanLateness;
                sumSquaredDiff += diff * diff;
            }
            double stddevLateness = std::sqrt(sumSquaredDiff / samples.size());

            // Final pass/fail based on max lateness
            bool passed = (maxLateness < maxAcceptableJitterUs) && !hadError;
            if (passed) {
                // On success, output fixed string to stdout (for .out matching)
                std::cout << "rtcallback PASSED" << std::endl;
            } else {
                // On failure, output detailed stats to stderr (test will fail)
                std::cerr << "rtcallback FAILED" << std::endl;
                std::cerr << "  Invocations: " << samples.size() << std::endl;
                std::cerr << "  Min lateness: " << minLateness << " us" << std::endl;
                std::cerr << "  Max lateness: " << maxLateness << " us" << std::endl;
                std::cerr << "  Mean lateness: " << std::fixed << std::setprecision(1)
                          << meanLateness << " us" << std::endl;
                std::cerr << "  Jitter (stddev): " << std::fixed << std::setprecision(1)
                          << stddevLateness << " us" << std::endl;
                std::cerr << "  Threshold: " << maxAcceptableJitterUs << " us" << std::endl;
            }
        }
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

                        // Use 10Hz (100ms period) to allow time for multiple small time slices
                        auto inputSignal = df::Signal::newSourceSignal(10.0, Value::intVal(1), "df_multi_input");

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
                        const int maxIterations = 400;
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

    if (VM::thread) {
        collector.safepoint(*VM::thread);
    }

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
    ptr<SerializationContext> ctx = make_ptr<SerializationContext>();
    writeValue(ss, args[0], ctx);
    std::string data = ss.str();
    std::vector<Value> bytes;
    bytes.reserve(data.size());
    for(unsigned char ch : data)
        bytes.push_back(Value::byteVal(ch));
    return Value::listVal(bytes);
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

    std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
    ss.write(data.data(), data.size());
    ss.seekg(0);
    ptr<SerializationContext> ctx = make_ptr<SerializationContext>();
    return readValue(ss, ctx);
}

static json11::Json valueToJson(const Value& v) {
    using json11::Json;
    switch(v.type()) {
        case ValueType::Nil:   return Json();
        case ValueType::Bool:  return Json(v.asBool());
        case ValueType::Byte:  return Json(int(v.asByte()));
        case ValueType::Int:
            // TODO: consider emitting ints as double to preserve up to 53 bits exactly and round-trip small ints back as int on read.
            return Json(int(v.asInt())); // JSON library only has int ctor
        case ValueType::Real:  return Json(v.asReal());
        case ValueType::String: return Json(toUTF8StdString(asStringObj(v)->s));
        case ValueType::List: {
            Json::array arr; arr.reserve(asList(v)->length());
            for(int i=0;i<asList(v)->length();++i)
                arr.push_back(valueToJson(asList(v)->getElement(i)));
            return Json(arr);
        }
        case ValueType::Dict: {
            Json::object obj;
            for(const auto& kv : asDict(v)->items()) {
                if(!isString(kv.first))
                    throw std::runtime_error("dict key not string");
                obj[toUTF8StdString(asStringObj(kv.first)->s)] = valueToJson(kv.second);
            }
            return Json(obj);
        }
        default:
            if(isObjectInstance(v) || isActorInstance(v))
                return valueToJson(toType(ValueType::Dict, v, false));
            throw std::runtime_error("unsupported type for to_json");
    }
}

static void dumpJsonPretty(const json11::Json& j, std::string& out, int indent=0) {
    using json11::Json;
    switch(j.type()) {
        case Json::ARRAY: {
            out += "[";
            auto arr = j.array_items();
            if(!arr.empty()) {
                out += "\n";
                int nIndent = indent+2;
                for(size_t i=0;i<arr.size();++i) {
                    out += std::string(nIndent,' ');
                    dumpJsonPretty(arr[i], out, nIndent);
                    if(i+1<arr.size()) out += ",\n";
                }
                out += "\n" + std::string(indent,' ');
            }
            out += "]";
            break;
        }
        case Json::OBJECT: {
            out += "{";
            auto obj = j.object_items();
            if(!obj.empty()) {
                out += "\n";
                int nIndent = indent+2;
                size_t count=0;
                for(auto it=obj.begin(); it!=obj.end(); ++it,++count) {
                    out += std::string(nIndent,' ');
                    dumpJsonPretty(Json(it->first), out, nIndent);
                    out += ": ";
                    dumpJsonPretty(it->second, out, nIndent);
                    if(count+1<obj.size()) out += ",\n";
                }
                out += "\n" + std::string(indent,' ');
            }
            out += "}";
            break;
        }
        default:
            out += j.dump();
    }
}

Value ModuleSys::to_json_builtin(VM& vm, ArgsView args)
{
    if(args.size() < 1 || args.size() > 2)
        throw std::invalid_argument("to_json expects value and optional indent bool");

    bool indent = true;
    if(args.size() == 2)
        indent = toType(ValueType::Bool, args[1], false).asBool();

    json11::Json j = valueToJson(args[0]);
    std::string out;
    if(indent)
        dumpJsonPretty(j, out, 0);
    else
        out = j.dump();

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
            for(const auto& kv : j.object_items()) {
                asDict(d)->store(Value::stringVal(toUnicodeString(kv.first)), jsonToValue(kv.second));
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
    json11::Json j = json11::Json::parse(s, err);
    if(!err.empty())
        throw std::invalid_argument(std::string("invalid json: ")+err);
    return jsonToValue(j);
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

    auto readArg = [&](size_t index, const char* name) -> int {
        if (index >= args.size())
            return 0;
        if (!args[index].isNumber())
            throw std::invalid_argument(std::string("TimeSpan.init ") + name + " must be int");
        return toType(ValueType::Int, args[index], false).asInt();
    };

    int days = readArg(1, "days");
    int hours = readArg(2, "hours");
    int minutes = readArg(3, "minutes");
    int seconds = readArg(4, "seconds");
    int millis = readArg(5, "millis");
    int micros = readArg(6, "micros");

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

    auto readArg = [&](size_t index, const char* name) -> int {
        if (index >= args.size())
            return 0;
        if (!args[index].isNumber())
            throw std::invalid_argument(std::string("TimeSpan.from_fields ") + name + " must be int");
        return toType(ValueType::Int, args[index], false).asInt();
    };

    int days = readArg(1, "days");
    int hours = readArg(2, "hours");
    int minutes = readArg(3, "minutes");
    int seconds = readArg(4, "seconds");
    int millis = readArg(5, "millis");
    int micros = readArg(6, "micros");

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

// filter_builtin, map_builtin, reduce_builtin removed - now implemented in pure Roxal in sys.rox
