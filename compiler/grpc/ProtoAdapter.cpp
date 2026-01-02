#ifdef ROXAL_ENABLE_GRPC

#include "ProtoAdapter.h"

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/coded_stream.h>

#include <memory>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace google::protobuf;
using namespace google::protobuf::io;
using namespace google::protobuf::compiler;

class ErrorPrinter : public MultiFileErrorCollector {
public:
    ErrorPrinter() = default;

    void AddError(const std::string& filename, int line, int column,
                  const std::string& message) override {
        std::string errorMsg = "Error in " + filename + " at line " +
                               std::to_string(line) + " and column " +
                               std::to_string(column) + ": " + message;
        std::cerr << errorMsg << std::endl;
    }

    void AddWarning(const std::string& filename, int line, int column,
                    const std::string& message) override {
        std::cerr << "warning " << filename << " " << line << " " << column << " "
                  << message << std::endl;
    }
};

using namespace roxal;

ProtoAdapter::ProtoAdapter(const std::string& proto_path)
{
    if (!proto_path.empty())
        addProtoSearchPath(proto_path);

    m_errorPrinter = std::unique_ptr<ErrorPrinter>(new ErrorPrinter());
    m_importer = std::unique_ptr<Importer>(new Importer(&m_sourceTree, m_errorPrinter.get()));
    m_dynfactory = std::unique_ptr<DynamicMessageFactory>(new DynamicMessageFactory());
}

ProtoAdapter::~ProtoAdapter() = default;

std::string ProtoAdapter::canonicalPath(const std::string& path) const
{
    try {
        return std::filesystem::weakly_canonical(path).string();
    } catch (...) {
        return path;
    }
}

void ProtoAdapter::addProtoSearchPath(const std::string& path)
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    if (path.empty())
        return;

    auto canonical = canonicalPath(path);
    if (canonical.empty())
        return;

    if (m_searchPaths.insert(canonical).second)
        m_sourceTree.MapPath("", canonical);
}


std::string ProtoAdapter::getFullMethodName(const std::string& methodName) const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    for (const auto* service : m_serviceList) {
        if (!service) continue;
        for (int j = 0; j < service->method_count(); j++) {
            const auto* methodDesc = service->method(j);
            if (nameMatch(methodDesc->full_name(), methodName))
                return methodDesc->full_name();
        }
    }
    logError("Cannot find full method name for " + methodName);
    return "";
}

//--- RPC Methods ---//

std::string ProtoAdapter::getFullMessageName(const std::string& message) const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    for (const auto* service : m_serviceList) {
        if (!service) continue;
        const auto* fileDesc = service->file();

        for (int j = 0; j < fileDesc->message_type_count(); j++) {
            const auto* desc = fileDesc->message_type(j);

            if (nameMatch(desc->full_name(), message))
                return desc->full_name();
        }
    }

    logError("Cannot find full message name for " + message);
    return "";
}


std::string ProtoAdapter::getFormattedMethodName(const std::string& methodName) const
{
    std::string fullName = getFullMethodName(methodName);
    size_t last_dot = fullName.find_last_of('.');
    if (last_dot != std::string::npos) {
        fullName[last_dot] = '/';
    }
    fullName.insert(fullName.begin(), '/');
    return fullName;
}

std::string ProtoAdapter::getMessageNameFromMethod(const std::string& methodName, bool isRequest)
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    std::string fullName = getFullMethodName(methodName);
    const DescriptorPool* pool = m_importer->pool();

    const auto* method = pool->FindMethodByName(fullName);

    if (!method) {
        logError("Method not found: " + methodName);
        return "";
    }

    return isRequest ? method->input_type()->full_name() : method->output_type()->full_name();
}


