#include "ClientCall.h"

void* tag(intptr_t t)
{
    return reinterpret_cast<void*>(t);
}

ClientCall::ClientCall()
{}

ClientCall::ClientCall(std::shared_ptr<Channel> channel)
{
    m_stub = std::unique_ptr<GenericStub>(new GenericStub(channel));
}

ClientCall::~ClientCall()
{
    delete m_clicontext;
}



void ClientCall::InitializeCall(const std::string &methodName, OutgoingMetaData *metadata)
{
    m_clicontext = new ClientContext(); //Create a new Client Context

    if (metadata)
    {
        for (OutgoingMetaData::const_iterator iter = metadata->begin(); iter != metadata->end(); iter++)
            m_clicontext->AddMetadata(iter->first, iter->second);
    }

    m_communicator = m_stub->PrepareCall(m_clicontext, methodName, &m_cliqueue);
    m_communicator->StartCall(tag(1));

    void* got_tag;
    bool ok;

    m_cliqueue.Next(&got_tag, &ok);
    GPR_ASSERT(ok);
       
}

void ClientCall::Write(const grpc_slice &request)
{
    void *got_tag;
    bool ok;

    Slice slice(request, Slice::STEAL_REF);
    ByteBuffer buff(&slice, 1);

    m_communicator->Write(buff, tag(2));
    m_cliqueue.Next(&got_tag, &ok);
    GPR_ASSERT(ok);
    

}

void ClientCall::ReadResponse(grpc_slice &response, IncomingMetaData *server_meta)
{
    void *got_tag;
    bool ok;

    ByteBuffer recv_buffer;
    m_communicator->Read(&recv_buffer, tag(3));

    m_cliqueue.Next(&got_tag, &ok);
    GPR_ASSERT(ok);
    
    
    Slice slice;
    recv_buffer.DumpToSingleSlice(&slice);

    response = slice.c_slice();

    if (server_meta)
        *server_meta = m_clicontext->GetServerInitialMetadata();
    

}

void ClientCall::WritesDone()
{
    void *got_tag;
    bool ok;

    m_communicator->WritesDone(tag(4));
    m_cliqueue.Next(&got_tag, &ok);
    GPR_ASSERT(ok);

}

Status ClientCall::Finish(IncomingMetaData *servertrailing)
{
    void *got_tag;
    bool ok;
    Status status;

    m_communicator->Finish(&status, tag(5));
    m_cliqueue.Next(&got_tag, &ok);
    GPR_ASSERT(ok);

    if (servertrailing)
        *servertrailing = m_clicontext->GetServerTrailingMetadata();

    return status;

}