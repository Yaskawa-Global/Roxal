// The vendored Triton protocol messages (third_party/triton): their field
// numbers ARE the wire format a Triton server parses, so each one ai.nn uses
// is pinned to Triton's grpc_service.proto here, and an inference request
// survives a serialize/parse round trip.  Then the inference messages as
// ai.nn writes and reads them, with the tensor bytes outside protobuf
// (TritonInfer): what protobuf parses from them, and that an input is sent
// from its own memory, uncopied.

#include "TritonInfer.h"
#include "triton_inference.pb.h"

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

void expectField(const google::protobuf::Descriptor* message, const char* field, int number)
{
    const auto* f = message->FindFieldByName(field);
    check(f != nullptr, std::string(message->name()) + "." + field + " exists");
    if (f)
        check(f->number() == number,
              std::string(message->name()) + "." + field + " is field " + std::to_string(number));
}

std::string flatten(grpc::ByteBuffer& buffer)
{
    std::vector<grpc::Slice> slices;
    buffer.Dump(&slices);
    std::string bytes;
    for (const auto& slice : slices)
        bytes.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
    return bytes;
}

// `bytes` as gRPC might deliver them: in pieces of `piece` bytes, so fields
// straddle pieces.
grpc::ByteBuffer inPieces(const std::string& bytes, size_t piece)
{
    std::vector<grpc::Slice> slices;
    for (size_t at = 0; at < bytes.size(); at += piece)
        slices.emplace_back(bytes.data() + at, std::min(piece, bytes.size() - at));
    return grpc::ByteBuffer(slices.data(), slices.size());
}

std::string pattern(size_t size, int seed)
{
    std::string bytes(size, '\0');
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<char>((i * 31 + seed) & 0xff);
    return bytes;
}

void checkEncode()
{
    using namespace roxal::triton;
    ModelInferRequest header;
    header.set_model_name("dfine");
    for (const char* name : {"a", "empty", "big"}) {
        auto* in = header.add_inputs();
        in->set_name(name);
        in->set_datatype("UINT8");
    }
    header.add_outputs()->set_name("logits");
    (*header.mutable_parameters())["timeout"].set_int64_param(200000);

    const std::vector<std::string> data { pattern(8, 1), "", pattern(300000, 2) };
    bool bigReleased = false;
    auto big = std::shared_ptr<const void>(data[2].data(), [&](const void*) { bigReleased = true; });
    std::vector<roxal::nn::InferInputBytes> inputs {
        { std::shared_ptr<const void>(data[0].data(), [](const void*) {}), data[0].size() },
        { nullptr, 0 },
        { big, data[2].size() },
    };
    big.reset();
    {
        grpc::ByteBuffer wire = roxal::nn::encodeInferRequest(header, std::move(inputs));

        std::vector<grpc::Slice> slices;
        wire.Dump(&slices);
        bool sentInPlace = false;
        for (const auto& slice : slices)
            sentInPlace |= slice.begin() == reinterpret_cast<const uint8_t*>(data[2].data());
        check(sentInPlace, "an input is sent from its own memory, not a copy");
        slices.clear();
        check(!bigReleased, "an input's memory is held while the request lives");

        ModelInferRequest parsed;
        check(parsed.ParseFromString(flatten(wire)), "an encoded request parses");
        check(parsed.model_name() == "dfine" && parsed.inputs_size() == 3
              && parsed.inputs(2).name() == "big" && parsed.outputs_size() == 1
              && parsed.parameters().at("timeout").int64_param() == 200000,
              "an encoded request keeps its header");
        check(parsed.raw_input_contents_size() == 3, "every input's bytes are sent");
        if (parsed.raw_input_contents_size() == 3)
            for (int i = 0; i < 3; ++i)
                check(parsed.raw_input_contents(i) == data[i],
                      "input " + std::to_string(i) + "'s bytes arrive whole and in order");
    }
    check(bigReleased, "an input's memory is let go with the request");
}