std::vector<Value> ProtoAdapter::allocateObjects(const std::string& protoFile)
{
    std::filesystem::path p(protoFile);
    if (p.has_parent_path())
        addProtoSearchPath(p.parent_path().string());

    std::lock_guard<std::recursive_mutex> lk(m_mutex);

    std::string importName = protoFile;
    if (!p.parent_path().empty())
        importName = p.filename().string();

    auto* file_desc = m_importer->Import(importName);
    std::vector<Value> objects;

    if (!file_desc) {
        logError("Unable to import proto file: " + protoFile);
        return objects;
    }

    // Process dependencies first (imported proto files)
    for (int d = 0; d < file_desc->dependency_count(); ++d) {
        const auto* depFile = file_desc->dependency(d);
        if (depFile) {
            // Recursively allocate objects from dependency (if not already processed)
            // Use file name as marker since multiple files can share same package
            std::string depFileName = depFile->name();
            std::string processedKey = "_file_processed:" + depFileName;
            if (m_declByFullName.find(processedKey) == m_declByFullName.end()) {
                // Mark as processed to avoid infinite recursion
                m_declByFullName[processedKey] = Value::nilVal();
                auto depObjects = allocateObjectsFromFileDesc(depFile);
                objects.insert(objects.end(), depObjects.begin(), depObjects.end());
            }
        }
    }

    m_lastPackage = file_desc->package();

    // Use helper to register types from this file
    auto mainObjects = allocateObjectsFromFileDesc(file_desc);
    objects.insert(objects.end(), mainObjects.begin(), mainObjects.end());

    return objects;
}

std::vector<Value> ProtoAdapter::allocateObjectsFromFileDesc(const google::protobuf::FileDescriptor* file_desc)
{
    std::vector<Value> objects;

    if (!file_desc)
        return objects;

    auto registerEnum = [&](const EnumDescriptor* enumDesc) {
        if (!enumDesc)
            return;

        // Skip if already registered
        if (m_declByFullName.find(enumDesc->full_name()) != m_declByFullName.end())
            return;

        Value declVal = Value::objectTypeVal(toUnicodeString(enumDesc->name()), false, false, true);
        ObjObjectType* enumObj = asObjectType(declVal);

        for (int v = 0; v < enumDesc->value_count(); ++v) {
            const auto* valueDesc = enumDesc->value(v);
            icu::UnicodeString labelName = toUnicodeString(valueDesc->name());
            int number = valueDesc->number();
            if (number < std::numeric_limits<int16_t>::min() || number > std::numeric_limits<int16_t>::max()) {
                throw std::runtime_error("Enum value '" + valueDesc->name() + "' in " + enumDesc->full_name() +
                                         " is out of range for Roxal enums.");
            }
            Value enumValue = Value::enumVal(static_cast<int16_t>(number), enumObj->enumTypeId);
            enumObj->enumLabelValues[labelName.hashCode()] = std::make_pair(labelName, enumValue);
        }

        m_declByFullName.emplace(enumDesc->full_name(), declVal);
        m_declByShortName.emplace(enumDesc->name(), declVal);
        objects.push_back(declVal);
    };

    std::function<void(const Descriptor*)> registerNestedEnums = [&](const Descriptor* desc) {
        if (!desc)
            return;
        for (int e = 0; e < desc->enum_type_count(); ++e)
            registerEnum(desc->enum_type(e));
        for (int nested = 0; nested < desc->nested_type_count(); ++nested)
            registerNestedEnums(desc->nested_type(nested));
    };

    for (int e = 0; e < file_desc->enum_type_count(); ++e)
        registerEnum(file_desc->enum_type(e));

    for (int i = 0; i < file_desc->message_type_count(); i++) {
        const Descriptor* msgDesc = file_desc->message_type(i);

        // Skip if already registered
        if (m_declByFullName.find(msgDesc->full_name()) != m_declByFullName.end())
            continue;

        registerNestedEnums(msgDesc);

        Value declVal = Value::objectTypeVal(toUnicodeString(msgDesc->name()), false);
        ObjObjectType* obj = asObjectType(declVal);

        for (int f = 0; f < msgDesc->field_count(); f++) {
            const auto* field = msgDesc->field(f);
            ObjObjectType::Property prop;
            prop.name = toUnicodeString(field->name());
            prop.ownerType = declVal.weakRef();

            Value def;
            switch(field->cpp_type()) {
                case FieldDescriptor::CPPTYPE_BOOL:
                    prop.type = Value::typeSpecVal(ValueType::Bool);
                    def = defaultValue(ValueType::Bool);
                    break;
                case FieldDescriptor::CPPTYPE_DOUBLE:
                case FieldDescriptor::CPPTYPE_FLOAT:
                    prop.type = Value::typeSpecVal(ValueType::Real);
                    def = defaultValue(ValueType::Real);
                    break;
                case FieldDescriptor::CPPTYPE_UINT32:
                case FieldDescriptor::CPPTYPE_UINT64:
                case FieldDescriptor::CPPTYPE_INT32:
                case FieldDescriptor::CPPTYPE_INT64:
                case FieldDescriptor::CPPTYPE_ENUM:
                {
                    if (field->cpp_type() == FieldDescriptor::CPPTYPE_ENUM) {
                        Value enumDecl = declForFullName(field->enum_type()->full_name());
                        if (!enumDecl.isNil() && isEnumType(enumDecl)) {
                            prop.type = enumDecl;
                            const EnumValueDescriptor* defaultDesc = field->default_value_enum();
                            int defaultNumber = defaultDesc ? defaultDesc->number() : 0;
                            def = enumValueFromNumber(field->enum_type(), defaultNumber);
                        } else {
                            prop.type = Value::typeSpecVal(ValueType::Enum);
                            def = defaultValue(ValueType::Int);
                        }
                    } else {
                        prop.type = Value::typeSpecVal(ValueType::Int);
                        def = defaultValue(ValueType::Int);
                    }
                    break;
                }
                case FieldDescriptor::CPPTYPE_MESSAGE:
                    prop.type = Value::nilVal();
                    def = defaultValue(ValueType::Nil);
                    break;
                case FieldDescriptor::CPPTYPE_STRING:
                    prop.type = Value::typeSpecVal(ValueType::String);
                    def = defaultValue(ValueType::String);
                    break;
                default:
                    prop.type = Value::nilVal();
                    def = defaultValue(ValueType::Nil);
                    break;
            }

            if (field->is_repeated()) {
                prop.type = Value::typeSpecVal(ValueType::List);
                def = Value::listVal();
            }

            prop.initialValue = def;

            auto hash = prop.name.hashCode();
            obj->properties.emplace(hash, prop);
            obj->propertyOrder.push_back(hash);
        }

        m_declByFullName.emplace(msgDesc->full_name(), declVal);
        m_declByShortName.emplace(msgDesc->name(), declVal);
        objects.push_back(declVal);
    }

    return objects;
}


