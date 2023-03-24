#pragma once
#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/slice.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/completion_queue.h>


#include <map>
#include <iostream>
#include <string>


using namespace grpc;

//TODO: Add Timeouts

class ClientCall final
{
    typedef std::multimap<std::string, std::string> OutgoingMetaData;
    typedef std::multimap<string_ref, string_ref> IncomingMetaData;

    public:
        ClientCall();
        ClientCall(std::shared_ptr<Channel> channel);
        ~ClientCall();
        void Write(const std::string &request);
        void InitializeCall(const std::string &methodName, OutgoingMetaData *initialMetaData = nullptr);
        void WritesDone();
        void ReadResponse(std::string &response, IncomingMetaData *server_meta = nullptr);
        Status Finish(IncomingMetaData *server_trailing_data);
        

    private:
    std::unique_ptr<GenericStub> m_stub;
    std::unique_ptr<GenericClientAsyncReaderWriter> m_communicator;
    ClientContext* m_clicontext;
    CompletionQueue m_cliqueue;



};