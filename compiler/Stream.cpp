#include <time.h>
#include <string.h>

#include "Stream.h"

using namespace roxal;



Stream::Stream(ptr<StreamEngine> engine, double freq, Value initial)
{ 
    type = ObjType::Stream; 
    streamType = Type::Periodic;
    clockFreq = freq;
    initialVal = initial;
    previousVal = currentVal = initialVal;
    this->engine = engine;
}

Stream::~Stream() 
{
    engine->forgetStream(this);
}


void Stream::tick()
{
    //TODO: update values
    previousVal = currentVal;
    currentVal = intVal(currentVal.asInt()+1); //!!! counter for testing
    std::cout << "new value: "+toString(currentVal) << std::endl << std::flush;//!!!
}





StreamEngine::StreamEngine() 
{ 
    nsOffset = timeSinceBoot();
    //std::cout << "current time:" << currentTime() << std::endl;//!!!
}

StreamEngine::~StreamEngine() 
{
    
}





Value StreamEngine::newStream(double freq, Value initial)
{
    Stream* stream = streamVal(shared_from_this(),freq,initial);
    auto streamVal = objVal(stream);
    streams.push_back(streamVal);

    // queue first update for 1/freq secs in future
    //  TODO: compute the first update time to be on a multiple of 1/freq boundary of currentTime()
    QueueEntry entry;
    entry.type = QueueEntry::Type::UpdateStream;
    entry.timestamp = currentTime() + uint64_t(1000000000ULL/freq);
    entry.stream = stream;
    updateQueue.push(entry);

    return streamVal;
}


void StreamEngine::forgetStream(Stream* s)
{
    //TODO: remove from streams list and remove and queue entries that reference it
}



uint64_t StreamEngine::updateStreamStates()
{
    while(updateQueue.top().timestamp < currentTime()) {
        auto entry { updateQueue.top() };
        updateQueue.pop();
        entry.stream->tick();

        // if periodic, schedule the next tick
        if (entry.stream->streamType == Stream::Type::Periodic) {
            entry.timestamp += uint64_t(1000000000ULL/entry.stream->clockFreq);
            updateQueue.push(entry);
        }
    }

    return 1;
}



uint64_t StreamEngine::currentTime() const
{
    return timeSinceBoot() - nsOffset;
}


uint64_t StreamEngine::timeSinceBoot() const
{
    struct timespec tp;
    if (clock_gettime(CLOCK_MONOTONIC,&tp) != 0)
        throw std::runtime_error("Error querying system monotonic clock:"+std::string(strerror(errno)));
    return uint64_t(tp.tv_sec)*1000000000ull+uint64_t(tp.tv_nsec);
}


Stream* roxal::streamVal(ptr<StreamEngine> engine, double freq, Value initial)
{
    return newObj<Stream>(__func__,engine,freq,initial);
}