std::vector<ProtoAdapter::ServiceInfo> ProtoAdapter::addServices(const std::string& protoFile)
{
    std::filesystem::path p(protoFile);
    if (p.has_parent_path())
        addProtoSearchPath(p.parent_path().string());

    std::lock_guard<std::recursive_mutex> lk(m_mutex);

    std::string importName = protoFile;
    if (!p.parent_path().empty())
        importName = p.filename().string();

    auto* file_desc = m_importer->Import(importName);
    std::vector<ServiceInfo> services;

    if (!file_desc) {
        logError("Unable to import proto file: " + protoFile);
        return services;
    }

    m_lastPackage = file_desc->package();

    for (int i = 0; i < file_desc->service_count(); i++) {
        auto* service = file_desc->service(i);
        m_serviceList.push_back(service);
        ServiceInfo info;
        info.name = service->name();
        info.package = file_desc->package();
        for (int j = 0; j < service->method_count(); j++) {
            auto* methodDesc = service->method(j);
            ServiceInfo::Method m;
            m.name = methodDesc->name();
            m.inputTypeFullName = methodDesc->input_type()->full_name();
            m.outputTypeFullName = methodDesc->output_type()->full_name();
            m.clientStreaming = methodDesc->client_streaming();
            m.serverStreaming = methodDesc->server_streaming();
            info.methods.push_back(std::move(m));
        }
        services.push_back(std::move(info));
    }

    return services;
}


std::string ProtoAdapter::generateProtocRequest(const std::string& methodName, const Value& arg)
{
    std::string message = getMessageNameFromMethod(methodName, true);
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto* desc = m_importer->pool()->FindMessageTypeByName(message);
    if (!desc)
        throw std::runtime_error("generateProtocRequest() - unknown request type for " + methodName);

    std::unique_ptr<Message> msg(m_dynfactory->GetPrototype(desc)->New());
    if (!isObjectInstance(arg))
        throw std::runtime_error("generateProtocRequest() - expected arg to be request message instance of type "+message);
    ObjectInstance* instance = asObjectInstance(arg);
    generateProtocSubRequest(msg.get(), instance);

    std::string serialized;
    if(!msg->SerializeToString(&serialized))
        throw std::runtime_error("generateProtocRequest() - Unable to serialize message");

    return serialized;
}


