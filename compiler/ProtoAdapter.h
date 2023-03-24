#pragma once
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/message.h>


#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>

#include "Value.h"
#include "Object.h"

//TODO: Move Error Handling to Separate Class

using namespace google::protobuf;
using namespace google::protobuf::compiler;

class ErrorPrinter;

namespace roxal {

    class ProtoAdapter
    {
        public:
            //Constructors
            ProtoAdapter(const std::string &proto_path, const std::vector<std::string> &protoFiles);
            ~ProtoAdapter();

            // --- RPC Methods --- //
            std::string getFullMethodName(const std::string &methodName);
            std::string getFullMessageName(const std::string &messageName);
            std::string getFullEnumName(const std::string &enumName);
            std::string getFormattedMethodName(const std::string &methodName);
            std::unordered_map<std::string, std::string> getMessagePrimitives(const std::string &messageName);
            std::unordered_map<std::string, std::string> getMessagePrimitivesFromMethod(const std::string &methodName, bool isRequest);
            std::string getMessageNameFromMethod(const std::string &methodName, bool isRequest);
            std::string serializeProtoFromMessage(const std::string &messageName, const std::string &message);
            std::string serializeProtoFromMethod(const std::string &methodName, const std::string &message, bool isRequest);
            std::string deserializeProtoFromMessage(const std::string &messageName, const std::string &serial);
            std::string deserializeProtoFromMethod(const std::string &methodName, const std::string &serial, bool isRequest);

            // --- Roxal and Protobuf Type Conversions --- //
            std::string generateProtocRequestByMethod(const std::string &methodName, const Value *arg);
            std::string generateProtocRequest(const std::string &messageName, const ObjectInstance *arg);
            std::string generateProtocSubRequest(const std::string &messageName, const ObjectInstance *arg, unsigned int &index);
            Value convertToRoxal(ValueType type, const std::string &value);
            Value generateRoxalResponse(const std::string &methodName, const std::string &response);
            bool validateArguments(const std::string &methodName, const Value *arg);
            bool validateArgumentCount(const std::string &messageName, const Value *arg);
            bool validateArgumentTypes(const std::string &messageName, const Value *arg);

            // --- Auxiliary Methods --- //
            std::vector<std::string> methodList();
            void initializeServiceList(const std::vector<std::string> &protoFiles);
            void initializeMessageList(const std::vector<std::string> &protoFiles);
            void initializeConversionTables();
            std::string findProtobufType(ValueType type);
            int minrequiredFieldCount(const std::string &messageName, int &count);
            void addInnerMessages(const Descriptor *outerMessage);
            std::vector<std::string> getValues(const std::string &messageName, const std::string &message);
            bool nameMatch(const std::string &fullName, const std::string &name);
            void logError(const std::string &errormsg);
        
        private:
            DiskSourceTree m_sourceTree;
            std::unique_ptr<ErrorPrinter> m_errorPrinter;
            std::unique_ptr<Importer> m_importer;
            std::unique_ptr<DynamicMessageFactory> m_dynfactory;
            std::vector<const ServiceDescriptor*> m_serviceList;
            std::vector<const Descriptor*> m_messageList;
            //std::multimap<ValueType, protocValueType> m_conversionTable;
            std::map<std::string, ValueType> m_stringConversion;

    };
}

