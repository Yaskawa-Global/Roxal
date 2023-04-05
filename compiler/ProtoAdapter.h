#pragma once
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/message.h>
#include <google/protobuf/io/coded_stream.h>
#include <grpc/slice.h>

#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>

#include "Value.h"
#include "Object.h"

//TODO: Move Error Handling to Separate Class

using namespace google::protobuf;
using namespace google::protobuf::io;
using namespace google::protobuf::compiler;

class ErrorPrinter;

namespace roxal {

    class ProtoAdapter
    {
        public:
            //Constructors
            ProtoAdapter(const std::string &proto_path);
            ~ProtoAdapter();

            // --- RPC Methods --- //
            std::string getFullMethodName(const std::string &methodName);
            std::string getFullMessageName(const std::string &messageName);
            std::string getFormattedMethodName(const std::string &methodName);
            std::string getMessageNameFromMethod(const std::string &methodName, bool isRequest);

            // --- Roxal and Protobuf Type Conversions --- //
            grpc_slice generateProtocRequestByMethod(const std::string &methodName, const int &argCount, const Value *arg);
            grpc_slice generateProtocRequest(const std::string &messageName, const int &argCount, const Value *arg);
            void generateProtocSubRequest(const std::string &messageName, const int &argCount, const Value *arg, Message *inner, unsigned int &index);
            Value generateRoxalResponse(const std::string &methodName, const grpc_slice &response);
            // int validateArguments(const std::string &methodName, const Value *arg);
            // bool validateArgumentCount(const std::string &messageName, const Value *arg);
            // bool validateArgumentTypes(const std::string &messageName, const Value *arg);

            // --- Auxiliary Methods --- //
            std::vector<std::string> addService(const std::string &protoFile);
            int minrequiredFieldCount(const std::string &messageName, int &count);
            bool nameMatch(const std::string &fullName, const std::string &name);
            void logError(const std::string &errormsg);
        
        private:
            DiskSourceTree m_sourceTree;
            std::unique_ptr<ErrorPrinter> m_errorPrinter;
            std::unique_ptr<Importer> m_importer;
            std::unique_ptr<DynamicMessageFactory> m_dynfactory;
            std::vector<const ServiceDescriptor*> m_serviceList;
    };
}