void ProtoAdapter::generateProtocSubRequest(Message* msg, ObjectInstance* instance)
{
    auto* desc = msg->GetDescriptor();

    for (int i = 0; i < desc->field_count(); i++) {
        auto* field = desc->field(i);
        int32_t hash = toUnicodeString(field->name()).hashCode();
        auto it = instance->properties.find(hash);
        Value v = (it != instance->properties.end()) ? it->second.value : Value::nilVal();

        if (field->is_repeated())
            buildRepeatedReqField(msg, field, v);
        else
            buildReqField(msg, field, v);
    }
}

void ProtoAdapter::buildReqField(Message* msg, const FieldDescriptor* field, Value& v)
{
    auto* refl = msg->GetReflection();
    switch(field->cpp_type())
    {
        case FieldDescriptor::CPPTYPE_BOOL:
            refl->SetBool(msg, field, v.asBool());
            break;

        case FieldDescriptor::CPPTYPE_DOUBLE:
            refl->SetDouble(msg, field, v.asReal());
            break;

        case FieldDescriptor::CPPTYPE_ENUM: {
            int enumValue = v.isEnum() ? static_cast<int>(v.asEnum()) : v.asInt();
            refl->SetEnumValue(msg, field, enumValue);
            break;
        }

        case FieldDescriptor::CPPTYPE_FLOAT:
            refl->SetFloat(msg, field, v.asReal());
            break;

        case FieldDescriptor::CPPTYPE_INT32:
            refl->SetInt32(msg, field, v.asInt());
            break;

        case FieldDescriptor::CPPTYPE_INT64:
            refl->SetInt64(msg, field, v.asInt());
            break;

        case FieldDescriptor::CPPTYPE_UINT32:
            refl->SetUInt32(msg, field, v.asInt());
            break;

        case FieldDescriptor::CPPTYPE_UINT64:
            refl->SetUInt64(msg, field, v.asInt());
            break;

        case FieldDescriptor::CPPTYPE_STRING:
            refl->SetString(msg, field, toUTF8StdString(asStringObj(v)->s));
            break;

        case FieldDescriptor::CPPTYPE_MESSAGE:
        {
            if (v.isNil())
                break;
            if (!isObjectInstance(v))
                throw std::invalid_argument("Expected object instance for message field "+field->name());
            Message* innerMsg = refl->MutableMessage(msg, field);
            generateProtocSubRequest(innerMsg, asObjectInstance(v));
        }
        break;

        default:
            throw std::runtime_error("Unimplemented/Unsupported data type in buildReqField");
    }
}

void ProtoAdapter::buildRepeatedReqField(Message* msg, const FieldDescriptor* field, Value& arg)
{
    if (!isList(arg))
        throw std::invalid_argument("Expected list for repeated field " + field->name());

    ObjList* list = asList(arg);
    auto* refl = msg->GetReflection();

    auto values = list->elts.get();
    for (const auto& val : values) {
        switch(field->cpp_type())
        {
            case FieldDescriptor::CPPTYPE_BOOL:
                refl->AddBool(msg, field, val.asBool());
                break;

            case FieldDescriptor::CPPTYPE_DOUBLE:
                refl->AddDouble(msg, field, val.asReal());
                break;

            case FieldDescriptor::CPPTYPE_ENUM: {
                int enumValue = val.isEnum() ? static_cast<int>(val.asEnum()) : val.asInt();
                refl->AddEnumValue(msg, field, enumValue);
                break;
            }

            case FieldDescriptor::CPPTYPE_FLOAT:
                refl->AddFloat(msg, field, val.asReal());
                break;

            case FieldDescriptor::CPPTYPE_INT32:
                refl->AddInt32(msg, field, val.asInt());
                break;

            case FieldDescriptor::CPPTYPE_INT64:
                refl->AddInt64(msg, field, val.asInt());
                break;

            case FieldDescriptor::CPPTYPE_UINT32:
                refl->AddUInt32(msg, field, val.asInt());
                break;

            case FieldDescriptor::CPPTYPE_UINT64:
                refl->AddUInt64(msg, field, val.asInt());
                break;

            case FieldDescriptor::CPPTYPE_STRING:
                refl->AddString(msg, field, toUTF8StdString(asStringObj(val)->s));
                break;

            case FieldDescriptor::CPPTYPE_MESSAGE:
            {
                if (val.isNil())
                    break;
                if (!isObjectInstance(val))
                    throw std::invalid_argument("Expected object instance for message field "+field->name());
                Message* innermsg = refl->AddMessage(msg, field);
                generateProtocSubRequest(innermsg, asObjectInstance(val));
            }
            break;

            default:
                throw std::runtime_error("Unimplemented/Unsupported data type in buildRepeatedReqField");
        }
    }
}


