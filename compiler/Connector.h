#pragma once
#include <grpcpp/channel.h>
#include <queue>
#include <string>
#include <condition_variable>
#include <thread>
#include <chrono>

#include "Object.h"
#include "Value.h"
#include "ProtoAdapter.h"
#include "ClientCall.h"

using namespace grpc;
using namespace roxal;


class Connector 
{
    public:
        virtual Value call(const std::string &methodName, const Value *args) = 0;
    
    protected:
        std::unique_ptr<ClientCall> m_caller;
        ProtoAdapter *m_adapter;
};


class ACUCommunicator : public Connector
{
    public:
        ACUCommunicator(std::shared_ptr<Channel> channel, ProtoAdapter *adapter);
        Value call(const std::string &methodName, const Value *args);

};