#include "ProtoAdapter.h"

class ErrorPrinter : public MultiFileErrorCollector {
 public:
  ErrorPrinter()
  {}

  void AddError(const std::string& filename, int line, int column,
                const std::string& message) override {

    std::string errorMsg = "Error in " + filename + " at line " + std::to_string(line) + " and column " + std::to_string(column);
    std::cout << errorMsg << std::endl;
  }

  void AddWarning(const std::string& filename, int line, int column,
                  const std::string& message) override {
    std::cerr << "warning " << filename << " " << line << " " << column << " "
              << message << std::endl;
  }

};

using namespace roxal;

ProtoAdapter::ProtoAdapter(const std::string &proto_path)
{

    //Add the proto path to the source tree
    m_sourceTree.MapPath("", proto_path);

    //Initialize an Error Printer
    m_errorPrinter = std::unique_ptr<ErrorPrinter>(new ErrorPrinter());

    //Make an Importer Object That Will Parse Proto Files
    m_importer = std::unique_ptr<Importer>(new Importer(&m_sourceTree, m_errorPrinter.get()));

    //Create a message factory to create messages on the fly
    m_dynfactory = std::unique_ptr<DynamicMessageFactory>(new DynamicMessageFactory());
 
}

ProtoAdapter::~ProtoAdapter()
{}

std::string ProtoAdapter::getFullMethodName(const std::string &methodName)
{

    for (int i = 0; i < m_serviceList.size(); i++){

        for (int j = 0; j < m_serviceList[i]->method_count(); j++){
            const auto *methodDesc = m_serviceList[i]->method(j);

            if (nameMatch(methodDesc->full_name(), methodName))
                return methodDesc->full_name();
        }
    }
    logError("Cannot find full method name");
    return "";
}

//--- RPC Methods ---//

std::string ProtoAdapter::getFullMessageName(const std::string &message)
{
    for (int i = 0; i < m_serviceList.size(); i++)
    {
        const auto *fileDesc = m_serviceList[i]->file();

        for (int j = 0; j < fileDesc->message_type_count(); j++)
        {
            const auto *desc = fileDesc->message_type(j);

            if (nameMatch(desc->full_name(), message))
                return desc->full_name();
        }
    }

    logError("Cannot find full message name");
    return "";
}


std::string ProtoAdapter::getFormattedMethodName(const std::string &methodName)
{
    std::string fullName = getFullMethodName(methodName);
    size_t last_dot = fullName.find_last_of('.');
    if (last_dot != std::string::npos) {
        fullName[last_dot] = '/';
    }
    fullName.insert(fullName.begin(), '/');
    return fullName;
}

std::string ProtoAdapter::getMessageNameFromMethod(const std::string &methodName, bool isRequest)
{
    std::string fullName = getFullMethodName(methodName);
    const DescriptorPool *pool = m_importer->pool();

    const auto *method = pool->FindMethodByName(fullName);

    if (!method)
    {
        logError("Method not found!");
        return "";
    }
    
    return isRequest ? method->input_type()->full_name() : method->output_type()->full_name();
    
}

// int ProtoAdapter::validateArguments(const std::string &methodName, const Value *arg)
// {
//     std::string messageName = getMessageNameFromMethod(methodName, true);

//     if (!validateArgumentCount(messageName, arg))
//         return 1; //Add Error Message

//     if (!validateArgumentTypes(messageName, arg))
//         return 2; //Add Error Message

//     return -1;
// }


// bool ProtoAdapter::validateArgumentCount(const std::string &messageName, const Value *arg)
// {
//     int counter = 0;
//     ObjectInstance *object = asObjectInstance(*arg);
//     std::string fullName = getFullMessageName(messageName);
//     return (minrequiredFieldCount(fullName, counter) <= object->properties.size());
// }

// bool ProtoAdapter::validateArgumentTypes(const std::string &messageName, const Value *arg)
// {
//     std::string fullName = getFullMessageName(messageName);
//     auto *msg = m_importer->pool()->FindMessageTypeByName(messageName);

//     for (int i = 0; i < msg->field_count(); i++)
//     {
//         auto *field = msg->field(i);
//         if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
//             return validateArgumentTypes(field->message_type()->full_name(), arg);       


//         else if (m_stringConversion[field->type_name()] != arg[i].type())
//             return false;
//     }

//     return true;
// }


grpc_slice ProtoAdapter::generateProtocRequestByMethod(const std::string &methodName,  const Value *arg)
{
    std::string msg = getMessageNameFromMethod(methodName, true);
    return generateProtocRequest(msg, arg);
}


