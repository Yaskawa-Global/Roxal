#include "TritonInfer.h"

#include <google/protobuf/io/coded_stream.h>
#include <grpcpp/support/proto_buffer_reader.h>
#include <grpcpp/support/slice.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <new>
#include <string>

using namespace roxal::nn;

namespace {

// The fields carried outside protobuf (Triton's grpc_service.proto numbers,
// pinned by TritonProto_test).
constexpr uint32_t rawInputContents = 7;    // ModelInferRequest
constexpr uint32_t rawOutputContents = 6;   // ModelInferResponse

constexpr uint32_t wireVarint = 0;
constexpr uint32_t wireFixed64 = 1;
constexpr uint32_t wireLengthDelimited = 2;
constexpr uint32_t wireFixed32 = 5;

void appendVarint(std::string& out, uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<char>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void appendLittleEndian(std::string& out, uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<char>(value >> (8 * i)));
}

void releaseInput(void* data)
{
    delete static_cast<std::shared_ptr<const void>*>(data);
}

std::shared_ptr<void> allocateBlock(size_t size)
{
    // Uninitialised: every byte is about to be written.
    constexpr std::align_val_t alignment { 64 };
    void* block = ::operator new(std::max<size_t>(size, 1), alignment);
    return std::shared_ptr<void>(block, [](void* b) { ::operator delete(b, alignment); });
}

} // namespace

grpc::ByteBuffer roxal::nn::encodeInferRequest(const roxal::triton::ModelInferRequest& header,
                                               std::vector<InferInputBytes> inputs)
{
    // Small pieces -- the header, each input's tag and length -- gather in
    // `pending` and become a slice before each input's bytes.
    std::string pending = header.SerializeAsString();
    std::vector<grpc::Slice> slices;
    slices.reserve(2 * inputs.size() + 1);
    for (auto& input : inputs) {
        appendVarint(pending, (rawInputContents << 3) | wireLengthDelimited);
        appendVarint(pending, input.size);
        if (input.size == 0)
            continue;
        slices.emplace_back(pending);
        pending.clear();
        auto* keep = new std::shared_ptr<const void>(std::move(input.data));
        slices.emplace_back(const_cast<void*>(keep->get()), input.size, &releaseInput, keep);
    }
    if (!pending.empty())
        slices.emplace_back(pending);
    return grpc::ByteBuffer(slices.data(), slices.size());
}

bool roxal::nn::decodeInferResponse(grpc::ByteBuffer& reply, roxal::triton::ModelInferResponse& response,
                                    std::vector<InferOutputBytes>& contents)
{
    response.Clear();
    contents.clear();
    if (reply.Length() == 0)
        return true;    // every field at its default

    grpc::ProtoBufferReader reader(&reply);
    if (!reader.status().ok())
        return false;
    google::protobuf::io::CodedInputStream in(&reader);
    in.SetTotalBytesLimit(INT_MAX);

    // Every field but the outputs' bytes, re-encoded for protobuf to parse.
    std::string rest;
    while (const uint32_t tag = in.ReadTag()) {
        const uint32_t field = tag >> 3;
        const uint32_t wire = tag & 7;
        if (field == rawOutputContents && wire == wireLengthDelimited) {
            uint32_t size = 0;
            if (!in.ReadVarint32(&size) || size > INT_MAX)
                return false;
            InferOutputBytes out { allocateBlock(size), size };
            if (size > 0 && !in.ReadRaw(out.block.get(), static_cast<int>(size)))
                return false;
            contents.push_back(std::move(out));
            continue;
        }
        appendVarint(rest, tag);
        switch (wire) {
            case wireVarint: {
                uint64_t value = 0;
                if (!in.ReadVarint64(&value))
                    return false;
                appendVarint(rest, value);
                break;
            }
            case wireFixed64: {
                uint64_t value = 0;
                if (!in.ReadLittleEndian64(&value))
                    return false;
                appendLittleEndian(rest, value, 8);
                break;
            }
            case wireLengthDelimited: {
                uint32_t size = 0;
                if (!in.ReadVarint32(&size) || size > INT_MAX)
                    return false;
                appendVarint(rest, size);
                const size_t at = rest.size();
                rest.resize(at + size);
                if (size > 0 && !in.ReadRaw(&rest[at], static_cast<int>(size)))
                    return false;
                break;
            }
            case wireFixed32: {
                uint32_t value = 0;
                if (!in.ReadLittleEndian32(&value))
                    return false;
                appendLittleEndian(rest, value, 4);
                break;
            }
            default:
                return false;   // groups: not in proto3
        }
    }
    return in.ConsumedEntireMessage() && response.ParseFromString(rest);
}
