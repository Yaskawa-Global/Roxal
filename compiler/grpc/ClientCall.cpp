#ifdef ROXAL_ENABLE_GRPC

#include "ClientCall.h"
#include <stdexcept>

namespace {
void* tag(intptr_t t)
{
    return reinterpret_cast<void*>(t);
}
}

ClientCall::ClientCall() = default;

ClientCall::ClientCall(std::shared_ptr<grpc::Channel> channel)
{
    m_stub = std::make_unique<grpc::GenericStub>(channel);
}

ClientCall::~ClientCall()
{
}


grpc::Status ClientCall::Call(const std::string& methodName,
                              const std::string& request,
                              std::string& response,
                              OutgoingMetaData* metadata,
                              IncomingMetaData* server_trailing)
{
    grpc::ClientContext ctx;
    if (metadata) {
        for (const auto& entry : *metadata)
            ctx.AddMetadata(entry.first, entry.second);
    }

    grpc_slice raw = grpc::SliceFromCopiedString(request);
    grpc::Slice slice(raw, grpc::Slice::STEAL_REF);
    grpc::ByteBuffer reqBuffer(&slice, 1);

    grpc::ByteBuffer respBuffer;
    grpc::CompletionQueue cq;
    void* tag1 = reinterpret_cast<void*>(1);
    auto respReader = m_stub->PrepareUnaryCall(&ctx, methodName, reqBuffer, &cq);
    respReader->StartCall();
    grpc::Status status;
    respReader->Finish(&respBuffer, &status, tag1);

    void* got_tag = nullptr;
    bool ok = false;
    cq.Next(&got_tag, &ok);
    if (!ok || got_tag != tag1)
        return grpc::Status(grpc::StatusCode::UNKNOWN, "gRPC unary call failed");

    if (status.ok()) {
        std::vector<grpc::Slice> slices;
        respBuffer.Dump(&slices);
        response.clear();
        for (const auto& s : slices)
            response.append(reinterpret_cast<const char*>(s.begin()), s.size());
    }

    if (server_trailing)
        *server_trailing = ctx.GetServerTrailingMetadata();

    return status;
}

#endif // ROXAL_ENABLE_GRPC
