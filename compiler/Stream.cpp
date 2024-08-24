#include <time.h>
#include <string.h>
#include <cassert>
#include <iomanip>

#include "Stream.h"

using namespace roxal;



class roxal::StreamExpr {
public:
    enum Op {
        Const, // val is constant stream
        Prev,  // val is Int index (<=0) and stream is a Stream (or string name to be subst before eval)
        Not,   // 1 operand (unary)
        And,   // 2 operands (binary)
        Or,
        Negate,
        Add,
        Subtract,
        Multiply
    };

    StreamExpr(const Value& v)
        : op(Const)
    {
        val = v;
    }

    StreamExpr(int32_t index, const Value& s)
        : op(Prev)
    {
        val = intVal(index);
        #ifdef DEBUG_BUILD
        assert(isStream(s) || isString(s));
        #endif
        stream = s;
    }

    StreamExpr(Op o, ptr<StreamExpr> lhs, ptr<StreamExpr> rhs)
        : op(o)
    {
        operands.push_back(lhs);
        operands.push_back(rhs);
    }

    ~StreamExpr() {}

    Value eval() const;


    bool canEval() const {
        if (op == Op::Const) { return true; }
        else if (op == Op::Prev) {
            return isStream(stream);
        }
        else {
            for(auto& operand : operands)
                if (!operand->canEval())
                    return false;
            return true;
        }
    }

    void patch(UnicodeString name, const Value& s) {
        if (op == Op::Const) {}
        else if (op == Op::Prev) {
            if (isString(stream) && asUString(stream) == name)
                stream = s;
        }
        else {
            for(auto& operand : operands)
                operand->patch(name,s);
        }
    }

protected:
    Op op;
    Value val;    // Const
    Value stream; // Prev (stream or string name)
    std::vector<ptr<StreamExpr>> operands;

};






Stream::Stream(double freq, Value initial)
    : initialized(false), streamType(Type::Constant), expr(nullptr)
{
    type = ObjType::Stream;
    if (freq>0.0) {
        periodic = true;
        clockFreq = freq;
    }
    else {
        //periodic = false;
        //clockFreq = 0.0; // unused
        // FIXME: !!! default streams should be nonperiodic (or clocked at 'runtime' rate? or manually clocked/ticked?)
        periodic = true;
        clockFreq = 100.0;
    }
    initialVal = initial;
}


Stream::Stream(Value initial, Value rest)
    : initialized(false), streamType(Type::InitialRest), expr(nullptr)
{
    type = ObjType::Stream;
    initialVal = initial;

    #ifdef DEBUG_BUILD
    assert(isStream(rest));
    assert(!isClosure(initial)); // no streams of functions
    #endif
    followStream = rest;
}


Stream::Stream(bool periodic, double freq, ptr<StreamExpr> e)
    : initialized(false), streamType(Type::Expression), clockFreq(freq), expr(e)
{
    type = ObjType::Stream;
    this->periodic = periodic;
}



Stream::~Stream()
{
}


Value Stream::previousValue() const
{
    if (!initialized) const_cast<Stream*>(this)->init();
    return previousVal;
}

Value Stream::currentValue() const
{
    if (!initialized) const_cast<Stream*>(this)->init();
    return currentVal;
}


void Stream::init()
{
    if (initialized) return;

    switch (streamType) {
        case Type::Constant: {
            previousVal = currentVal = initialVal;
            expr = std::make_shared<StreamExpr>(initialVal);
        } break;
        case Type::InitialRest: {
            if (isStream(initialVal)) {
                #ifdef DEBUG_BUILD
                assert(asStream(initialVal)->canEvaulateCurrent());
                assert(asStream(followStream)->canEvaulateCurrent());
                #endif
                periodic = asStream(initialVal)->periodic;
                clockFreq = asStream(initialVal)->clockFreq;
                initialVal = asStream(initialVal)->currentValue();
            }
            else {
                periodic = true;
                clockFreq = StreamEngine::DefaultFreq;
            }
            previousVal = currentVal = initialVal;
        } break;
        case Type::Expression: {
            #ifdef DEBUG_BUILD
            if (!expr->canEval())
                throw std::runtime_error("Can't init stream with unpatched variables");
            #endif
            initialVal = previousVal = currentVal = expr->eval();
        } break;
        default:
            throw std::runtime_error("Unhandled Stream::Type");
    }

    initialized = true;

    StreamEngine::instance()->startStream(this);
}


