#pragma once

#include <string>
#include <complex>

#include "DataflowEngine.h"

namespace df {


// output constant values (ignore input signals)
class ConstFunc : public Func {
public:
    ConstFunc(const std::string& name, Value c) : m_c(c), Func(name) {}

    virtual Names inputNames() const { return {"clock" }; };
    virtual Names outputNames() const { return {"const"}; };

    using Func::operator();

    virtual Values operator()(const Values& inputValues) override {
        return {m_c};
    }

    bool isPure() const override { return true; }

protected:
    Value m_c;
};


// assumed single Value (int or real)
class AddConstFunc : public Func {
public:
    AddConstFunc(const std::string& name, Value v) : m_v(v), Func(name) {}

    virtual Names inputNames() const { return {"input" }; };
    virtual Names outputNames() const { return {"output"}; };

    using Func::operator();

    Values operator()(const Values& inputValues) override {
        auto inValue = inputValues[0];
        if (inValue.isNil())
            throw std::runtime_error("AddConstFunc: input value is nil");
        if (inValue.isInt()) {
            auto inInt = inputValues[0].asInt();
            return {Value(inInt+m_v.asInt())};
        }
        // assume real
        auto inReal = inputValues[0].asReal();
        return {Value(inReal+m_v.asReal())};
    }

    bool isPure() const override { return true; }

protected:
    Value m_v;
};


// assumes single real Value
class MultiplyConst : public Func {
public:
    MultiplyConst(const std::string& name, Value v) : m_v(v), Func(name) {}

    virtual Names inputNames() const { return {"input" }; };
    virtual Names outputNames() const { return {"output"}; };

    using Func::operator();

    Values operator()(const Values& inputValues) override {
        if (inputValues[0].isNil())
            throw std::runtime_error("MultiplyConst: input value is nil");
        auto product = inputValues[0].asReal() * m_v.asReal();
        return {Value(product)};
    }

    bool isPure() const override { return true; }

protected:
    Value m_v;
};


class SumFunc : public Func {
public:
    SumFunc(const std::string& name, int numInputs) : Func(name), m_numInputs(numInputs) {}

    using Func::operator();

    virtual Names inputNames() const {
        Names names;
        for (int i = 0; i < m_numInputs; i++)
            names.push_back("input" + std::to_string(i));
        return names;
    };
    virtual Names outputNames() const { return {"sum"}; };

    Values operator()(const Values& inputValues) override {
        if (inputValues.empty()) return {Value(0)};

        for(auto i=0; i<inputValues.size(); i++) {
            if (inputValues.at(i).isNil())
                throw std::runtime_error("SumFunc: input "+inputNames().at(i)+" value is nil");
        }

        if (inputValues.at(0).isInt()) {
            int32_t sum = 0;
            for (const auto& val : inputValues) sum += val.asInt();
            return {Value(sum)};
        }
        else if (inputValues.at(0).isReal()) {
            double sum = 0;
            for (const auto& val : inputValues) sum += val.asReal();
            return {Value(sum)};
        }
        return {Value(0)};
    }

    bool isPure() const override { return true; }

protected:
    int m_numInputs;
};



class SinFunc : public Func {
public:
    SinFunc(const std::string& name) : Func(name) {}

    virtual Names inputNames() const { return {"x" }; };
    virtual Names outputNames() const { return {"sinx"}; };

    using Func::operator();

    Values operator()(const Values& inputValues) override {
        if (inputValues[0].isNil())
            throw std::runtime_error("MultiplyConst: input value is nil");
        auto inValue = inputValues[0].asReal();
        return {Value(std::sin(inValue))};
    }

    bool isPure() const override { return true; }
};


bool runTest(const std::string& testName, bool saveNetworkGraph, bool saveSignalGraphs, std::function<bool()> testFunc);


bool clockTest1();
bool executionSeqTest1();
bool skipConnectionTest();
bool simpleLatencyTest();
bool feedbackTest();
bool restartEngineTest();
bool UserTickTest();
bool testSplitJoin();
bool testVectorOperations();


// run tests and return pairs of test name and pass result with message
std::vector<std::tuple<std::string, bool, std::string>> runTests(bool saveNetworkGraphs=false, bool saveSignalGraphs=false);


}
