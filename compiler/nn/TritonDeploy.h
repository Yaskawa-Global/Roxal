#pragma once

// Deploying a model to a Triton server from its files, for ai.nn's
// Model(path, server=...): which files a model is, the content-addressed name
// it is deployed under, placing it in a repository directory the server
// shares, and the RepositoryModelLoad request that uploads it instead.  Knows
// nothing of Roxal values.

#include <grpcpp/support/byte_buffer.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace roxal::nn {

// One file of a model, as the model's version directory holds it.
struct ModelFile {
    std::filesystem::path source;   // absolute
    std::string name;               // relative to the version directory, e.g. "model.onnx"
    std::uint64_t size = 0;
};

struct ModelFileSet {
    std::string stem;               // what the deployed name starts from
    std::vector<ModelFile> files;   // sorted by name
    std::uint64_t totalBytes = 0;
};

// The files of the model at `path`, named as Triton's backends expect them:
//   .onnx           model.onnx, plus the external-data files it names
//   .plan, .engine  model.plan (TensorRT)
//   .pt             model.pt (TorchScript)
//   .xml            model.xml, plus its sibling .bin as model.bin (OpenVINO)
//   a directory     its files, as they are (a Python model, anything else)
// Throws std::invalid_argument naming what is wrong.
ModelFileSet modelFiles(const std::filesystem::path& path);

// The external-data files an ONNX model names, relative to its directory --
// read from the model's protobuf without a schema or a full parse: tensors in
// graph initializers, sparse initializers and node attributes, subgraphs
// included.  Empty for a self-contained model.  Throws std::invalid_argument
// for a file that is not a model, or that names a file outside its directory.
std::vector<std::string> onnxExternalData(const std::filesystem::path& onnxFile);

// "roxal.<stem>.<16 hex digits>": FNV-1a 64 over every file's name, size and
// bytes, and the configuration override.  Each file's hash is memoized for the
// process by path, size, modification time and inode.
std::string deploymentName(const ModelFileSet& files, const std::string& configJson);

// "host[:port]" -> server, from "server=dir;server=dir" (whitespace around
// either side ignored, `~` expanded, directories made absolute).  Servers are
// normalized as normalizeAddress does.  Throws std::invalid_argument.
std::map<std::string, std::filesystem::path> parseRepositoryList(const std::string& list);

// A shared repository directory as given: `~` expanded, made absolute.
// Throws std::invalid_argument when `~` cannot be expanded.
std::filesystem::path repositoryDirectory(std::string directory);

// host[:port], with Triton's gRPC port (8001) when none is given.  An IPv6
// literal is bracketed, its own colons inside: "[::1]" has no port.
std::string normalizeAddress(std::string address);

// Put the model's files in <repository>/<name>/1/, each the first way that
// works: a hard link (no copy, and it needs no extra container mounts), a
// symlink (only if `allowSymlinks`: it resolves only where the server sees
// the file at the same path), or a copy.  Staged in a hidden directory and
// renamed into place; an existing <name> -- an earlier run's, another
// program's -- is kept, as the content-addressed name says it holds the same.
// Returns whether the deployed directory holds any symlink.
bool placeInRepository(const ModelFileSet& files, const std::filesystem::path& repository,
                       const std::string& name, bool allowSymlinks);

// Replace every symlink in <repository>/<name>/1/ with a copy of its target.
void copyOverSymlinks(const std::filesystem::path& repository, const std::string& name);

// The most a load request may carry: it is one gRPC message, and both
// Triton's limit and protobuf's are INT32_MAX.  Some room is left for the
// request's other fields.
constexpr std::uint64_t maxUploadBytes = 0x7fffffffull - (1ull << 20);

// The RepositoryModelLoad request for `name`, with the configuration override
// when `configJson` is not empty, and -- when `upload` is given -- every file
// as a `file:1/<name>` parameter whose bytes are mapped from the file rather
// than read into memory (gRPC sends them from the mapping).  An upload always
// carries a configuration, "{}" if none is given: Triton reads uploaded files
// only with one.
grpc::ByteBuffer encodeLoadRequest(const std::string& name, const std::string& configJson,
                                   const ModelFileSet* upload);

} // namespace roxal::nn
