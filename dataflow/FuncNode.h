#pragma once

#include "Signal.h"
#include "compiler/Object.h"
#include "compiler/Value.h"

namespace df {

class FuncNode : public std::enable_shared_from_this<FuncNode>
{
public:
    using ConstArgMap = std::map<std::string, roxal::Value>;
    using Names = std::vector<std::string>;
    using Values = std::vector<roxal::Value>;

    FuncNode(const std::string& name,
             roxal::ObjClosure* closure,
             const ConstArgMap& constArgs,
             const std::vector<ptr<Signal>>& signalArgs);

    Names inputNames() const { return m_inputNames; }
    Values operator()(const Values& inputValues);

    roxal::ObjClosure* closure;
    ConstArgMap constArgs;
    std::vector<ptr<Signal>> signalArgs;
    std::string nodeName;

    // parameter names in order of declaration
    std::vector<std::string> paramNames;
    // index of signal argument for each param (-1 if constant)
    std::vector<int> paramSignalIndex;

private:
    Names m_inputNames;
};

}
