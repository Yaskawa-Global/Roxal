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

ProtoAdapter::ProtoAdapter(const std::string &proto_path, const std::vector<std::string> &protoFiles)
{

    //Add the proto path to the source tree
    m_sourceTree.MapPath("", proto_path);

    //Initialize an Error Printer
    m_errorPrinter = std::unique_ptr<ErrorPrinter>(new ErrorPrinter());

    //Make an Importer Object That Will Parse Proto Files
    m_importer = std::unique_ptr<Importer>(new Importer(&m_sourceTree, m_errorPrinter.get()));

    //Create a message factory to create messages on the fly
    m_dynfactory = std::unique_ptr<DynamicMessageFactory>(new DynamicMessageFactory());
 


   initializeServiceList(protoFiles);
   initializeMessageList(protoFiles); 
   initializeConversionTables();

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

std::string ProtoAdapter::getFullEnumName(const std::string &enumName)
{
    for (int i = 0; i < m_serviceList.size(); i++)
    {
        const auto *fileDesc = m_serviceList[i]->file();

        for (int j = 0; j < fileDesc->enum_type_count(); j++)
        {
            const auto *enumDesc = fileDesc->enum_type(j);

            if (nameMatch(enumDesc->full_name(), enumName))
                return enumDesc->full_name();
        }
    }

    logError("Cannot find full enum name");
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


std::string ProtoAdapter::serializeProtoFromMethod(const std::string &methodName, const std::string &message, bool isRequest)
{
    std::string msgName = getMessageNameFromMethod(methodName, isRequest);
    
    if (msgName.empty())
    {
        logError("Could not find a matching message for method. Please check proto files");
        return "";
    }

    return serializeProtoFromMessage(msgName, message);

}

std::string ProtoAdapter::serializeProtoFromMessage(const std::string &messageName, const std::string &message)
{
    std::string serial;
    std::string fullMsg = getFullMessageName(messageName);
    const auto *descPool = m_importer->pool();
    const auto *descriptor = descPool->FindMessageTypeByName(fullMsg);

    if (!descriptor)
    {
        logError("Cannot find message type");
        return "";
    }

    auto *msg = m_dynfactory->GetPrototype(descriptor)->New();

    //Input the message to be serialized
    if (!TextFormat::ParseFromString(message, msg))
    {
        logError("Cannot Parse Message");
        return "";   
    }

    //Serialize the message into the protobuf wire format.
    if (!msg->SerializeToString(&serial))
    {
        logError("Cannot Serialize Message");
        return "";
    }

    return serial;

}


std::string ProtoAdapter::deserializeProtoFromMessage(const std::string &messageName, const std::string &serial)
{
    std::string formattedMessage;
    std::string fullMsg = getFullMessageName(messageName);
    const auto *descPool = m_importer->pool();
    const auto *desc = descPool->FindMessageTypeByName(fullMsg);

    if (!desc)
    {
        logError("Cannot find message type");
        return "";
    }

    auto *msg = m_dynfactory->GetPrototype(desc)->New();

    if(!msg->ParseFromString(serial))
    {
        logError("Cannot Deserialize Message");
        return "";
    }

    if (!TextFormat::PrintToString(*msg, &formattedMessage))
    {
        logError("Cannot format message");
        return "";
    }   
    

    return formattedMessage;


}

std::string ProtoAdapter::deserializeProtoFromMethod(const std::string &methodName, const std::string &serial, bool isRequest)
{
    std::string msgName = getMessageNameFromMethod(methodName, isRequest);

    if (msgName.empty())
    {
        logError("Could not find a matching message for method. Please check proto files");
        return "";
    }

    return deserializeProtoFromMessage(msgName, serial);

}



// ---- Roxal and Protobuf Type Conversions ---- //
void ProtoAdapter::initializeConversionTables()
{
    
    m_stringConversion.insert({"double", ValueType::Real});
    m_stringConversion.insert({"float", ValueType::Decimal});
    m_stringConversion.insert({"int32", ValueType::Int});
    m_stringConversion.insert({"int64", ValueType::Int});
    m_stringConversion.insert({"uint32", ValueType::Int});
    m_stringConversion.insert({"uint64", ValueType::Int});
    m_stringConversion.insert({"sint32", ValueType::Int});
    m_stringConversion.insert({"sint64", ValueType::Int});
    m_stringConversion.insert({"fixed32", ValueType::Int});
    m_stringConversion.insert({"fixed64", ValueType::Int});
    m_stringConversion.insert({"sfixed32", ValueType::Int});
    m_stringConversion.insert({"sfixed64", ValueType::Int});
    m_stringConversion.insert({"enum", ValueType::Int});
    m_stringConversion.insert({"bool", ValueType::Bool});
    m_stringConversion.insert({"string", ValueType::String});
    m_stringConversion.insert({"bytes", ValueType::Byte});
    m_stringConversion.insert({"", ValueType::Nil}); //Empty String Returns Nil Type
}

bool ProtoAdapter::validateArguments(const std::string &methodName, const Value *arg)
{
    std::string messageName = getMessageNameFromMethod(methodName, true);

    if (!validateArgumentCount(messageName, arg))
        return false; //Add Error Message

    if (!validateArgumentTypes(messageName, arg))
        return false; //Add Error Message

    return true;
}


bool ProtoAdapter::validateArgumentCount(const std::string &messageName, const Value *arg)
{
    int counter = 0;
    ObjectInstance *object = asObjectInstance(*arg);
    std::string fullName = getFullMessageName(messageName);
    return (minrequiredFieldCount(fullName, counter) <= object->properties.size());
}

bool ProtoAdapter::validateArgumentTypes(const std::string &messageName, const Value *arg)
{
    std::string fullName = getFullMessageName(messageName);
    auto *msg = m_importer->pool()->FindMessageTypeByName(messageName);

    for (int i = 0; i < msg->field_count(); i++)
    {
        auto *field = msg->field(i);
        if (std::strcmp(field->type_name(), "message") == 0)
            return validateArgumentTypes(field->message_type()->full_name(), arg);       


        else if (m_stringConversion[field->type_name()] != arg[i].type())
            return false;
    }

    return true;
}

Value ProtoAdapter::convertToRoxal(ValueType type, const std::string &value)
{
    Value *v;

    switch(type)
    {
        case ValueType::Int:
            v = new Value(atoi(value.c_str()));
        break;

        case ValueType::Real:
            v = new Value(atof(value.c_str()));
        break;

        case ValueType::Bool:
            if(std::strcmp(value.c_str(), "true"))
                v = new Value(true);
            else
                v = new Value(false);
        break;

        default:
            std::cout << "Unsupported data type" << std::endl;
        break;

    }

    return *v;
}

std::string ProtoAdapter::generateProtocRequestByMethod(const std::string &methodName, const Value *arg)
{
    std::string msg = getMessageNameFromMethod(methodName, true);
    ObjectInstance *object = asObjectInstance(*arg);
    return generateProtocRequest(msg, object);
}


std::string ProtoAdapter::generateProtocRequest(const std::string &messageName, const ObjectInstance *arg)
{
    // auto primitives = getMessagePrimitives(messageName);
    // auto prime = primitives.begin();
    std::string fullMsg = getFullMessageName(messageName);
    const auto *msg = m_importer->pool()->FindMessageTypeByName(fullMsg);
    std::string request;
    unsigned int size = arg->properties.size();
    unsigned int index = 0;

    //Add [] for repeated types. Ex: parameters: [{a: x, b: y, c: z, d: lol, e: loser, f: funny}] - > Repeated Twice

    while (index < size)
    {
        auto *field = msg->field(index);

        if (std::strcmp(field->type_name(), "string") == 0)
            request.append(field->name() + ": " + "'" + toString(arg->properties.at(index)) + "'");

        else if (std::strcmp(field->type_name(), "message") == 0)
        {
            request.append(field->name() + ": ");
            request.append("{");
            request.append(generateProtocSubRequest(field->message_type()->full_name(), arg, index));
            request.append("}");

            if (index < size-1)
                request.append(", ");

            continue;
        }

        else
            request.append(field->name() + ": " + toString(arg->properties.at(index)));
        
        if (index < size-1)
            request.append(", ");

        index++;
    }

    return serializeProtoFromMessage(fullMsg, request);
}

std::string ProtoAdapter::generateProtocSubRequest(const std::string &messageName, const ObjectInstance *arg, unsigned int &index)
{
    auto *msg = m_importer->pool()->FindMessageTypeByName(messageName);
    std::string request;

    //Add [] for repeated types. Ex: parameters: [{a: x, b: y, c: z, d: lol, e: loser, f: funny}] - > Two instances of the parameters field
    //Alternatively, specify as parameters: {a: x, b: y, c: z}, parameters: {d: i, e: j; f: k}

    for (int i = 0; i < msg->field_count(); i++)
    {
        auto *field = msg->field(i);

        if (std::strcmp(field->type_name(), "string") == 0)
            request.append(field->name() + ": " + "'" + toString(arg->properties.at(index)) + "'");

        else if (std::strcmp(field->type_name(), "message") == 0)
        {
            request.append(field->name() + ": ");
            request.append("{");
            request.append(generateProtocSubRequest(field->message_type()->name(), arg, index));
            request.append("} ");
        }

        else
            request.append(field->name() + ": " + toString(arg->properties.at(index)));
        
        if (index < msg->field_count()-1)
            request.append(", ");

        index++;
    }

    return request;
}


Value ProtoAdapter::generateRoxalResponse(const std::string &methodName, const std::string &response)
{
    std::string message = getMessageNameFromMethod(methodName, false);
    auto *msg = m_importer->pool()->FindMessageTypeByName(message);
    std::vector<std::string> fields = getValues(message, response);
    ObjObjectType *type = objectTypeVal(toUnicodeString(msg->name()), false);
    ObjectInstance *instance = objectInstanceVal(type);
    
    for (int i = 0; i < fields.size(); i++)
    {
        Value v = convertToRoxal(m_stringConversion[msg->field(i)->type_name()], fields[i]);
        instance->properties.emplace(i, v);
    }

    return Value(instance);
}




std::unordered_map<std::string, std::string> ProtoAdapter::getMessagePrimitives(const std::string &messageName) 
{
    //Key = Primitive ID
    //Value = Primitive Type (Protobuf)
    std::unordered_map<std::string, std::string> msgPrimitives;
    std::string fullMsgName = getFullMessageName(messageName);

    for (int i = 0; i < m_messageList.size(); i++)
    {
        if (m_messageList[i]->full_name().compare(fullMsgName) == 0)
        {
            for (int j = m_messageList[i]->field_count()-1; j > -1; j--)
            {
                const auto *field_desc = m_messageList[i]->field(j);
                msgPrimitives.emplace(field_desc->name(), field_desc->type_name());
            }
            break;
        }
    }
     
    return msgPrimitives;
}

std::unordered_map<std::string, std::string> ProtoAdapter::getMessagePrimitivesFromMethod(const std::string &methodName, bool isRequest)
{
    std::string message = getMessageNameFromMethod(methodName, isRequest);
    return getMessagePrimitives(message);
}

// ---- Auxiliary Methods ---- //
std::vector <std::string> ProtoAdapter::methodList()
{
    std::vector<std::string> methods;

    for (int i = 0; i < m_serviceList.size(); i++)
    {
        for (int j = 0; j < m_serviceList[i]->method_count(); j++)
        {
            const auto *method = m_serviceList[i]->method(j);
            methods.push_back(method->name());
        }
    }

    return methods;
}

bool ProtoAdapter::nameMatch(const std::string &fullName, const std::string &name)
{
    if (name.size() > fullName.size())
        return false;

    return fullName.compare(fullName.size() - name.size(),
                           name.size(), name) == 0;
}

std::vector<std::string> ProtoAdapter::getValues(const std::string &messageName, const std::string &message)
{
    std::string fullMsg = getFullMessageName(messageName);
    std::vector<std::string> values;
    const auto *descPool = m_importer->pool();
    const auto *desc = descPool->FindMessageTypeByName(fullMsg);
    std::string value;
    auto *msg = m_dynfactory->GetPrototype(desc)->New();
    msg->ParseFromString(message);

    for(int i = 0; i < msg->GetDescriptor()->field_count(); i++)
    {
        const auto *field = msg->GetDescriptor()->field(i);
        TextFormat::PrintFieldValueToString(*msg, field, -1, &value);
        values.push_back(value);
    }

    return values;
    
}

void ProtoAdapter::initializeServiceList(const std::vector<std::string> &protoFiles)
{
    for (int i = 0; i < protoFiles.size(); i++)
    {
        const auto *file_desc = m_importer->Import(protoFiles[i]);

        for (int j = 0; j < file_desc->service_count(); j++)
        {   
            m_serviceList.push_back(file_desc->service(j));
        }

    }
}

void ProtoAdapter::initializeMessageList(const std::vector<std::string> &protoFiles)
{
    const auto *descPool = m_importer->pool();
    
    for (int i = 0; i < protoFiles.size(); i++)
    {
        auto *fileDesc = descPool->FindFileByName(protoFiles[i]);

        for (int j = 0; j < fileDesc->message_type_count(); j++)
        {
            m_messageList.push_back(fileDesc->message_type(j));
            addInnerMessages(fileDesc->message_type(j));
        }

    }
}

void ProtoAdapter::addInnerMessages(const Descriptor *outerMessage)
{
    for (int i = 0; i < outerMessage->field_count(); i++)
    {
        std::string field = outerMessage->field(i)->type_name();
        if (field.compare("message") == 0)
        {
            auto *desc = outerMessage->field(i)->message_type();
            m_messageList.push_back(desc);
            addInnerMessages(desc);
        }
    }
}

// int ProtoAdapter::totalFieldCount(const std::string &messageName, int &count)
// {

//     const auto *desc = m_importer->pool()->FindMessageTypeByName(messageName);

//     for (int i = 0; i < desc->field_count(); i++)
//     {
//         if (std::strcmp(desc->field(i)->type_name(), "message") == 0)
//             return totalFieldCount(desc->field(i)->message_type()->full_name(), count);

//         count++;
//     }
    
//     return count;
// }

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

std::string ProtoAdapter::findProtobufType(ValueType type)
{
    for (auto it : m_stringConversion)
    {
        if (it.second == type)
            return it.first;
    }

    return "";
}


void ProtoAdapter::logError(const std::string &errormsg)
{
    std::cerr << errormsg << std::endl;
}

