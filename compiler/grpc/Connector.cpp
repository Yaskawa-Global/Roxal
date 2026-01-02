#ifdef ROXAL_ENABLE_GRPC

#include "Connector.h"
#include "Object.h"
#include "dataflow/Signal.h"
#include "dataflow/DataflowEngine.h"

#include <stdexcept>
#include <future>
#include <grpcpp/support/status_code_enum.h>

using namespace roxal;

GrpcStatusError::GrpcStatusError(const std::string& method,
                                 const grpc::Status& status)
    : std::runtime_error("gRPC call '" + method + "' failed [" +
                         std::string(grpcStatusCodeName(status.error_code())) + "]: " +
                         status.error_message()),
      statusCode(status.error_code()),
      methodName(method),
      statusMessage(status.error_message())
{}

ACUCommunicator::ACUCommunicator(std::shared_ptr<grpc::Channel> channel, ProtoAdapter* adapter)
{
    if (!adapter)
        throw std::invalid_argument("ACUCommunicator requires a ProtoAdapter");
    m_caller = std::make_unique<ClientCall>(channel);
    m_adapter = adapter;
}

ACUCommunicator::~ACUCommunicator()
{
    // Cancel all active streams
    std::lock_guard<std::mutex> lock(m_streamsMutex);
    for (auto& state : m_activeStreams) {
        if (state->handle) {
            m_caller->CancelStream(state->handle);
        }
    }
    m_activeStreams.clear();
}

Value ACUCommunicator::call(const std::string& methodName,
                            ArgsView args,
                            std::optional<std::chrono::milliseconds> timeout)
{
    if (!m_adapter || !m_caller)
        throw std::runtime_error("gRPC connector is not initialized");

    if (args.size() != 1)
        throw std::invalid_argument("gRPC service calls expect a single request object argument");

    std::string request = m_adapter->generateProtocRequest(methodName, args[0]);
    std::string response;

    std::string formattedName = m_adapter->getFormattedMethodName(methodName);
    grpc::Status status = m_caller->Call(formattedName,
                                         request,
                                         response,
                                         nullptr,
                                         nullptr,
                                         timeout);

    if (!status.ok())
        throw GrpcStatusError(methodName, status);

    return m_adapter->generateRoxalResponse(methodName, response);
}

Value ACUCommunicator::callStreaming(const std::string& methodName,
                                     ArgsView args,
                                     bool clientStreaming,
                                     bool serverStreaming,
                                     std::optional<std::chrono::milliseconds> timeout)
{
    if (!m_adapter || !m_caller)
        throw std::runtime_error("gRPC connector is not initialized");

    if (args.size() != 1)
        throw std::invalid_argument("gRPC streaming calls expect a single request object argument");

    // For now, we handle the simple case where args[0] is a request object
    // The signal-based streaming will be integrated through ModuleGrpc
    // which will detect signal arguments and call this method appropriately

    auto state = std::make_shared<ActiveStreamState>();
    state->methodName = methodName;

    // Create output signal for server streaming
    ptr<df::Signal> outputSig;
    if (serverStreaming) {
        // Create an event-driven signal (freq=0) for async message arrival
        outputSig = df::Signal::newSourceSignal(0.0, Value::nilVal(), methodName + "_response");
        outputSig->run();  // Start in running state
        state->outputSignal = Value::signalVal(outputSig);
    }

    // Callback for receiving server messages
    auto onServerMessage = [this, state, outputSig](const std::string& msgData) {
        if (outputSig) {
            // Parse the response and update the signal
            Value parsed = m_adapter->generateRoxalResponse(state->methodName, msgData);
            outputSig->set(parsed);
        }
    };

    // Callback for stream end
    auto onStreamEnd = [this, state, outputSig](const grpc::Status& status) {
        // Mark output signal as not running
        if (outputSig) {
            outputSig->stop();
        }

        // If there's a response promise (client streaming), fulfill it
        if (state->responsePromise) {
            if (status.ok()) {
                // For client streaming, the response comes at the end
                // The actual response data should have been received via onServerMessage
                state->responsePromise->set_value(Value::nilVal());
            } else {
                // Set error - the promise will resolve to nil
                state->responsePromise->set_value(Value::nilVal());
            }
        }

        // Remove from active streams
        {
            std::lock_guard<std::mutex> lock(m_streamsMutex);
            auto it = std::find(m_activeStreams.begin(), m_activeStreams.end(), state);
            if (it != m_activeStreams.end()) {
                m_activeStreams.erase(it);
            }
        }
    };

    // Start the appropriate stream type
    std::string formattedMethod = m_adapter->getFormattedMethodName(methodName);

    if (serverStreaming && !clientStreaming) {
        // Server streaming: send one request, receive stream of responses
        std::string request = m_adapter->generateProtocRequest(methodName, args[0]);
        state->handle = m_caller->StartServerStream(formattedMethod, request,
                                                    onServerMessage, onStreamEnd, timeout);
    } else {
        // Client streaming or bidirectional: start stream, will write later
        state->handle = m_caller->StartStream(formattedMethod, onServerMessage, onStreamEnd, timeout);

        if (state->handle && !clientStreaming) {
            // If not client streaming (shouldn't happen in this branch), send the request
            std::string request = m_adapter->generateProtocRequest(methodName, args[0]);
            m_caller->WriteToStream(state->handle, request);
            m_caller->CloseClientStream(state->handle);
        }
    }

    if (!state->handle) {
        throw std::runtime_error("Failed to start gRPC stream for " + methodName);
    }

    // Store active stream
    {
        std::lock_guard<std::mutex> lock(m_streamsMutex);
        m_activeStreams.push_back(state);
    }

    // Return appropriate value
    if (serverStreaming) {
        return state->outputSignal;
    } else {
        // For client-only streaming, return a future that resolves when stream closes
        state->responsePromise = std::make_shared<std::promise<Value>>();
        auto future = state->responsePromise->get_future().share();
        return Value::futureVal(future);
    }
}

