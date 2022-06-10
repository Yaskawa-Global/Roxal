#pragma once

#include <queue>
#include <functional>

#include "Object.h"
#include "Value.h"


namespace roxal {

class StreamEngine;


class Stream : public Obj
{
public:
    Stream(ptr<StreamEngine> engine, double freq, Value initial); // periodic
    virtual ~Stream();

    enum class Type {
        Periodic,
        ExternallyClocked
    };

    Value previousvalue() const;
    Value currentValue() const;

    // update current value of stream to the next one
    void tick(); 

    Type streamType;
    double clockFreq; // if streamType is Periodic
protected:

    Value initialVal;
    Value previousVal;
    Value currentVal;

    ptr<StreamEngine> engine;
};


Stream* streamVal(ptr<StreamEngine> engine, double freq, Value initial); 



class StreamEngine : public std::enable_shared_from_this<StreamEngine>
{
public:
    StreamEngine();
    virtual ~StreamEngine();

    uint64_t currentTime() const; // nanosecs since engine created

    // create new perodic stream
    Value newStream(double freq, Value initial);

    // forget about this stream (will no longer be updated or referenced)
    void forgetStream(Stream* s);


    // update state of all streams
    //  (returns number of nanosecs until next call necessary - call again within that time) 
    uint64_t updateStreamStates();

protected:
    uint64_t timeSinceBoot() const; // nanosecs since boot
    uint64_t nsOffset; // nanosecs since boot when engine instantiated

    std::vector<Value> streams;

    // queue of timestamped future actions to take on streams
    struct QueueEntry {
        uint64_t timestamp;
        enum class Type {
            UpdateStream
        };
        Type type;
        Stream* stream;

    };

    struct CompareEnries {
        bool operator() (QueueEntry lhs, QueueEntry rhs) {
            return lhs.timestamp < rhs.timestamp;
        }
    };

    std::priority_queue<QueueEntry, std::vector<QueueEntry>, CompareEnries> updateQueue;
};




}