void checkDecode()
{
    using namespace roxal::triton;
    const std::vector<std::string> data { pattern(1200, 3), "", pattern(250000, 4) };

    ModelInferResponse meta;
    meta.set_model_name("dfine");
    meta.set_model_version("1");
    for (const char* name : {"pred_boxes", "none", "logits"}) {
        auto* out = meta.add_outputs();
        out->set_name(name);
        out->set_datatype("FP32");
        out->add_shape(1);
    }
    (*meta.mutable_parameters())["sequence_end"].set_bool_param(true);
    ModelInferResponse raw;
    for (const auto& bytes : data)
        raw.add_raw_output_contents(bytes);

    // As a server writes it (fields in number order); with the contents
    // first, as a parser must also take them; and with fields of every other
    // wire type -- a newer server's -- which pass through to protobuf.
    ModelInferResponse whole = meta;
    for (const auto& bytes : data)
        whole.add_raw_output_contents(bytes);
    const std::string newer = std::string("\x98\x06\xac\x02", 4)                    // 99: varint 300
                            + std::string("\x91\x06", 2) + std::string(8, '\x05')     // 98: fixed64
                            + std::string("\x8d\x06", 2) + std::string(4, '\x07');    // 97: fixed32
    const std::pair<const char*, std::string> forms[] = {
        { "in field order", whole.SerializeAsString() },
        { "contents first", raw.SerializeAsString() + meta.SerializeAsString() },
        { "with newer fields", whole.SerializeAsString() + newer },
    };
    for (const auto& [form, bytes] : forms) {
        grpc::ByteBuffer reply = inPieces(bytes, 7);
        ModelInferResponse response;
        std::vector<roxal::nn::InferOutputBytes> contents;
        const std::string what = std::string("a reply ") + form;
        check(roxal::nn::decodeInferResponse(reply, response, contents), what + " decodes");
        check(response.model_name() == "dfine" && response.model_version() == "1"
              && response.outputs_size() == 3 && response.outputs(2).name() == "logits"
              && response.parameters().at("sequence_end").bool_param(),
              what + ": every other field parses");
        check(response.raw_output_contents_size() == 0, what + ": contents are not parsed twice");
        const auto& unknown = response.GetReflection()->GetUnknownFields(response);
        if (bytes.size() > whole.ByteSizeLong())
            check(unknown.field_count() == 3 && unknown.field(0).varint() == 300
                  && unknown.field(1).fixed64() == 0x0505050505050505ull
                  && unknown.field(2).fixed32() == 0x07070707u,
                  what + ": they reach protobuf intact");
        check(contents.size() == 3, what + ": every output's contents");
        for (size_t i = 0; i < contents.size() && i < data.size(); ++i) {
            check(contents[i].size == data[i].size()
                  && std::string(static_cast<const char*>(contents[i].block.get()), contents[i].size) == data[i],
                  what + ": output " + std::to_string(i) + "'s bytes");
            check(reinterpret_cast<uintptr_t>(contents[i].block.get()) % 64 == 0,
                  what + ": output " + std::to_string(i) + "'s block is 64-byte aligned");
        }
    }

    const std::string bytes = whole.SerializeAsString();
    grpc::ByteBuffer truncated = inPieces(bytes.substr(0, bytes.size() - 100), 4096);
    ModelInferResponse response;
    std::vector<roxal::nn::InferOutputBytes> contents;
    check(!roxal::nn::decodeInferResponse(truncated, response, contents), "a truncated reply is refused");

    grpc::ByteBuffer empty;
    check(roxal::nn::decodeInferResponse(empty, response, contents)
          && response.outputs_size() == 0 && contents.empty(), "an empty reply is every field's default");
}

} // namespace

int main()
{
    using namespace roxal::triton;

    const auto* infer = ModelInferRequest::descriptor();
    expectField(infer, "model_name", 1);
    expectField(infer, "model_version", 2);
    expectField(infer, "parameters", 4);
    expectField(infer, "inputs", 5);
    expectField(infer, "outputs", 6);
    expectField(infer, "raw_input_contents", 7);
    const auto* input = ModelInferRequest::InferInputTensor::descriptor();
    expectField(input, "name", 1);
    expectField(input, "datatype", 2);
    expectField(input, "shape", 3);
    expectField(ModelInferRequest::InferRequestedOutputTensor::descriptor(), "name", 1);

    const auto* reply = ModelInferResponse::descriptor();
    expectField(reply, "outputs", 5);
    expectField(reply, "raw_output_contents", 6);
    const auto* output = ModelInferResponse::InferOutputTensor::descriptor();
    expectField(output, "name", 1);
    expectField(output, "datatype", 2);
    expectField(output, "shape", 3);

    const auto* meta = ModelMetadataResponse::descriptor();
    expectField(meta, "inputs", 4);
    expectField(meta, "outputs", 5);
    expectField(ModelMetadataResponse::TensorMetadata::descriptor(), "shape", 3);
    const auto* load = RepositoryModelLoadRequest::descriptor();
    expectField(load, "repository_name", 1);
    expectField(load, "model_name", 2);
    expectField(load, "parameters", 3);
    expectField(RepositoryModelUnloadRequest::descriptor(), "model_name", 2);
    const auto* repoParam = ModelRepositoryParameter::descriptor();
    expectField(repoParam, "string_param", 3);
    expectField(repoParam, "bytes_param", 4);
    expectField(RepositoryIndexRequest::descriptor(), "ready", 2);
    expectField(RepositoryIndexResponse::descriptor(), "models", 1);
    const auto* index = RepositoryIndexResponse::ModelIndex::descriptor();
    expectField(index, "name", 1);
    expectField(index, "state", 3);
    expectField(index, "reason", 4);
    const auto* server = ServerMetadataResponse::descriptor();
    expectField(server, "name", 1);
    expectField(server, "version", 2);
    expectField(server, "extensions", 3);
    expectField(ModelReadyRequest::descriptor(), "version", 2);
    expectField(ModelReadyResponse::descriptor(), "ready", 1);
    expectField(InferParameter::descriptor(), "int64_param", 2);

    // A request as the backend builds one: raw bytes per input, a deadline
    // for the server, the outputs wanted by name.
    ModelInferRequest request;
    request.set_model_name("dfine");
    auto* in = request.add_inputs();
    in->set_name("pixel_values");
    in->set_datatype("FP32");
    in->add_shape(1);
    in->add_shape(3);
    const std::string bytes("\x00\x00\x80\x3f\x00\x00\x00\x40", 8);   // 1.0f, 2.0f
    request.add_raw_input_contents(bytes);
    request.add_outputs()->set_name("logits");
    (*request.mutable_parameters())["timeout"].set_int64_param(200000);

    ModelInferRequest parsed;
    check(parsed.ParseFromString(request.SerializeAsString()), "request parses back");
    check(parsed.model_name() == "dfine", "model name round-trips");
    check(parsed.inputs_size() == 1 && parsed.inputs(0).shape_size() == 2
          && parsed.inputs(0).shape(1) == 3, "input shape round-trips");
    check(parsed.raw_input_contents_size() == 1 && parsed.raw_input_contents(0) == bytes,
          "raw input bytes round-trip");
    check(parsed.outputs_size() == 1 && parsed.outputs(0).name() == "logits",
          "requested output round-trips");
    check(parsed.parameters().at("timeout").int64_param() == 200000, "timeout parameter round-trips");

    checkEncode();
    checkDecode();

    if (failures == 0)
        std::cout << "triton proto: ok\n";
    return failures == 0 ? 0 : 1;
}
