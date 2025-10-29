#include "BuiltinModule.h"

#include "Object.h"
#include "dataflow/Signal.h"

#include <stdexcept>

namespace roxal {

roxal::ptr<df::Signal> BuiltinModule::moduleSourceSignal(const std::string& name,
                                                         bool required)
{
    ObjModuleType* modType = asModuleType(moduleType());
    auto maybeValue = modType->vars.load(toUnicodeString(name));

    if (!maybeValue.has_value()) {
        if (required) {
            throw std::runtime_error("Module '" +
                                     toUTF8StdString(modType->name) +
                                     "' is missing source signal '" + name + "'.");
        }
        return nullptr;
    }

    Value value = maybeValue.value();
    if (!isSignal(value)) {
        throw std::runtime_error("Module '" + toUTF8StdString(modType->name) +
                                 "' member '" + name + "' is not a signal.");
    }

    ObjSignal* signalObj = asSignal(value);
    if (!signalObj || !signalObj->signal) {
        throw std::runtime_error("Module '" + toUTF8StdString(modType->name) +
                                 "' signal '" + name + "' is unavailable.");
    }

    auto signal = signalObj->signal;
    if (!signal->isSourceSignal()) {
        throw std::runtime_error("Module '" + toUTF8StdString(modType->name) +
                                 "' signal '" + name + "' is not a source signal.");
    }

    return signal;
}

void BuiltinModule::setModuleSourceSignalValue(const std::string& name,
                                               const Value& value)
{
    auto signal = moduleSourceSignal(name);
    setModuleSourceSignalValue(signal, value, name);
}

void BuiltinModule::setModuleSourceSignalValue(const roxal::ptr<df::Signal>& signal,
                                               const Value& value,
                                               const std::string& signalName)
{
    if (!signal) {
        throw std::runtime_error("Attempted to update a null source signal.");
    }

    if (!signal->isSourceSignal()) {
        throw std::runtime_error("Signal update helper requires a source signal.");
    }

    ValueType expectedType = ValueType::Nil;
    try {
        expectedType = signal->lastValue().type();
    } catch (...) {
        expectedType = ValueType::Nil;
    }

    ValueType actualType = value.type();
    if (expectedType != ValueType::Nil && actualType != expectedType) {
        std::string label = signalName;
        if (label.empty()) {
            label = signal->name();
        }
        if (label.empty()) {
            label = "<unnamed>";
        }
        throw std::runtime_error("Source signal '" + label + "' expects values of type '" +
                                 to_string(expectedType) + "' but received '" +
                                 to_string(actualType) + "'.");
    }

    signal->set(value);
}

} // namespace roxal

