#pragma once

#include "Signal.h"
#include <vector>

namespace df {

class DataflowEngine : public std::enable_shared_from_this<DataflowEngine>
{
public:
    static ptr<DataflowEngine> instance();

    void addSignal(ptr<Signal> signal);
    void clear();

private:
    DataflowEngine();

    std::vector<ptr<Signal>> signals;
};

}
