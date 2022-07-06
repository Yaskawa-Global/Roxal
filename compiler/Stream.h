#pragma once

#include <functional>

#include "core/queue.h"
#include "Object.h"
#include "Value.h"


namespace roxal {

class StreamEngine;
class StreamExpr;


class Stream : public Obj
{
public:
    // periodic constant stream
    Stream(double freq, Value initial); 

    // stream from initial value followed by rest stream
    Stream(Value initial, Value rest);

    virtual ~Stream();

    bool periodic; // is this periodic (freq) or externally clocked?

    Value previousValue() const;
    Value currentValue() const;

    // update current value of stream to the next one
    void tick(); 

    double clockFreq; // if streamType is Periodic

    // substitute any proxy names var streams in this streams representation, with
    //  the stream s
    void patch(UnicodeString name, Value& s);

    bool canEvaulateCurrent() const;

    // compute current value (no advance of clock)
    void evaluateCurrent();

    static Value prev(int32_t index, Value s);
    static Value add(Value lhs, Value rhs);

    friend class StreamEngine;
    friend class StreamExpr;

    // logical protected but needed by newObj
    Stream(bool periodic, double freq, ptr<StreamExpr> e);

    std::string toString() const;

protected:
    enum class Type {
        Constant,
        InitialRest,
        Expression
    };
    Type streamType;

    mutable bool initialized;
    void init();

    // expression for this stream
    ptr<StreamExpr> expr;
    // or stream we're wraping (following after initial)
    Value followStream;

    Value initialVal;
    Value previousVal;
    Value currentVal;
};


Stream* streamVal(double freq, Value initial); 
Stream* streamVal(Value initial, Value rest); 


inline bool isStream(const Value& v) { return isObjType(v, ObjType::Stream); }
inline Stream* asStream(const Value& v) { 
    #ifdef DEBUG_BUILD
    if (!isStream(v))
        throw std::runtime_error("Value is not a Stream");
    #endif
    return static_cast<Stream*>(v.asObj()); 
}





Value followedBy(Value initial, Value rest);


//
// Singleton StreamEngine keeps references to all active streams
//  and manages queue of events for updating them
class StreamEngine 
{
public:
    static StreamEngine* instance()
    {
        static StreamEngine engine;
        return &engine;
    }


    StreamEngine(StreamEngine const&)   = delete;
    void operator=(StreamEngine const&) = delete;

    constexpr static double DefaultFreq = 1000; // Hz

    uint64_t currentTime() const; // nanosecs since engine created

    // // create new perodic constant stream
    // Value newStream(double freq, Value initial);

    // // create new stream from initial value followed by rest stream
    // Value newStream(double freq, Value initial, Value rest);

    // register stream (so that it can be updated)
    void registerStream(Stream* s);

    // forget about this stream (will no longer be updated or referenced)
    void unregisterStream(Stream* s);

    // start stream running (e.g. for periodic, schedule first tick)
    void startStream(Stream* s);

    // update state of all streams
    //  (returns number of nanosecs until next call necessary - call again within that time) 
    uint64_t updateStreamStates();

protected:
    StreamEngine();
    virtual ~StreamEngine();

    uint64_t timeSinceBoot() const; // nanosecs since boot
    uint64_t nsOffset; // nanosecs since boot when engine instantiated

    std::vector<Stream*> streams;

    // queue of timestamped future actions to take on streams
    struct QueueEntry {
        enum class Type {
            UpdateStream
        };

        QueueEntry() {}
        QueueEntry(Type t, uint64_t ts, Stream* s) : type(t), timestamp(ts), stream(s) {}
        Type type;
        uint64_t timestamp;
        Stream* stream;
    };

    struct CompareEnriesSort {
        bool operator() (const QueueEntry& lhs, const QueueEntry& rhs) {
            return lhs.timestamp > rhs.timestamp;
        }
    };
    struct CompareEnriesEqualStream {
        bool operator() (const QueueEntry& lhs, const QueueEntry& rhs) {
            return lhs.stream == rhs.stream;
        }
    };

    typedef priority_queue<QueueEntry, CompareEnriesSort, CompareEnriesEqualStream> UpdateQueue;
    UpdateQueue updateQueue;

    uint64_t nextPeriodOnPeriodBoundary(uint64_t period);
    uint64_t nextPeriodOnPeriodBoundary(double freq);

};



}
