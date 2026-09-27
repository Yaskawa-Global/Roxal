// VM::forgetUserModules(): a host reloading edited library modules between
// runs.  The registry keeps the first revision of every module it loaded;
// forgetting drops chosen entries so the next import loads the current
// source -- all or nothing when an unselected module imports a selected one.
//
// Its own executable: it needs a VM whose registry holds only its modules.

#include "compiler/Object.h"
#include "compiler/VM.h"
#include "core/Output.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

class CaptureSink final : public roxal::OutputSink {
public:
    roxal::OutputResult emit(const roxal::OutputEventView& event) override
    {
        if (event.kind != roxal::OutputKind::Print)
            return roxal::OutputResult::Accepted;
        std::lock_guard<std::mutex> lock(mutex_);
        text_.append(event.text);
        return roxal::OutputResult::Accepted;
    }

    std::string take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string result = std::move(text_);
        text_.clear();
        return result;
    }

private:
    std::mutex mutex_;
    std::string text_;
};

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

// Each revision is longer than the last, so its stamp differs even when two
// writes land within the filesystem's timestamp resolution.
std::string libSource(const std::string& version)
{
    return "func ver():\n  return 'lib-" + version + "'\n";
}

bool sameNames(std::vector<std::string> actual, std::vector<std::string> expected)
{
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected;
}

std::string joined(const std::vector<std::string>& names)
{
    std::string result;
    for (const auto& name : names)
        result += (result.empty() ? "" : ",") + name;
    return "[" + result + "]";
}

} // namespace

