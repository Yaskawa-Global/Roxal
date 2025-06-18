#include "FuncNode.h"

#include "core/common.h"

using namespace df;

FuncNode::FuncNode(const std::string& name,
                   roxal::ObjClosure* closure_,
                   const ConstArgMap& constArgs_,
                   const std::vector<ptr<Signal>>& signalArgs_)
  : Func(name), closure(closure_), constArgs(constArgs_), signalArgs(signalArgs_)
{
    if (closure && closure->function->funcType.has_value()) {
        auto funcTypePtr = closure->function->funcType.value();
        if (funcTypePtr->func.has_value()) {
            const auto& funcType = funcTypePtr->func.value();
            for (const auto& param : funcType.params) {
                if (param.has_value())
                    m_inputNames.push_back(roxal::toUTF8StdString(param->name));
                else
                    m_inputNames.push_back("");
            }
        }
    }
}

