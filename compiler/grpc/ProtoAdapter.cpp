#ifdef ROXAL_ENABLE_GRPC

#include "ProtoAdapter.h"

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/coded_stream.h>

#include <memory>
#include <filesystem>
#include <iostream>
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

    std::string importName = protoFile;
    if (!p.parent_path().empty())
        importName = p.filename().string();

    auto* file_desc = m_importer->Import(importName);
    std::vector<Value> objects;

    if (!file_desc) {
        logError("Unable to import proto file: " + protoFile);
        return objects;
    }

    for (int i = 0; i < file_desc->message_type_count(); i++) {
        const Descriptor* msgDesc = file_desc->message_type(i);
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
                    prop.type = Value::typeSpecVal(ValueType::Int);
                    def = defaultValue(ValueType::Int);
                    break;
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

        m_decls.emplace(obj->name.hashCode(), declVal);
        objects.push_back(declVal);
    }

    return objects;
}


std::vector<std::string> ProtoAdapter::addServices(const std::string& protoFile)
{
    std::filesystem::path p(protoFile);
    if (p.has_parent_path())
        addProtoSearchPath(p.parent_path().string());

    std::string importName = protoFile;
    if (!p.parent_path().empty())
        importName = p.filename().string();

    auto* file_desc = m_importer->Import(importName);
    std::vector<std::string> methods;

    if (!file_desc) {
        logError("Unable to import proto file: " + protoFile);
        return methods;
    }

    for (int i = 0; i < file_desc->service_count(); i++) {
        auto* service = file_desc->service(i);
        m_serviceList.push_back(service);

        for (int j = 0; j < service->method_count(); j++) {
            methods.push_back(service->method(j)->name());
        }
    }

    return methods;
}


std::string ProtoAdapter::generateProtocRequest(const std::string& methodName, const Value& arg)
{
    std::string message = getMessageNameFromMethod(methodName, true);
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

        case FieldDescriptor::CPPTYPE_ENUM:
            refl->SetEnumValue(msg, field, v.asInt());
            break;

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

            case FieldDescriptor::CPPTYPE_ENUM:
                refl->AddEnumValue(msg, field, val.asInt());
                break;

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

    auto declIt = m_decls.find(toUnicodeString(desc->name()).hashCode());
    if (declIt == m_decls.end())
        throw std::runtime_error("No declaration for message type " + desc->name());

    Value instanceVal = Value::objectInstanceVal(declIt->second);
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
            roxField.assign(Value::intVal(static_cast<int32_t>(refl->GetInt64(msg, field))));
            break;

        case FieldDescriptor::CPPTYPE_UINT64:
            roxField.assign(Value::intVal(static_cast<int32_t>(refl->GetUInt64(msg, field))));
            break;

        case FieldDescriptor::CPPTYPE_UINT32:
            roxField.assign(Value::intVal(static_cast<int32_t>(refl->GetUInt32(msg, field))));
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
            roxField.assign(Value::intVal(refl->GetEnumValue(msg, field)));
            break;

        case FieldDescriptor::CPPTYPE_STRING:
            roxField.assign(Value::stringVal(toUnicodeString(refl->GetString(msg, field))));
            break;

        case FieldDescriptor::CPPTYPE_MESSAGE:
        {
            auto declIt = m_decls.find(toUnicodeString(field->message_type()->name()).hashCode());
            if (declIt == m_decls.end()) {
                roxField.assign(Value::nilVal());
                break;
            }

            Value instVal = Value::objectInstanceVal(declIt->second);
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
                list->elts.push_back(Value::intVal(static_cast<int32_t>(refl->GetRepeatedInt64(msg, field, i))));
        }
        break;

        case FieldDescriptor::CPPTYPE_UINT32:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::intVal(static_cast<int32_t>(refl->GetRepeatedUInt32(msg, field, i))));
        }
        break;

        case FieldDescriptor::CPPTYPE_UINT64:
        {
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::intVal(static_cast<int32_t>(refl->GetRepeatedUInt64(msg, field, i))));
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
            for (int i = 0; i < refl->FieldSize(msg, field); i++)
                list->elts.push_back(Value::intVal(refl->GetRepeatedEnumValue(msg, field, i)));
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
            auto declIt = m_decls.find(toUnicodeString(field->message_type()->name()).hashCode());
            if (declIt == m_decls.end()) {
                objField.assign(Value::nilVal());
                return;
            }

            for (int i = 0; i < refl->FieldSize(msg, field); i++) {
                Value instVal = Value::objectInstanceVal(declIt->second);
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


bool ProtoAdapter::nameMatch(const std::string& fullName, const std::string& name) const
{
    if (name.size() > fullName.size())
        return false;

    return fullName.compare(fullName.size() - name.size(),
                            name.size(), name) == 0;
}


void ProtoAdapter::logError(const std::string& errormsg) const
{
    std::cerr << errormsg << std::endl;
}

#endif // ROXAL_ENABLE_GRPC
