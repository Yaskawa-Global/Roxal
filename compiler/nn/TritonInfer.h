#pragma once

// ModelInfer messages with the tensor bytes kept out of protobuf, which would
// otherwise copy them on the way out (into the message, then into the
// serialized string, then into gRPC's buffer) and again on the way in.
//
// A request goes on the wire as its header -- every field but
// raw_input_contents -- followed by one raw_input_contents field per input,
// whose bytes are gRPC slices over memory the caller keeps alive: protobuf
// reads a message concatenated from its fields, and a repeated field's
// entries keep their order.  So an input is never copied by the client.
//
// A reply's raw_output_contents are copied once, from gRPC's buffers into a
// block per output; every other field is parsed as usual.

#include "triton_inference.pb.h"

#include <grpcpp/support/byte_buffer.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace roxal::nn {

// One input's bytes.  `data` also keeps them alive and unchanged until gRPC
// has sent them; it is released on whichever thread gRPC lets go on.
struct InferInputBytes {
    std::shared_ptr<const void> data;
    size_t size = 0;
};

// `header` carries no raw_input_contents; `inputs` follow in the order of its
// inputs.
grpc::ByteBuffer encodeInferRequest(const roxal::triton::ModelInferRequest& header,
                                    std::vector<InferInputBytes> inputs);

// One output's bytes, in a block of its own: 64-byte aligned, so a tensor can
// adopt it as is.
struct InferOutputBytes {
    std::shared_ptr<void> block;
    size_t size = 0;
};

// The raw_output_contents into `contents`, in order, and every other field
// into `response`.  False when the reply is malformed.
bool decodeInferResponse(grpc::ByteBuffer& reply, roxal::triton::ModelInferResponse& response,
                         std::vector<InferOutputBytes>& contents);

} // namespace roxal::nn