Value ProtoAdapter::generateRoxalResponse(const std::string& methodName, const std::string& response)
{
    std::string message = getMessageNameFromMethod(methodName, false);
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto* desc = m_importer->pool()->FindMessageTypeByName(message);
    if (!desc) {
        logError("Could not find response descriptor for " + methodName);
        return Value::nilVal();
    }

    std::unique_ptr<Message> msg(m_dynfactory->GetPrototype(desc)->New());

    if(!msg->ParseFromArray(response.data(), static_cast<int>(response.size()))) {
        std::cerr << "Could not parse response for method " << methodName << std::endl;
        return Value::nilVal();
    }

    Value declVal = declForFullName(desc->full_name());
    if (declVal.isNil())
        throw std::runtime_error("No declaration for message type " + desc->full_name());

    Value instanceVal = Value::objectInstanceVal(declVal);
    ObjectInstance* instance = asObjectInstance(instanceVal);

    generateSubResponse(*msg, instance);
    return instanceVal;
}


void ProtoAdapter::generateSubResponse(const Message& msg, ObjectInstance* instance)
{
    auto* desc = msg.GetDescriptor();
    auto* refl = msg.GetReflection();

    for (int i = 0; i < desc->field_count(); i++) {
        auto* field = desc->field(i);
        int32_t hash = toUnicodeString(field->name()).hashCode();
        auto& slot = instance->properties[hash];

        if (field->is_repeated())
            buildRepeatedRespField(msg, field, slot);
        else
            buildRespField(msg, field, slot);
    }
}


void ProtoAdapter::buildRespField(const Message& msg, const FieldDescriptor* field, VariablesMap::MonitoredValue& roxField)
{
    auto* refl = msg.GetReflection();

    switch (field->cpp_type())
    {
        case FieldDescriptor::CPPTYPE_INT32:
            roxField.assign(Value::intVal(refl->GetInt32(msg, field)));
            break;

        case FieldDescriptor::CPPTYPE_INT64:
            roxField.assign(Value::intVal(refl->GetInt64(msg, field)));
            break;

        case FieldDescriptor::CPPTYPE_UINT64:
        {
            uint64_t raw = refl->GetUInt64(msg, field);
            roxField.assign(Value::intVal(static_cast<int64_t>(raw)));
        }
            break;

        case FieldDescriptor::CPPTYPE_UINT32:
            roxField.assign(Value::intVal(static_cast<int64_t>(refl->GetUInt32(msg, field))));
            break;

        case FieldDescriptor::CPPTYPE_DOUBLE:
            roxField.assign(Value::realVal(refl->GetDouble(msg, field)));
            break;

        case FieldDescriptor::CPPTYPE_FLOAT:
            roxField.assign(Value::realVal(refl->GetFloat(msg, field)));
            break;

        case FieldDescriptor::CPPTYPE_BOOL:
            roxField.assign(Value::boolVal(refl->GetBool(msg, field)));
            break;

        case FieldDescriptor::CPPTYPE_ENUM:
        {
            int enumNumber = refl->GetEnumValue(msg, field);
            roxField.assign(enumValueFromNumber(field->enum_type(), enumNumber));
        }
        break;

        case FieldDescriptor::CPPTYPE_STRING:
            roxField.assign(Value::stringVal(toUnicodeString(refl->GetString(msg, field))));
            break;

        case FieldDescriptor::CPPTYPE_MESSAGE:
        {
            Value declVal = declForFullName(field->message_type()->full_name());
            if (declVal.isNil()) {
                std::cerr << "DEBUG: declForFullName returned nil for: " << field->message_type()->full_name() << std::endl;
                roxField.assign(Value::nilVal());
                break;
            }

            Value instVal = Value::objectInstanceVal(declVal);
            generateSubResponse(refl->GetMessage(msg, field), asObjectInstance(instVal));
            roxField.assign(instVal);
        }
        break;

        default:
        {
            std::cout << "Undefined message type for field " << field->name() << std::endl;
            roxField.assign(Value::nilVal());
        }
        break;
    }
}

