#include "Connector.h"

// --- Handler Class --- //
void Handler::setBufferSize(unsigned int size)
{
    m_bufferSize = size;
}

unsigned int Handler::bufferSize()
{
    return m_bufferSize;
}

void Handler::setMethodName(std::string &methodName)
{
    m_methodName = methodName;
}

void Handler::setRefObject(Value *refObject)
{
    m_refObject = refObject;
}

void Handler::wait()
{
    int i = 0;
    while (m_pollFlag > 0)
    {
        i++;
    }
}

void Handler::setPollFlag(unsigned int flag)
{
    m_pollFlag = flag;
}

void Handler::donePolling()
{
    m_pollFlag = 0;
}


void Handler::printValue(Value &v)
{
    ObjectInstance *obj = asObjectInstance(v);
    for (int i = 0; i < obj->properties.size(); i++)
        std::cout << obj->properties[i] << std::endl;
}

// --- ACUCommunicator Class --- //

ACUCommunicator::ACUCommunicator(std::shared_ptr<Channel> channel)
{
    m_caller = std::unique_ptr<ClientCall>(new ClientCall(channel));
}

void ACUCommunicator::setHandlers(Handler *input, Handler *output)
{
    m_input = input;
    m_output = output;
}

void ACUCommunicator::setInputHandler(Handler *input)
{
    m_input = input;
}

void ACUCommunicator::setOutputHandler(Handler *output)
{
    m_output = output;
}

void ACUCommunicator::call(std::string &methodName)
{
    m_caller->InitializeCall(methodName);
    std::string request;
    std::string response;

    for (int i = 0; i < m_input->bufferSize(); i++)
    {
        m_input->buffer(request);
        m_caller->Write(request);
    }

    m_caller->WritesDone();

    for (int i = 0; i < m_output->bufferSize(); i++)
    {
        m_caller->ReadResponse(response);
        m_output->buffer(response);
    }

    notify(m_caller->Finish(nullptr));
}

void ACUCommunicator::notify(Status st)
{
    //Add error handling based on status later

    switch(st.error_code())
    {
        case StatusCode::OK:
            m_output->update();
            m_input->update();
            break;

        default:
            std::cout << "Unsupported case" << std::endl;
            break;
    }
    
}

// --- Input Handler Class --- //
InputHandler::InputHandler(ProtoAdapter *adapter, Connector *communion) 
{
    m_adapter = adapter;
    m_manager = communion;
}

void InputHandler::buffer(std::string &request)
{
    Value v = m_messageBuffer.front();
    request = m_adapter->generateProtocRequestByMethod(m_methodName, &v);
    m_messageBuffer.pop();
}

void InputHandler::update()
{
    // switch(m_refObject->asObj()->type)
    // {
    //     case ObjType::Stream:
    //         return poll();
    //         break;

    //     default:
    //         Value v = generateRandomObject();
    //         setRefObject(&v); //Temporary Code
    //         break;
    // }

    poll();
}

void InputHandler::poll()
{

    /*Temp Code (Dummy Stream)*/
    m_pollFlag = 1;
    wait();
    notify();

}

void InputHandler::notify()
{
    m_manager->call(m_methodName);
}


Value InputHandler::generateRandomObject()
{
    ObjObjectType *type = objectTypeVal(toUnicodeString("Request"), false);
    ObjectInstance* instance = objectInstanceVal(type);
    // instance->properties.emplace(0, std::rand()%10 + 1);
    // instance->properties.emplace(0, std::rand()%10 + 1);
    instance->properties.emplace(0, 1.43);
    instance->properties.emplace(0, -3.85);

    return Value(instance);
}

void InputHandler::stream()
{
    while (true)
    {
        Value v = generateRandomObject();

        if (m_pollFlag == 0)
            printValue(v);

        else
        {
            m_messageBuffer.push(v);
            std::cout << "Polled request" << std::endl;
            if (m_messageBuffer.size() == m_bufferSize)
                donePolling();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));       
    }
}


// --- Output Handler Class --- //
OutputHandler::OutputHandler(ProtoAdapter *adapter, Connector *master)
{
    m_adapter = adapter;
    m_manager = master;
}

void OutputHandler::buffer(std::string &response)
{
    Value v =m_adapter->generateRoxalResponse(m_methodName, response);
    m_messageBuffer.push(v);
}

void OutputHandler::update()
{
    switch (m_refObject->asObj()->type)
    {
    case ObjType::Stream:
        return poll();
        break;
    
    default:
        m_refObject = &m_messageBuffer.front(); //Temporary Code
        break;
    }
}

void OutputHandler::poll()
{
    m_pollFlag = 2;
    wait();
    notify();
}

void OutputHandler::stream()
{
    while (true)
    {
        if (m_pollFlag == 0)
            std::cout << "..." << std::endl;

        else
        {
            Value v = m_messageBuffer.front();
            std::cout << "Response generated: ";
            printValue(v); 
            if (m_messageBuffer.size() == m_bufferSize)
                donePolling();  
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
}

void OutputHandler::notify()
{
    std::cout << "Response buffer empty" << std::endl;
}