std::string Stream::toString() const
{
    std::ostringstream oss;
    oss << std::hex << uint64_t(this) << std::dec;
    oss << ":stream(";
    if (streamType==Type::Constant) oss << "C ";
    else if (streamType==Type::InitialRest) oss << "|> ";
    else if (streamType==Type::Expression) oss << "E ";
    else oss << "? ";
    if (streamType!=Type::Expression)
        oss << ::toString(initialVal);
    if (periodic)
        oss << "," << std::fixed << std::setprecision(2) << std::to_string(clockFreq) << "Hz";
    oss << ")";

    if (streamType==Type::Expression && expr->canEval() && !initialized)
        const_cast<Stream*>(this)->init();

    if (initialized)
        oss << "=" << ::toString(currentVal);
    else
        oss << "[uninit]";
    return oss.str();
}




void Stream::tick()
{
    if (!initialized) init();

    previousVal = currentVal;

    evaluateCurrent();

    //std::cout << "tick(): " << toString() << std::endl << std::flush;//!!!
}


bool Stream::canEvaulateCurrent() const
{
    if (streamType==Type::Constant)
        return true;
    else if (streamType==Type::InitialRest)
        return asStream(followStream)->canEvaulateCurrent();
    else if (streamType==Type::Expression)
        return expr->canEval();
    throw std::runtime_error(__func__+std::string("- unhandled stream type"));
}



void Stream::evaluateCurrent()
{
    if (!initialized) init();

    #ifdef DEBUG_BUILD
    assert(isStream(followStream) || (expr != nullptr));
    #endif

    if (isStream(followStream))
        currentVal = asStream(followStream)->currentValue();
    else
        currentVal = expr->eval();
}


void Stream::patch(UnicodeString name, Value& s)
{
    if (streamType==Type::Expression) {
        #ifdef DEBUG_BUILD
        assert(expr != nullptr);
        #endif
        expr->patch(name,s);
    }
    else if (streamType==Type::InitialRest) {
        asStream(followStream)->patch(name,s);
    }
}




Value Stream::prev(int32_t index, Value s)
{
    #ifdef DEBUG_BUILD
    if (!isStream(s) && !isString(s))
        throw std::runtime_error("Stream::prev() requires a stream (or stream proxy name)");
    #endif


    //std::vector<Value> args {};
    std::vector<Value> args {intVal(0), realVal(100.0)}; //!!!
    auto resultValue = construct(ValueType::Stream, args.begin(), args.end());
    auto result = asStream(resultValue);
    if (isStream(s)) {
        auto stream { asStream(s) };
        result->periodic = stream->periodic;
        result->clockFreq = stream->clockFreq;
    }
    else {
//FIXME: !!! somehow set prev stream's periodicity & clock when it is a name (only have engine schedule first on init?)
        result->periodic = true; // hardcode defaults for now
        result->clockFreq = 100.0;
    }
    result->streamType = Stream::Type::Expression;
    result->expr = std::make_shared<StreamExpr>(index, s);

    return resultValue;
}

// TODO:
//  * eliminate the construct() call  (pass Streams around as Values, but compose them using non-Value StreamExpr)


Value Stream::add(Value lhs, Value rhs)
{
    #ifdef DEBUG_BUILD
    if (!isStream(lhs) && !isStream(rhs))
        throw std::runtime_error("Stream::add() only operates on streams");
    #endif

    bool periodic = isStream(lhs) ? asStream(lhs)->periodic : asStream(rhs)->periodic;
    double freq = 0.0;
    if (periodic)
        freq = isStream(lhs) ? asStream(lhs)->clockFreq : asStream(rhs)->clockFreq;

    auto lhsexpr = isStream(lhs) ? asStream(lhs)->expr : std::make_shared<StreamExpr>(lhs);
    auto rhsexpr = isStream(rhs) ? asStream(rhs)->expr : std::make_shared<StreamExpr>(rhs);

    Value result { objVal(newObj<Stream>(__func__,periodic,freq,std::make_shared<StreamExpr>(StreamExpr::Add, lhsexpr, rhsexpr))) };

    return result;
}



Value StreamExpr::eval() const
{
    switch(op) {
        case Const: return val;
        case Prev: {
            if (isString(this->stream))
                throw std::runtime_error("Undefined stream \""+objToString(this->stream)+"\"");

            auto stream = asStream(this->stream);
            auto index = val.asInt();
            if (index == 0)
                return stream->currentValue();
            else if (index == -1)
                return stream->previousValue();
            else
                throw std::runtime_error("Stream index "+std::to_string(index)+" out-of-range.");
        }
        case Not: return negate(operands.at(0)->eval());
        case Add: {
            #ifdef DEBUG_BUILD
            assert(operands.size()==2);
            #endif
            return add(operands.at(0)->eval(),operands.at(1)->eval());
        }
        default:
            throw std::runtime_error("Unhandled Stream operation "+std::to_string(int(op)));
    }
}