std::string ACUCommunicator::buildRequestFromArgs(const std::string& methodName,
                                                  const std::vector<Value>& args,
                                                  const std::vector<size_t>& signalIndices,
                                                  const std::vector<Value>& signalValues)
{
    // Build current args by replacing signal positions with current values
    std::vector<Value> currentArgs = args;
    for (size_t i = 0; i < signalIndices.size(); ++i) {
        size_t idx = signalIndices[i];
        if (idx < currentArgs.size() && i < signalValues.size()) {
            if (isSignal(signalValues[i])) {
                currentArgs[idx] = asSignal(signalValues[i])->signal->lastValue();
            }
        }
    }

    // Use the first arg as the request
    if (currentArgs.empty()) {
        throw std::runtime_error("No arguments for streaming request");
    }
    return m_adapter->generateProtocRequest(methodName, currentArgs[0]);
}

void ACUCommunicator::onInputSignalChanged(std::shared_ptr<ActiveStreamState> state)
{
    if (!state || !state->handle || state->handle->clientDone || state->handle->cancelled) {
        return;
    }

    // Build and send a new request message
    std::string request = buildRequestFromArgs(state->methodName,
                                               state->frozenArgs,
                                               state->signalArgIndices,
                                               state->inputSignalValues);
    m_caller->WriteToStream(state->handle, request);
}

void ACUCommunicator::onInputSignalStopped(std::shared_ptr<ActiveStreamState> state)
{
    if (!state) return;

    int stopped = ++(state->stoppedSignalCount);
    if (stopped >= state->totalSignalCount) {
        // All input signals stopped - close the client stream
        if (state->handle && !state->handle->clientDone) {
            m_caller->CloseClientStream(state->handle);
        }
    }
}

const char* roxal::grpcStatusCodeName(grpc::StatusCode code)
{
    switch (code) {
        case grpc::StatusCode::OK: return "OK";
        case grpc::StatusCode::CANCELLED: return "CANCELLED";
        case grpc::StatusCode::UNKNOWN: return "UNKNOWN";
        case grpc::StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case grpc::StatusCode::DEADLINE_EXCEEDED: return "DEADLINE_EXCEEDED";
        case grpc::StatusCode::NOT_FOUND: return "NOT_FOUND";
        case grpc::StatusCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
        case grpc::StatusCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case grpc::StatusCode::UNAUTHENTICATED: return "UNAUTHENTICATED";
        case grpc::StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
        case grpc::StatusCode::FAILED_PRECONDITION: return "FAILED_PRECONDITION";
        case grpc::StatusCode::ABORTED: return "ABORTED";
        case grpc::StatusCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case grpc::StatusCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
        case grpc::StatusCode::INTERNAL: return "INTERNAL";
        case grpc::StatusCode::UNAVAILABLE: return "UNAVAILABLE";
        case grpc::StatusCode::DATA_LOSS: return "DATA_LOSS";
        case grpc::StatusCode::DO_NOT_USE: return "DO_NOT_USE";
        default: return "UNKNOWN";
    }
}

#endif // ROXAL_ENABLE_GRPC
