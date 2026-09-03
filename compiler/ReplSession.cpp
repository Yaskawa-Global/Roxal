#include "ReplSession.h"

#include "Object.h"
#include "RoxalCompiler.h"
#include "SimpleMarkSweepGC.h"
#include "VM.h"

namespace roxal {

Value ReplSession::module() const
{
    if (!valid())
        return Value::nilVal();
    return state_->roots.get().module;
}

PrepareFragmentResult ReplSession::prepareFragment(std::istream& source,
                                                   FragmentOptions options)
{
    PrepareFragmentResult result;
    if (!valid()) {
        result.status = PrepareFragmentStatus::ShuttingDown;
        return result;
    }

    // One producer at a time.  The driver never blocks on this, and the root
    // tracer never touches it.
    std::unique_lock<std::mutex> producerLock(state_->producerMutex, std::try_to_lock);
    if (!producerLock.owns_lock()) {
        result.status = PrepareFragmentStatus::SessionBusy;
        return result;
    }

    // Idle BEFORE compiling, not merely before the closure is handed over:
    // compiling against a module a live fragment is still mutating is the
    // race this ordering exists to prevent.  Checked UNDER the producer lock
    // -- two producers that both passed an unlocked check would both compile.
    if (state_->fragmentInFlight.load(std::memory_order_acquire)) {
        result.status = PrepareFragmentStatus::SessionBusy;
        return result;
    }

    // Preparation of any kind is serialized per VM: fragment and full-program
    // compilations share module registries and caches.
    std::lock_guard<std::mutex> preparation(vm_->preparationMutex_);

    // Compilation defers tracing collection and runs covered, exactly as a
    // full program's preparation does.
    SimpleMarkSweepGC::CollectionDeferralScope deferCollection;
    ScopedGCMutatorCover gcCover;

    if (!state_->compiler)
        state_->compiler = std::make_unique<RoxalCompiler>();
    RoxalCompiler& compiler = *state_->compiler;
    vm_->configureFragmentCompiler(compiler, options.replMode);

    Value function { Value::nilVal() };
    try {
        function = compiler.compile(source, "cli", state_->roots.get().module,
                                    options.sourceName);
    } catch (std::exception& e) {
        compiler.setReplMode(false);
        result.status = PrepareFragmentStatus::CompileError;
        result.diagnostics.message = e.what();
        return result;
    }
    compiler.setReplMode(false);

    if (function.isNil()) {
        result.status = PrepareFragmentStatus::CompileError;
        return result;
    }

    // The first fragment mints the session's module; later ones compile into
    // it, which is what makes their declarations visible to each other.
    if (state_->roots.get().module.isNil())
        state_->roots.get().module = asFunction(function)->moduleType.strongRef();

    auto record = std::make_unique<PreparedExecutionRecord>();
    record->sourceName = options.sourceName;
    record->completion = options.completion;
    record->session = state_;

    ExecutionRootValues values;
    values.closure = Value::closureVal(function);
    values.module = state_->roots.get().module;
    record->roots = std::move(values);

    state_->fragmentInFlight.store(true, std::memory_order_release);
    record->fragmentClaimHeld = true;
    result.fragment = PreparedProgram(std::move(record));
    result.status = PrepareFragmentStatus::Ready;
    return result;
}

ExecutionStatus ReplSession::evaluateFragmentSync(std::istream& source,
                                                  FragmentOptions options)
{
    if (!valid())
        return ExecutionStatus::RuntimeError;

    // Once a host owns execution, evaluating here would be a second driver on
    // the same VM.  Checked before preparing, so a refused call compiles
    // nothing and leaves the session idle.
    if (vm_->embeddedDriverAttached())
        return ExecutionStatus::Busy;

    PrepareFragmentResult prepared = prepareFragment(source, options);
    switch (prepared.status) {
    case PrepareFragmentStatus::Ready:
        break;
    case PrepareFragmentStatus::SessionBusy:
        return ExecutionStatus::Busy;
    case PrepareFragmentStatus::CompileError:
    case PrepareFragmentStatus::ShuttingDown:
    default:
        return ExecutionStatus::CompileError;
    }

    return vm_->evaluateFragmentOnThisThread(*state_, std::move(prepared.fragment));
}

bool ReplSession::valid() const noexcept
{
    // A retained handle must not look usable after the VM has shut down.
    return vm_ != nullptr && state_ != nullptr && !vm_->isShutdown();
}

} // namespace roxal
