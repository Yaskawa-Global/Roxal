#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <mutex>

#include "Value.h"
#include "Object.h"

class ErrorPrinter;

namespace roxal {

    class ProtoAdapter
    {
    public:
        explicit ProtoAdapter(const std::string& proto_path);
        ~ProtoAdapter();

        std::string getFullMethodName(const std::string& methodName) const;
        std::string getFullMessageName(const std::string& messageName) const;
        std::string getFormattedMethodName(const std::string& methodName) const;
        std::string getMessageNameFromMethod(const std::string& methodName, bool isRequest);

            struct ServiceInfo {
                struct Method {
                    std::string name;
                    std::string inputTypeFullName;
                    std::string outputTypeFullName;
                };
                std::string name;
                std::string package;
                std::vector<Method> methods;
            };

            std::vector<ServiceInfo> addServices(const std::string& protoFile);
        std::vector<Value> allocateObjects(const std::string& protoFile);
        std::string packageName() const { return m_lastPackage; }
        Value declForFullName(const std::string& fullName) const;

        std::string generateProtocRequest(const std::string& methodName, const Value& arg);
        void generateProtocSubRequest(google::protobuf::Message* msg, ObjectInstance* instance);
        void buildReqField(google::protobuf::Message* msg, const google::protobuf::FieldDescriptor* field, Value& arg);
        void buildRepeatedReqField(google::protobuf::Message* msg, const google::protobuf::FieldDescriptor* field, Value& arg);

        Value generateRoxalResponse(const std::string& methodName, const std::string& response);
        void generateSubResponse(const google::protobuf::Message& msg, ObjectInstance* instance);
        void buildRespField(const google::protobuf::Message& msg, const google::protobuf::FieldDescriptor* field, VariablesMap::MonitoredValue& roxField);
        void buildRepeatedRespField(const google::protobuf::Message& msg, const google::protobuf::FieldDescriptor* field, VariablesMap::MonitoredValue& objField);

        void addProtoSearchPath(const std::string& path);
        bool nameMatch(const std::string& fullName, const std::string& name) const;

    private:
        google::protobuf::compiler::DiskSourceTree m_sourceTree;
        std::unique_ptr<ErrorPrinter> m_errorPrinter;
        std::unique_ptr<google::protobuf::compiler::Importer> m_importer;
        std::unique_ptr<google::protobuf::DynamicMessageFactory> m_dynfactory;
        std::vector<const google::protobuf::ServiceDescriptor*> m_serviceList;
        std::unordered_map<std::string, Value> m_declByFullName;
        std::unordered_map<std::string, Value> m_declByShortName;
        std::unordered_set<std::string> m_searchPaths;
        std::string m_lastPackage;
        mutable std::recursive_mutex m_mutex;

        std::string canonicalPath(const std::string& path) const;
        void logError(const std::string& errormsg) const;
    };
}

#endif // ROXAL_ENABLE_GRPC
