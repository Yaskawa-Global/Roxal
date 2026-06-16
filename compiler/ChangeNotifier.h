#pragma once

#include <functional>
#include <vector>

#include <core/common.h>      // roxal::ptr
#include <core/TimePoint.h>   // roxal::TimePoint
#include "Value.h"            // roxal::Value

namespace df { class Signal; }

namespace roxal {

// A list of value-change callbacks. Shared by df::Signal (its change callbacks) and by
// ObjChangeNotifier — a lightweight observer a property's slot can hold so C++ can be
// notified of a change WITHOUT a full dataflow signal (no DataflowEngine involvement).
//
// The 3-arg callback signature matches df::Signal's existing one, so signal consumers
// are unchanged; the signal-less (bare) path passes a null signal and the current time
// (consumers that only want the value ignore the first two args).
struct ChangeNotifier {
    using Callback = std::function<void(TimePoint, ptr<df::Signal>, const Value&)>;

    std::vector<Callback> callbacks;

    void addCallback(Callback cb) { callbacks.push_back(std::move(cb)); }
    bool empty() const { return callbacks.empty(); }

    void notify(TimePoint t, ptr<df::Signal> sig, const Value& v) const {
        for (const auto& cb : callbacks) {
            try { cb(t, sig, v); } catch (...) {}
        }
    }
};

} // namespace roxal