StreamEngine::StreamEngine()
{
    nsOffset = timeSinceBoot();
    //std::cout << "current time:" << currentTime() << std::endl;//!!!

    // #ifdef DEBUG_BUILD
    // auto ns1sec = 1000000000ULL;
    // uint64_t ct = currentTime();
    // uint64_t pt = ct;
    // while (1) {
    //     pt = ct;
    //     ct = currentTime();
    //     std::cout << ct << " " << nextPeriodOnPeriodBoundary(10.0) << " " << (ct+(ns1sec/10)) << " " << (ct-pt) << " " << (ct/ns1sec) << std::endl;//!!!
    //     std::this_thread::sleep_for(std::chrono::milliseconds(100));
    //     //assert(nextPeriodOnPeriodBoundary(10.0) == ct+(ns1sec/1000));
    // }
    // #endif

}

StreamEngine::~StreamEngine()
{

}



uint64_t StreamEngine::nextPeriodOnPeriodBoundary(uint64_t period)
{
    throw std::runtime_error("unused?");
    return (uint64_t(currentTime()+1.5*period)*period)/period;
}

uint64_t StreamEngine::nextPeriodOnPeriodBoundary(double freq)
{
    #ifdef DEBUG_BUILD
    assert(freq>0.0);
    #endif
    uint64_t period = 1000000000ULL/freq;
    uint64_t nextPlusHalf = currentTime()+1.5*period;
    uint64_t nextOnBoundary = nextPlusHalf - (nextPlusHalf%period);
    return nextOnBoundary;
    //return (uint64_t(currentTime()+1.5*period)*period)/period;
}


void StreamEngine::registerStream(Stream* s)
{
    streams.push_back(s);
    //std::cout << __func__ << "( " << s->toString() << " )" << std::endl << std::flush;//!!!
}


void StreamEngine::unregisterStream(Stream* s)
{
    //std::cout << __func__ << "( " << s->toString() << " )" << std::endl << std::flush;//!!!
    auto it = std::find(streams.begin(),streams.end(),s);
    if (it != streams.end())
        streams.erase(it);

    // remove any outstanding queued events for this stream
    QueueEntry entry {};
    entry.stream = s;
    updateQueue.remove(entry);
}


void StreamEngine::startStream(Stream* s)
{
    auto it = std::find(streams.begin(),streams.end(),s);
    if (it == streams.end())
        return;
    //std::cout << __func__ << "( " << s->toString() << " )" << std::endl << std::flush;//!!!

    if (s->periodic) {
        // queue first update for 1/freq secs in future
        QueueEntry entry { QueueEntry::Type::UpdateStream, nextPeriodOnPeriodBoundary(s->clockFreq), s };
        #ifdef DEBUG_BUILD
        assert(entry.timestamp > currentTime());
        #endif
        updateQueue.push(entry);
    }
}



uint64_t StreamEngine::updateStreamStates()
{
    if (updateQueue.empty()) return 1;

// auto ct=currentTime();
// std::cout << __func__ << " queue#" << updateQueue.size()
//           << " current:" << ct
//           << " top:" << updateQueue.top().timestamp
//           << std::endl << std::flush;//!!!
// for(auto it=updateQueue.cbegin(); it != updateQueue.cend(); ++it)
//     std::cout << "  " << it->stream->toString() << " " << it->timestamp << " (" << (int64_t(it->timestamp)-int64_t(ct)) << ")" << std::endl << std::flush;
// std::cout << "---" << std::endl << std::flush;

    while(updateQueue.top().timestamp < currentTime()) {
        auto entry { updateQueue.top() };
        updateQueue.pop();

        if (std::find(streams.begin(),streams.end(),entry.stream) != streams.end()) {

            if (entry.type == QueueEntry::Type::UpdateStream) {

                Stream* stream = entry.stream;

                stream->tick();

                // if periodic, schedule the next tick
                if (stream->periodic) {
                    entry.timestamp += uint64_t(1000000000ULL/stream->clockFreq);
                    updateQueue.push(entry);
                }
            }
            else
                throw std::runtime_error("unhandled stream event type");

        }
        else
            throw std::runtime_error("Unknown stream referenced in updateQueue");
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


Stream* roxal::streamVal(double freq, Value initial)
{
    return newObj<Stream>(__func__,freq,initial);
}

Stream* roxal::streamVal(Value initial, Value rest)
{
    return newObj<Stream>(__func__,initial, rest);
}


// stream followed-by (initial ↦ rest) operator
Value roxal::followedBy(Value initial, Value rest)
{
    return objVal(streamVal(initial, rest));
}
