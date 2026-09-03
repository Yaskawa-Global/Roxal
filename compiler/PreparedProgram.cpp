#include "PreparedProgram.h"

#include <cassert>

#include "ReplSession.h"
#include "SimpleMarkSweepGC.h"
#include "VM.h"

namespace roxal {

PreparedExecutionRecord::~PreparedExecutionRecord()
{
    // Release the session's in-flight claim if this fragment never ran.  A
    // consumed claim (fragmentClaimHeld cleared at activation) belongs to the
    // execution, which releases it itself -- and by the time a consumed
    // record dies a NEWER fragment may hold the claim, which this must not
    // clear.
    //
    // `session` is owned by the VM.  A prepared fragment must not outlive its
    // VM -- the same rule every PreparedProgram already lives under, since
    // its Values would decRef controls the shutdown sweep has freed.
    if (session && fragmentClaimHeld)
        session->fragmentInFlight.store(false, std::memory_order_release);
}

void PreparedProgram::reset()
{
    if (!record_)
        return;
    // Discarding a prepared program destroys a registered typed root, which
    // is a mutator operation: it unregisters from the root registry and
    // releases the launch's Values.  Roots may not be created or destroyed
    // inside an RT yield section, so a driver must never be the one dropping
    // a prepared program.
    assert(!SimpleMarkSweepGC::inGCYieldSectionOnThisThread()
           && "a prepared program must not be discarded inside a GC yield section");
    ScopedGCMutatorCover gcCover;
    record_.reset();
}

PreparedProgram::~PreparedProgram()
{
    reset();
}

PreparedProgram& PreparedProgram::operator=(PreparedProgram&& other) noexcept
{
    if (this == &other)
        return *this;
    reset();
    record_ = std::move(other.record_);
    return *this;
}

} // namespace roxal
