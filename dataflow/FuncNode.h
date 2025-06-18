#pragma once

#include "Func.h"
#include "compiler/Object.h"
#include "compiler/Value.h"

namespace df {

class FuncNode : public Func
{
public:
    using ConstArgMap = std::map<std::string, roxal::Value>;

    FuncNode(const std::string& name,
             const roxal::Value& closure,
             const ConstArgMap& constArgs,
             const std::vector<ptr<Signal>>& signalArgs);

    virtual Names inputNames() const override { return m_inputNames; }
    virtual Names outputNames() const override { return {}; }
    virtual Values operator()(const Values& inputValues) override;

    roxal::Value closure;
    ConstArgMap constArgs;
    std::vector<ptr<Signal>> signalArgs;

    // parameter names in order of declaration
    std::vector<std::string> paramNames;
    // index of signal argument for each param (-1 if constant)
    std::vector<int> paramSignalIndex;

private:
    Names m_inputNames;
};

}
