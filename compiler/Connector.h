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

class Handler;
class Connector;

class Connector 
{
    public:
        virtual void notify(Status st) = 0;
        virtual void call(std::string &methodName) = 0;
    

};

class Handler
{
    public:
        virtual void notify() = 0;
        virtual void buffer(std::string &message) = 0;
        virtual void poll() = 0;
        virtual void stream() = 0; //Use temporarily
        virtual void update() = 0;
        unsigned int bufferSize();
        void setBufferSize(unsigned int size);
        void setMethodName(std::string &methodName);
        void printValue(Value &v);

        /*Added temporarily for streaming*/
        void setRefObject(Value *refObject);
        void setPollFlag(unsigned int flag);
        unsigned int pollFlag();
        void wait();
        void donePolling();

    protected:
        Connector *m_manager;
        ProtoAdapter *m_adapter;
        std::queue<Value> m_messageBuffer;
        unsigned int m_bufferSize;
        std::string m_methodName;
        Value *m_refObject;
        unsigned int m_pollFlag = 0;
};

class ACUCommunicator : public Connector
{
    public:
        ACUCommunicator(std::shared_ptr<Channel> channel);
        void notify(Status st);
        void call(std::string &methodName);
        void setHandlers(Handler *input, Handler *output);
        void setInputHandler(Handler *input);
        void setOutputHandler(Handler *output);


    private:
        std::unique_ptr<ClientCall> m_caller;
        Handler *m_input;
        Handler *m_output;
};

class InputHandler : public Handler
{
    public:
        InputHandler(ProtoAdapter *adapter, Connector *master);
        void buffer(std::string &message);
        void poll();
        void stream();
        void update();
        void notify();
        Value generateRandomObject();
        
};

class OutputHandler : public Handler
{
    public:
        OutputHandler(ProtoAdapter *adapter, Connector *master);
        void buffer(std::string &message);
        void poll();
        void update();
        void stream();
        void notify();
};