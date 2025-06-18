#include "DataflowEngine.h"

using namespace df;

ptr<DataflowEngine> DataflowEngine::instance()
{
    static ptr<DataflowEngine> inst = std::shared_ptr<DataflowEngine>(new DataflowEngine());
    return inst;
}

DataflowEngine::DataflowEngine()
{
}

void DataflowEngine::addSignal(ptr<Signal> signal)
{
    signals.push_back(signal);
}

void DataflowEngine::clear()
{
    signals.clear();
}