int main()
{
    using namespace roxal;

    int failures = 0;
    auto expect = [&](bool condition, const std::string& description) {
        if (!condition) {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    };

    const std::filesystem::path dir = std::filesystem::temp_directory_path()
        / ("roxal_module_reload_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    writeFile(dir / "mr_lib.rox", libSource("v1"));
    writeFile(dir / "mr_user.rox",
              "import mr_lib\n"
              "func via():\n"
              "  return 'user+' + mr_lib.ver()\n");
    // A module inside a namespace folder (no init.rox): `import mr_pkg.mr_sub`
    // reaches it through a shared namespace node.
    std::filesystem::create_directories(dir / "mr_pkg");
    writeFile(dir / "mr_pkg" / "mr_sub.rox", libSource("sub-v1"));

    VM& vm = VM::instance();
    CaptureSink sink;
    OutputRouter::setSink(&sink);
    vm.appendModulePaths({ dir.string() });

    auto run = [&](const std::string& source) {
        std::stringstream stream(source);
        ProgramOptions options;
        options.sourceName = "module_reload_main";
        const ExecutionStatus status = vm.executeProgramSync(stream, std::move(options));
        const std::string output = sink.take();
        return status == ExecutionStatus::OK ? output : "status " + std::to_string(int(status));
    };
    auto runFragment = [&](const std::string& source) {
        std::stringstream stream(source);
        FragmentOptions options;
        options.replMode = false;
        const ExecutionStatus status =
            vm.defaultReplSession().evaluateFragmentSync(stream, std::move(options));
        const std::string output = sink.take();
        return status == ExecutionStatus::OK ? output : "status " + std::to_string(int(status));
    };
    auto staleOnly = [](const VM::UserModuleInfo& module) { return module.stale; };

    const std::string useLib = "import mr_lib\nprint(mr_lib.ver())\n";
    const std::string useUser = "import mr_user\nprint(mr_user.via())\n";

    expect(run(useLib) == "lib-v1\n", "first run loads the library");

    writeFile(dir / "mr_lib.rox", libSource("v22"));
    expect(run(useLib) == "lib-v1\n",
           "without a forget, the registry keeps the revision it loaded first");

    // What a selector is shown, and a stale-only forget.
    {
        std::vector<VM::UserModuleInfo> seen;
        const auto result = vm.forgetUserModules([&](const VM::UserModuleInfo& module) {
            seen.push_back(module);
            return module.stale;
        });
        const auto lib = std::find_if(seen.begin(), seen.end(),
                                      [](const auto& m) { return m.name == "mr_lib"; });
        expect(lib != seen.end(), "the selector sees each registered module");
        if (lib != seen.end()) {
            expect(lib->stale, "an edited module is reported stale");
            expect(lib->kind == ModuleKind::User, "a .rox module is reported as User");
            expect(std::filesystem::path(lib->sourcePath).filename() == "mr_lib.rox",
                   "the selector sees the module's source path");
        }
        expect(sameNames(result.forgotten, { "mr_lib" }) && result.blockedBy.empty(),
               "a stale-only forget drops the edited module: got "
                   + joined(result.forgotten) + " blocked " + joined(result.blockedBy));
    }
    expect(!vm.lookupUserModule(toUnicodeString("mr_lib")).has_value(),
           "a forgotten module is no longer registered");
    expect(run(useLib) == "lib-v22\n", "after a forget, the next import loads the edit");

    {
        const auto result = vm.forgetUserModules(staleOnly);
        expect(result.forgotten.empty() && result.blockedBy.empty(),
               "nothing is stale once the edit has been loaded");
    }

    // An unselected importer blocks the forget, and nothing changes.
    expect(run(useUser) == "user+lib-v22\n", "an importer links the current library");
    writeFile(dir / "mr_lib.rox", libSource("v333"));
    {
        const auto result = vm.forgetUserModules([](const VM::UserModuleInfo& module) {
            return module.name == "mr_lib";
        });
        expect(result.forgotten.empty() && sameNames(result.blockedBy, { "mr_user" }),
               "an unselected importer blocks the forget: got "
                   + joined(result.forgotten) + " blocked " + joined(result.blockedBy));
    }
    expect(vm.lookupUserModule(toUnicodeString("mr_lib")).has_value(),
           "a blocked forget leaves the registry unchanged");
    expect(run(useUser) == "user+lib-v22\n",
           "a blocked forget leaves both modules at the revision they linked");

    // The importer is stale too (its record of the library no longer
    // matches), so a stale-only forget takes both.
    {
        const auto result = vm.forgetUserModules(staleOnly);
        expect(sameNames(result.forgotten, { "mr_lib", "mr_user" }) && result.blockedBy.empty(),
               "an importer of an edited module is stale as well: got "
                   + joined(result.forgotten) + " blocked " + joined(result.blockedBy));
    }
    expect(run(useUser) == "user+lib-v333\n", "the importer is rebuilt against the edit");

    // A dotted import binds through a namespace node, which is not forgotten
    // (it has no source) and so must be rebound to the reloaded module.
    const std::string useSub = "import mr_pkg.mr_sub\nprint(mr_pkg.mr_sub.ver())\n";
    expect(run(useSub) == "lib-sub-v1\n", "a dotted import loads the module");
    writeFile(dir / "mr_pkg" / "mr_sub.rox", libSource("sub-v22"));
    {
        const auto result = vm.forgetUserModules(staleOnly);
        expect(sameNames(result.forgotten, { "mr_pkg.mr_sub" }) && result.blockedBy.empty(),
               "a stale-only forget takes the module, not its namespace node: got "
                   + joined(result.forgotten) + " blocked " + joined(result.blockedBy));
    }
    expect(vm.lookupUserModule(toUnicodeString("mr_pkg")).has_value(),
           "the namespace node stays registered");
    expect(run(useSub) == "lib-sub-v22\n",
           "a dotted re-import reaches the reloaded module through the namespace node");

    // A fragment session's compiler keeps its own record of what it
    // imported; a forget must reach past it.
    expect(runFragment(useLib) == "lib-v333\n", "a fragment imports the library");
    writeFile(dir / "mr_lib.rox", libSource("v4444"));
    {
        const auto result = vm.forgetUserModules(staleOnly);
        expect(sameNames(result.forgotten, { "mr_lib", "mr_user" }),
               "the forget after a fragment import: got " + joined(result.forgotten));
    }
    expect(runFragment(useLib) == "lib-v4444\n",
           "re-importing in a fragment session loads the edit");

    // The REPL's /reload: everything, and nothing can block it.
    vm.clearUserModuleRegistry();
    expect(!vm.lookupUserModule(toUnicodeString("mr_lib")).has_value()
               && !vm.lookupUserModule(toUnicodeString("mr_user")).has_value(),
           "clearUserModuleRegistry forgets every module");
    expect(run(useUser) == "user+lib-v4444\n", "modules load again after a full reset");

    OutputRouter::setSink(nullptr);
    VM::shutdownIfConstructed();
    std::filesystem::remove_all(dir);

    if (failures == 0)
        std::cout << "module reload tests passed\n";
    return failures == 0 ? 0 : 1;
}
