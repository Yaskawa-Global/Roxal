#include "Connector.h"

ACUCommunicator::ACUCommunicator(std::shared_ptr<Channel> channel, ProtoAdapter *adapter)
{
    m_caller = std::unique_ptr<ClientCall>(new ClientCall(channel));
    m_adapter = adapter;
}

Value ACUCommunicator::call(const std::string &methodName, const Value *args)
{
    grpc_slice request = m_adapter->generateProtocRequestByMethod(methodName, args);
    grpc_slice response;
    m_caller->InitializeCall(m_adapter->getFormattedMethodName(methodName)); //IMPORTANT: ALWAYS USE FORMATTED METHOD NAME WHEN INITIALIZING THE SERVICE CALL
    m_caller->Write(request);
    m_caller->WritesDone();
    m_caller->ReadResponse(response);
    m_caller->Finish();

    return (m_adapter->generateRoxalResponse(methodName, response));
}