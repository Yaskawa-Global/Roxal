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
             roxal::ObjClosure* closure,
             const ConstArgMap& constArgs,
             const std::vector<ptr<Signal>>& signalArgs);

    virtual Names inputNames() const override { return m_inputNames; }
    virtual Names outputNames() const override { return {}; }
    virtual Values operator()(const Values& inputValues) override { return {}; }

    roxal::ObjClosure* closure;
    ConstArgMap constArgs;
    std::vector<ptr<Signal>> signalArgs;

private:
    Names m_inputNames;
};

}
