// Deploying a model to Triton from its files (TritonDeploy): which files a
// model is, the name its content gives it, where a shared repository gets its
// files, and what the upload request carries.  Runs from the source tree
// (modules/ai holds the test models).

#include "TritonDeploy.h"
#include "triton_inference.pb.h"

#include <grpcpp/grpcpp.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace roxal::nn;

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

template <typename Fn>
void checkThrows(Fn&& fn, const std::string& mentioning, const std::string& what)
{
    try {
        fn();
        check(false, what + " (no error)");
    } catch (const std::invalid_argument& e) {
        check(std::string(e.what()).find(mentioning) != std::string::npos,
              what + " (said: " + e.what() + ")");
    }
}

std::string read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
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

// A length-delimited protobuf field, for hand-made ONNX files.
std::string field(int number, const std::string& body)
{
    std::string out(1, static_cast<char>((number << 3) | 2));
    std::uint64_t n = body.size();
    while (n >= 0x80) {
        out.push_back(static_cast<char>(n | 0x80));
        n >>= 7;
    }
    out.push_back(static_cast<char>(n));
    return out + body;
}

} // namespace

int main()
{
    const fs::path tmp = fs::temp_directory_path() / ("roxal_triton_deploy_test." + std::to_string(::getpid()));
    fs::create_directories(tmp);

    // ---- which files a model is ----
    const fs::path external = "modules/ai/add-external.onnx";
    check(onnxExternalData("modules/ai/scale-shift.onnx").empty(), "a self-contained model names no external data");
    const auto locations = onnxExternalData(external);
    check(locations.size() == 1 && locations[0] == "add-external.weights", "external data is found");

    const ModelFileSet onnx = modelFiles(external);
    check(onnx.stem == "add-external", "the stem is the file's");
    check(onnx.files.size() == 2 && onnx.files[0].name == "add-external.weights" &&
          onnx.files[1].name == "model.onnx", "an ONNX model is its file and its weights");
    check(onnx.totalBytes == fs::file_size(external) + fs::file_size("modules/ai/add-external.weights"),
          "the total counts every file");
    check(onnx.files[1].source.is_absolute(), "sources are absolute");

    write(tmp / "notonnx.onnx", "this is not a model \xff\xff\xff");
    checkThrows([&] { modelFiles(tmp / "notonnx.onnx"); }, "is not an ONNX model", "a non-model is refused");
    // ModelProto.graph.initializer.external_data = {location: "../secret"}
    const std::string escape = field(7, field(5, field(13, field(1, "location") + field(2, "../secret"))));
    write(tmp / "escape.onnx", escape);
    checkThrows([&] { modelFiles(tmp / "escape.onnx"); }, "outside its own directory",
                "external data outside the model's directory is refused");
    const std::string missing = field(7, field(5, field(13, field(1, "location") + field(2, "gone.bin"))));
    write(tmp / "missing.onnx", missing);
    checkThrows([&] { modelFiles(tmp / "missing.onnx"); }, "which is not there", "missing external data is reported");

    write(tmp / "vino" / "net.xml", "<net/>");
    checkThrows([&] { modelFiles(tmp / "vino" / "net.xml"); }, "has no 'net.bin'", "OpenVINO needs its .bin");
    write(tmp / "vino" / "net.bin", "weights");
    const ModelFileSet vino = modelFiles(tmp / "vino" / "net.xml");
    check(vino.files.size() == 2 && vino.files[0].name == "model.bin" && vino.files[1].name == "model.xml",
          "an OpenVINO model is its .xml and .bin");

    write(tmp / "engine.plan", "plan");
    check(modelFiles(tmp / "engine.plan").files[0].name == "model.plan", "a TensorRT engine is model.plan");
    write(tmp / "script.pt", "pt");
    check(modelFiles(tmp / "script.pt").files[0].name == "model.pt", "TorchScript is model.pt");
    write(tmp / "weights.safetensors", "w");
    checkThrows([&] { modelFiles(tmp / "weights.safetensors"); }, "deploy the model's directory",
                "an unknown kind of file is refused");

    write(tmp / "py model" / "model.py", "print('hi')\n");
    write(tmp / "py model" / "data" / "table.txt", "1 2 3\n");
    write(tmp / "py model" / ".cache" / "junk", "x");
    write(tmp / "py model" / ".hidden", "x");
    const ModelFileSet dir = modelFiles(tmp / "py model");
    check(dir.stem == "py_model", "a directory's stem is its name, made safe");
    check(dir.files.size() == 2 && dir.files[0].name == "data/table.txt" && dir.files[1].name == "model.py",
          "a directory is its files, hidden ones left out");
    checkThrows([&] { modelFiles(tmp / "nothing-here"); }, "there is no model", "a missing path is reported");

    // ---- the name its content gives it ----
    const std::string name = deploymentName(onnx, "");
    check(name.rfind("roxal.add-external.", 0) == 0 && name.size() == std::string("roxal.add-external.").size() + 16,
          "names are roxal.<stem>.<16 hex digits> (" + name + ")");
    check(deploymentName(onnx, "") == name, "the same files give the same name");
    check(deploymentName(onnx, "{\"max_batch_size\":8}") != name, "a configuration changes the name");
    write(tmp / "changing.plan", "version one");
    const std::string before = deploymentName(modelFiles(tmp / "changing.plan"), "");
    write(tmp / "changing.plan", "version two!");
    check(deploymentName(modelFiles(tmp / "changing.plan"), "") != before, "changed content changes the name");

    // ---- addresses and the shared-repository setting ----
    check(normalizeAddress("gpu-rack") == "gpu-rack:8001", "the port defaults to 8001");
    check(normalizeAddress("gpu-rack:9001") == "gpu-rack:9001", "a given port stays");
    check(normalizeAddress("[::1]") == "[::1]:8001" && normalizeAddress("[::1]:9") == "[::1]:9",
          "an IPv6 literal's own colons are not a port");
    const auto list = parseRepositoryList(" localhost = /srv/models ; triton://gpu:9000=/mnt/m/../m2;; ");
    check(list.size() == 2 && list.at("localhost:8001") == fs::path("/srv/models") &&
          list.at("gpu:9000") == fs::path("/mnt/m2"), "the setting maps normalized servers to directories");
    if (const char* home = std::getenv("HOME"))
        check(parseRepositoryList("h=~/r").at("h:8001") == fs::path(home) / "r", "~ is the home directory");
    checkThrows([] { parseRepositoryList("just-a-dir"); }, "is not server=directory", "an entry needs both sides");

    // ---- placing a model in a shared repository ----
    const fs::path repo = tmp / "repo";
    fs::create_directories(repo);
    placeInRepository(onnx, repo, name, true);
    check(read(repo / name / "1" / "model.onnx") == read(external), "the model is in place");
    check(read(repo / name / "1" / "add-external.weights") == read("modules/ai/add-external.weights"),
          "its weights beside it");
    bool staged = false;
    for (const auto& entry : fs::directory_iterator(repo))
        staged = staged || entry.path().filename().string().rfind(".roxal-staging", 0) == 0;
    check(!staged, "no staging directory is left behind");
    check(!fs::is_symlink(repo / name / "1" / "model.onnx"), "on one filesystem, a hard link (or a copy), not a symlink");
    placeInRepository(onnx, repo, name, true);
    check(read(repo / name / "1" / "model.onnx") == read(external), "placing it again keeps what is there");

    const std::string linked = "roxal.linked.0000000000000000";
    fs::create_directories(repo / linked / "1");
    fs::create_symlink(fs::absolute(external), repo / linked / "1" / "model.onnx");
    check(placeInRepository(onnx, repo, linked, true), "an existing deployment holding a symlink says so");
    copyOverSymlinks(repo, linked);
    check(!fs::is_symlink(repo / linked / "1" / "model.onnx") &&
          read(repo / linked / "1" / "model.onnx") == read(external), "symlinks become copies");

    // ---- the upload request ----
    {
        grpc::ByteBuffer request = encodeLoadRequest(name, "{\"max_batch_size\":8}", &onnx);
        roxal::triton::RepositoryModelLoadRequest parsed;
        check(parsed.ParseFromString(flatten(request)), "the load request parses");
        check(parsed.model_name() == name, "it names the model");
        const auto& params = parsed.parameters();
        check(params.count("config") && params.at("config").string_param() == "{\"max_batch_size\":8}",
              "it carries the configuration");
        check(params.count("file:1/model.onnx") && params.at("file:1/model.onnx").bytes_param() == read(external),
              "it carries the model file");
        check(params.count("file:1/add-external.weights") &&
              params.at("file:1/add-external.weights").bytes_param() == read("modules/ai/add-external.weights"),
              "and its weights");
        check(params.size() == 3, "and nothing else");
    }
    {
        grpc::ByteBuffer request = encodeLoadRequest(name, "", &onnx);
        roxal::triton::RepositoryModelLoadRequest parsed;
        check(parsed.ParseFromString(flatten(request)) && parsed.parameters().count("config") &&
              parsed.parameters().at("config").string_param() == "{}",
              "an upload with no configuration carries {} (Triton reads uploaded files only with one)");
    }
    {
        grpc::ByteBuffer request = encodeLoadRequest(name, "", nullptr);
        roxal::triton::RepositoryModelLoadRequest parsed;
        check(parsed.ParseFromString(flatten(request)) && parsed.parameters().empty(),
              "a load from the repository carries no parameters");
    }

    std::error_code ignored;
    fs::remove_all(tmp, ignored);
    if (failures == 0)
        std::cout << "triton deploy: ok\n";
    return failures == 0 ? 0 : 1;
}
