#include "FuncNode.h"

#include "core/common.h"
#include <algorithm>
#include "compiler/VM.h"

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
            size_t sigIndex = 0;
            for (const auto& param : funcType.params) {
                std::string pname;
                if (param.has_value())
                    pname = roxal::toUTF8StdString(param->name);
                else
                    pname = std::to_string(paramNames.size());
                m_inputNames.push_back(pname);
                paramNames.push_back(pname);

                auto it = constArgs.find(pname);
                if (it == constArgs.end()) {
                    if (sigIndex < signalArgs.size()) {
                        addInput(pname, signalArgs[sigIndex]);
                        paramSignalIndex.push_back(int(sigIndex));
                        sigIndex++;
                    } else {
                        paramSignalIndex.push_back(-1);
                    }
                } else {
                    paramSignalIndex.push_back(-1);
                }
            }
            // compute max input frequency
            double maxFreq = 0.0;
            for (auto& sig : signalArgs)
                maxFreq = std::max(maxFreq, sig->frequency());

            if (maxFreq <= 0.0)
                maxFreq = 1.0;

            createOutputSignals(maxFreq);
            m_operatorSignalsCalled = true;
        }
    }
}

Values FuncNode::operator()(const Values& inputValues)
{
    using namespace roxal;

    auto& vm = VM::instance();

    std::vector<Value> args;
    size_t sigIdx = 0;
    for (const auto& pname : paramNames) {
        auto cit = constArgs.find(pname);
        if (cit != constArgs.end()) {
            args.push_back(cit->second);
        } else {
            if (sigIdx < inputValues.size())
                args.push_back(inputValues[sigIdx++]);
            else
                args.push_back(nilVal());
        }
    }

    auto result = vm.callAndExec(closure, args);

    return { result.second };
}