void ProtoAdapter::buildRepeatedRespField(const Message& msg, const FieldDescriptor* field, VariablesMap::MonitoredValue& objField)
{
    auto* refl = msg.GetReflection();
    Value listVal = Value::listVal();
    ObjList* list = asList(listVal);

    switch (field->cpp_type())
    {
        case FieldDescriptor::CPPTYPE_INT32:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::intVal(refl->GetRepeatedInt32(msg, field, i)));
        }
        break;

        case FieldDescriptor::CPPTYPE_INT64:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::intVal(refl->GetRepeatedInt64(msg, field, i)));
        }
        break;

        case FieldDescriptor::CPPTYPE_UINT32:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::intVal(static_cast<int64_t>(refl->GetRepeatedUInt32(msg, field, i))));
        }
        break;

        case FieldDescriptor::CPPTYPE_UINT64:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++) {
                uint64_t raw = refl->GetRepeatedUInt64(msg, field, i);
                list->elts.push_back(Value::intVal(static_cast<int64_t>(raw)));
            }
        }
        break;

        case FieldDescriptor::CPPTYPE_BOOL:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::boolVal(refl->GetRepeatedBool(msg, field, i)));
        }
        break;

        case FieldDescriptor::CPPTYPE_ENUM:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++) {
                int enumNumber = refl->GetRepeatedEnumValue(msg, field, i);
                list->elts.push_back(enumValueFromNumber(field->enum_type(), enumNumber));
            }
        }
        break;

        case FieldDescriptor::CPPTYPE_FLOAT:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::realVal(refl->GetRepeatedFloat(msg, field, i)));
        }
        break;

        case FieldDescriptor::CPPTYPE_DOUBLE:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::realVal(refl->GetRepeatedDouble(msg, field, i)));
        }
        break;

        case FieldDescriptor::CPPTYPE_STRING:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::stringVal(toUnicodeString(refl->GetRepeatedString(msg, field, i))));
        }
        break;

        case FieldDescriptor::CPPTYPE_MESSAGE:
        {
            Value declVal = declForFullName(field->message_type()->full_name());
            if (declVal.isNil()) {
                objField.assign(Value::nilVal());
                return;
            }

            for (int i = 0; i < refl->FieldSize(msg, field); i++) {
                Value instVal = Value::objectInstanceVal(declVal);
                generateSubResponse(refl->GetRepeatedMessage(msg, field, i), asObjectInstance(instVal));
                list->elts.push_back(instVal);
            }
        }
        break;

        default:
        {
            std::cout << "Undefined message type " << field->cpp_type() << " for repeated field " << field->name() << std::endl;
            list->elts.push_back(Value::nilVal());
        }
        break;
    }

    objField.assign(listVal);
}

ObjObjectType* ProtoAdapter::enumTypeFromDescriptor(const EnumDescriptor* enumDesc) const
{
    if (!enumDesc)
        return nullptr;

    Value declVal = declForFullName(enumDesc->full_name());
    if (declVal.isNil() || !isEnumType(declVal))
        return nullptr;

    return asObjectType(declVal);
}

Value ProtoAdapter::enumValueFromNumber(const EnumDescriptor* enumDesc, int number) const
{
    if (!enumDesc)
        return Value::intVal(number);

    ObjObjectType* enumType = enumTypeFromDescriptor(enumDesc);
    if (!enumType)
        return Value::intVal(number);

    if (number < std::numeric_limits<int16_t>::min() ||
        number > std::numeric_limits<int16_t>::max()) {
        throw std::runtime_error(
            "Enum value out of range when constructing '" + enumDesc->full_name() + "'");
    }

    return Value::enumVal(static_cast<int16_t>(number), enumType->enumTypeId);
}


bool ProtoAdapter::nameMatch(const std::string& fullName, const std::string& name) const
{
    if (fullName == name)
        return true;
    auto pos = fullName.find_last_of('.');
    if (pos != std::string::npos) {
        std::string shortName = fullName.substr(pos + 1);
        return shortName == name;
    }
    return false;
}

Value ProtoAdapter::declForFullName(const std::string& fullName) const
{
    auto it = m_declByFullName.find(fullName);
    if (it != m_declByFullName.end())
        return it->second;
    auto pos = fullName.find_last_of('.');
    if (pos != std::string::npos) {
        std::string shortName = fullName.substr(pos + 1);
        auto sit = m_declByShortName.find(shortName);
        if (sit != m_declByShortName.end())
            return sit->second;
    }
    return Value::nilVal();
}


void ProtoAdapter::logError(const std::string& errormsg) const
{
    std::cerr << errormsg << std::endl;
}

#endif // ROXAL_ENABLE_GRPC