grpc_slice ProtoAdapter::generateProtocRequest(const std::string &messageName, const Value *arg)
{
    std::string fullMsg = getFullMessageName(messageName);
    auto *desc = m_importer->pool()->FindMessageTypeByName(fullMsg);
    auto *msg = m_dynfactory->GetPrototype(desc)->New();
    unsigned int index = 0;
    ObjectInstance *instance = asObjectInstance(*arg);
    int reqSize = instance->properties.size();

    //TODO: Consider repeated types


    while (index < reqSize)
    {
        auto *field = desc->field(index);
        Value v = instance->properties[toUnicodeString(field->name()).hashCode()];

        switch (field->cpp_type())
        {
            case FieldDescriptor::CPPTYPE_BOOL:
                msg->GetReflection()->SetBool(msg, field, v.asBool());
            break;

            case FieldDescriptor::CPPTYPE_DOUBLE:
                msg->GetReflection()->SetDouble(msg, field, v.asReal());
            break;

            case FieldDescriptor::CPPTYPE_ENUM:
                msg->GetReflection()->SetEnumValue(msg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_FLOAT:
               msg->GetReflection()->SetFloat(msg, field, v.asReal());
            break;

            case FieldDescriptor::CPPTYPE_INT32:
                msg->GetReflection()->SetInt32(msg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_INT64:
                msg->GetReflection()->SetInt32(msg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_UINT32:
                msg->GetReflection()->SetInt32(msg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_UINT64:
                msg->GetReflection()->SetInt32(msg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_MESSAGE:
            {
                auto *innerMsg = m_dynfactory->GetPrototype(field->message_type())->New();
                generateProtocSubRequest(field->message_type()->full_name(), arg, innerMsg, index);
                msg->GetReflection()->SetAllocatedMessage(msg, innerMsg, field);
            }
            break;

            default:
                throw("Unimplemented/Unsupported data type");
            break;
        }

        index++;
    }

    size_t size = msg->ByteSizeLong();
    uint8_t *buffer = new uint8_t[size];

    if(!msg->SerializeToArray(buffer, size))
        throw("Unable to serialize message");

    return grpc_slice_from_static_buffer(buffer, size);
}

void ProtoAdapter::generateProtocSubRequest(const std::string &messageName, const Value *arg, Message *innerMsg, unsigned int &index)
{
    auto *desc = m_importer->pool()->FindMessageTypeByName(messageName);
    std::string request;
    ObjectInstance *instance = asObjectInstance(*arg);

    for (int i = 0; i < desc->field_count(); i++)
    {
        auto *field = desc->field(i);
        Value v = instance->properties[index];

        switch (field->cpp_type())
        {
            case FieldDescriptor::CPPTYPE_BOOL:
                innerMsg->GetReflection()->SetBool(innerMsg, field, v.asBool());
            break;

            case FieldDescriptor::CPPTYPE_DOUBLE:
                innerMsg->GetReflection()->SetDouble(innerMsg, field, v.asReal());
            break;

            case FieldDescriptor::CPPTYPE_ENUM:
                innerMsg->GetReflection()->SetEnumValue(innerMsg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_FLOAT:
               innerMsg->GetReflection()->SetFloat(innerMsg, field, v.asReal());
            break;

            case FieldDescriptor::CPPTYPE_INT32:
                innerMsg->GetReflection()->SetInt32(innerMsg, field, v.asInt());
            break;
            case FieldDescriptor::CPPTYPE_INT64:
                innerMsg->GetReflection()->SetInt32(innerMsg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_UINT32:
                innerMsg->GetReflection()->SetInt32(innerMsg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_UINT64:
                innerMsg->GetReflection()->SetInt32(innerMsg, field, v.asInt());
            break;

            case FieldDescriptor::CPPTYPE_MESSAGE:
                auto *anotherMsg = m_dynfactory->GetPrototype(field->message_type())->New();
                generateProtocSubRequest(field->message_type()->full_name(), arg, anotherMsg, index);
                innerMsg->GetReflection()->SetAllocatedMessage(innerMsg, anotherMsg, field);
            break;
        }
        index++;
    }

}



Value ProtoAdapter::generateRoxalResponse(const std::string &methodName, const grpc_slice &response)
{
    std::string message = getMessageNameFromMethod(methodName, false);
    auto *desc = m_importer->pool()->FindMessageTypeByName(message);
    auto *msg = m_dynfactory->GetPrototype(desc)->New();
    ObjObjectType *type = buildObjectDeclaration(message);
    ObjectInstance *instance = objectInstanceVal(type);


    
    //Note: Need to divide by two to eliminate extra characters added in the message
    if(!msg->ParseFromArray(response.data.inlined.bytes, response.data.inlined.length))
    {
        std::cout << "Could not parse response" << std::endl;
        return nilVal();
    }
    

    auto *refl = msg->GetReflection();

    for (int i = 0; i < desc->field_count(); i++)
    {
        auto *field = desc->field(i);
        int32_t hash = toUnicodeString(field->name()).hashCode();
        
        switch (field->cpp_type())
        {
            case FieldDescriptor::CPPTYPE_INT32:
                instance->properties[hash] = intVal(refl->GetInt32(*msg, field));
                //std::cout << v << std::endl;
            break;

            case FieldDescriptor::CPPTYPE_INT64:
               instance->properties[hash] = intVal(refl->GetInt64(*msg, field)); //Use 32 bit integers for now
            break;

            case FieldDescriptor::CPPTYPE_UINT64:
                instance->properties[hash] = intVal(refl->GetUInt64(*msg, field));
            break;

            case FieldDescriptor::CPPTYPE_UINT32:
                instance->properties[hash] = intVal(refl->GetUInt32(*msg, field));
            break;

            case FieldDescriptor::CPPTYPE_DOUBLE:
                instance->properties[hash] = realVal(refl->GetDouble(*msg, field));
            break;

            case FieldDescriptor::CPPTYPE_FLOAT:
                instance->properties[hash] = realVal(refl->GetFloat(*msg, field));
            break;

            case FieldDescriptor::CPPTYPE_BOOL:
                instance->properties[hash] = boolVal(refl->GetBool(*msg, field));
            break;

            case FieldDescriptor::CPPTYPE_ENUM:
                instance->properties[hash] = intVal(refl->GetEnumValue(*msg, field));
            break;

            //TODO: Revisit later (nested messages)
            // case FieldDescriptor::CPPTYPE_MESSAGE:
            //     instance->properties.emplace(i, Value(refl->GetMessage(*msg, field)));
            // break;

            default:
                std::cout << "Undefined type" << std::endl;
                instance->properties[hash] = nilVal();
            break;
        }
    }    

    return Value(instance);
}

// ---- Auxiliary Methods ---- //


bool ProtoAdapter::nameMatch(const std::string &fullName, const std::string &name)
{
    if (name.size() > fullName.size())
        return false;

    return fullName.compare(fullName.size() - name.size(),
                           name.size(), name) == 0;
}


std::vector<std::string> ProtoAdapter::addServices(const std::string &protoFile)
{

    auto *file_desc = m_importer->Import(protoFile);
    std::vector<std::string> methods;

    for (int i = 0; i < file_desc->service_count(); i++)
    {
        auto *service = file_desc->service(i);
        m_serviceList.push_back(service);

        for (int j = 0; j < service->method_count(); j++)
        {    
            methods.push_back(service->method(j)->name());
        }
    }

    return methods; 
}



int ProtoAdapter::minrequiredFieldCount(const std::string &messageName, int &count)
{
    const auto *desc = m_importer->pool()->FindMessageTypeByName(messageName);

    for (int i = 0; i < desc->field_count(); i++)
    {
        auto *field = desc->field(i);
        if (field->is_required())
        {
            if(field->containing_type())
                count = minrequiredFieldCount(field->full_name(), count);
            else
                count++;
        }
        
        else if (field->containing_oneof())
            count++;

    }
    
    return count;
}

ObjObjectType* ProtoAdapter::buildObjectDeclaration(const std::string &messageName)
{
    auto *desc = m_importer->pool()->FindMessageTypeByName(messageName);
    ObjObjectType *obj = objectTypeVal(toUnicodeString(desc->name()), false);

    for (int i = 0; i < desc->field_count(); i++)
    {
        auto *field = desc->field(i);
        Value def;
        
        switch(field->cpp_type())
        {
            case FieldDescriptor::CPPTYPE_BOOL:
                def = defaultValue(ValueType::Bool);
            break;

            case FieldDescriptor::CPPTYPE_DOUBLE:
            case FieldDescriptor::CPPTYPE_FLOAT:
                def = defaultValue(ValueType::Real);
            break;

            case FieldDescriptor::CPPTYPE_UINT32:
            case FieldDescriptor::CPPTYPE_UINT64:
            case FieldDescriptor::CPPTYPE_INT32:
            case FieldDescriptor::CPPTYPE_INT64:
            case FieldDescriptor::CPPTYPE_ENUM:
                def = defaultValue(ValueType::Int);
            break;

            case FieldDescriptor::CPPTYPE_MESSAGE:
                def = defaultValue(ValueType::Object);
            break;

            case FieldDescriptor::CPPTYPE_STRING:
                def = defaultValue(ValueType::String);
            break;

            default:
                def = defaultValue(ValueType::Nil);
            break;

        }

        obj->properties.emplace(toUnicodeString(field->name()).hashCode(), std::make_tuple(toUnicodeString(field->name()), Value(def.type()), def));
    }

    return obj;
}


std::vector<ObjObjectType*> ProtoAdapter::allocateObjects(const std::string &protoFile)
{
    auto *file_desc = m_importer->Import(protoFile);
    std::vector<ObjObjectType*> objects; 

    for (int i = 0; i < file_desc->message_type_count(); i++)
        objects.push_back(buildObjectDeclaration(file_desc->message_type(i)->full_name()));

    return objects;
}



void ProtoAdapter::logError(const std::string &errormsg)
{
    std::cerr << errormsg << std::endl;
}

