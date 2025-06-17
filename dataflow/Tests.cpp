#include "Tests.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <tuple>
#include <iomanip>
//#include <format>
#include <utility>



using namespace df;


auto engine { DataflowEngine::instance() };


bool df::runTest(const std::string& testName, bool saveNetworkGraph, bool saveSignalGraphs, std::function<bool()> testFunc)
{
    engine->clear();

    int tick = 0;

    // if saveSignalGraphs, save one graph after each tick with signal values included
    engine->addTickCallback([&](ptr<DataflowEngine> engine, TimePoint tickTime) {
        if (saveSignalGraphs) {

            auto formatTick = [](int tick) -> std::string {
                std::ostringstream oss;
                oss << std::setfill('0') << std::setw(6) << tick;
                return oss.str();
            };

            std::string title = testName+" t="+tickTime.humanString();
            std::ofstream graphFile(testName+"_"+ formatTick(tick)+"-"+tickTime.humanString()+".dot");
            graphFile << engine->graphDot(title, engine->signalValues());
            graphFile.close();
        }
        tick++;
    });

    // call test (allow exceptions to escape)
    bool pass = testFunc();

    if (saveNetworkGraph) {
        std::ofstream graphFile(testName+".dot");
        graphFile << engine->graphDot(testName, engine->signalValues());
        graphFile.close();
    }

    return pass;
}


bool df::clockTest1()
{
    auto clockSignal = Signal::newClockSignal(20.0, "clock20Hz");

    std::vector<Value> values;

    // collect signal output values
    clockSignal->addValueChangedCallback(
        [&](TimePoint t, ptr<Signal> s,const Value& v) {
            values.push_back(v);
        }
    );

    engine->runFor(TimeDuration::milliSecs(130)); // time for exactly 3 ticks
    return values == std::vector<Value>{Value(1),Value(2),Value(3)};
}


bool df::executionSeqTest1()
{

    // --clock-->|const1|--const1out(1)-->|add1|--add1Out(2)-->|add1b|--add1bOut(3)--> 3

    auto clockSignal = Signal::newClockSignal(1000.0, "clock");

    auto const1 = Func::newFunc<ConstFunc>("const1",Value(1)); // 1
    auto const1Out = (*const1)(Signals{clockSignal}).at(0);

    auto add1 = Func::newFunc<AddConstFunc>("add1",Value(1)); // +1
    auto add1Out = (*add1)(Signals{const1Out}).at(0);

    auto add1b = Func::newFunc<AddConstFunc>("add1b",Value(1)); // +1
    auto add1bOut = (*add1b)(Signals{add1Out}).at(0); // = 3

    std::vector<Value> outValues;

    // collect signal output values
    //  NB: since callback is only for a change, we only expect one call
    //      when the signal changes from its initial value to the output 3 (1 + 1 + 1)
    //      since const1Out never changes from 1
    add1bOut->addValueChangedCallback(
        [&](TimePoint t, ptr<Signal> s, const Value& v) {
            outValues.push_back(v);
        }
    );

    // ensure the funcs are executed in the correct order
    std::vector<std::string> execFuncNames{};
    auto funcExecuteCallback = [&](TimePoint t, ptr<Func> f,const Values& inputs, const Values& outputs) {
        execFuncNames.push_back(f->name());
    };

    const1->addExecutionCallback(funcExecuteCallback);
    add1->addExecutionCallback(funcExecuteCallback);
    add1b->addExecutionCallback(funcExecuteCallback);

    engine->runFor(TimeDuration::milliSecs(15));
    //std::cout << join(execFuncNames, ", ") << std::endl;

    bool correctValues = (outValues == std::vector<Value>{Value(3)});
    bool correctOrder = execFuncNames.size()>=3 && execFuncNames[0] == "const1" && execFuncNames[1] == "add1" && execFuncNames[2] == "add1b";

    return correctValues && correctOrder;
}



bool df::skipConnectionTest()
{
    // Create a clock signal at 1000 Hz
    auto clockSignal = Signal::newClockSignal(100, "clock");

    // Path 1: Clock -> Add 2 -> Multiply by 2
    auto add2 = Func::newFunc<AddConstFunc>("add2", Value(2));
    auto add2Out = (*add2)(Signals{clockSignal}).at(0)->rename("add2Out");

    auto multiplyBy2 = Func::newFunc<MultiplyConst>("multiplyBy2", Value(2.0));
    auto multipliedSinOut = (*multiplyBy2)(Signals{/*sinOut*/add2Out}).at(0)->rename("mult2Out");;

    // Path 2: Clock -> Add 1 -> Multiply by 3
    auto add1 = Func::newFunc<AddConstFunc>("add1", Value(1));
    auto add1Out = (*add1)(Signals{clockSignal}).at(0)->rename("add1Out");

    auto multiplyBy3 = Func::newFunc<MultiplyConst>("multiplyBy3", Value(3.0));
    auto multipliedAdd1Out = (*multiplyBy3)(Signals{add1Out}).at(0)->rename("mult3Out");

    // Combine the two paths
    auto addResults = Func::newFunc<SumFunc>("addResults",2); // combine the signals
    auto combinedResult = (*addResults)(Signals{multipliedSinOut, multipliedAdd1Out}).at(0)->rename("resultSum");

    // Skip connection: Direct clock to final addition
    auto finalAdd = Func::newFunc<SumFunc>("finalAdd",2); // combine the signals
    auto finalOutput = (*finalAdd)(Signals{combinedResult, clockSignal}).at(0);

    // Collect output values
    std::vector<Value> outValues;
    finalOutput->addValueChangedCallback(
        [&](TimePoint t, ptr<Signal> s, const Value& v) {
            outValues.push_back(v);
        }
    );

    // Track execution order
    std::vector<std::string> execFuncNames;
    auto funcExecuteCallback = [&](TimePoint t, ptr<Func> f, const Values& inputs, const Values& outputs) {
        // std::string inputValues = join(inputs, ", ");
        // std::string outputValues = join(outputs, ", ");
        // std::cout << inputValues << " -> " << f->name() << " -> " << outputValues << std::endl;
        execFuncNames.push_back(f->name());
    };

    // Add execution callbacks to all functions
    add2->addExecutionCallback(funcExecuteCallback);
    multiplyBy2->addExecutionCallback(funcExecuteCallback);
    add1->addExecutionCallback(funcExecuteCallback);
    multiplyBy3->addExecutionCallback(funcExecuteCallback);
    addResults->addExecutionCallback(funcExecuteCallback);
    finalAdd->addExecutionCallback(funcExecuteCallback);

    // Run the engine for a short duration
    engine->runFor(TimeDuration::milliSecs(200));

    // Verify results
    // expected output:
    std::vector<Value> expected {
        Value(13), Value(19), Value(25), Value(31), Value(37), Value(43),
        Value(49), Value(55), Value(61), Value(67), Value(73), Value(79),
        Value(85), Value(91), Value(97), Value(103), Value(109), Value(115),
        Value(121), Value(127), Value(133), Value(139), Value(145), Value(151)
    };
    bool correctValues = !outValues.empty();
    if (correctValues) {
        for(auto i=0; i< std::min(outValues.size(), expected.size()); ++i) {
            if (outValues[i].asInt(false) != expected[i].asInt()) {
                correctValues = false;
                break;
            }
        }
    }

    bool correctOrder = !execFuncNames.empty() && execFuncNames.size() >= 12;
    // several combinations of ordering are correct:
    //   add1 -> add2 -> multiplyBy2 -> multiplyBy3 -> addResults -> finalAdd
    //   add2 -> add1 -> multiplyBy3 -> multiplyBy2 -> addResults -> finalAdd
    //   add2 -> multiplyBy2 -> add1 -> multiplyBy3 -> addResults -> finalAdd
    //   add1 -> multiplyBy3 -> add2 -> multiplyBy2 -> addResults -> finalAdd
    // just check final part
    correctOrder = correctOrder && (execFuncNames.at(4) == "addResults") && (execFuncNames.at(5) == "finalAdd");

    // Print results for debugging
    #if 0
    std::cout << "Execution order: ";
    for (const auto& name : execFuncNames)
        std::cout << name << " ";
    std::cout << std::endl;

    std::cout << "Output values: ";
    for (const auto& value : outValues)
        std::cout << value.asReal() << " ";
    std::cout << std::endl;
    #endif

    return correctValues && correctOrder;
}


bool df::simpleLatencyTest()
{
    // Create a clock signal at 100 Hz
    auto clockSignal = Signal::newClockSignal(100.0, "clock");

    auto add1 = Func::newFunc<AddConstFunc>("add1", Value(1));
    auto add1Out = (*add1)(Signals{clockSignal}).at(0);

    auto add1b = Func::newFunc<AddConstFunc>("add1b", Value(1));
    auto add1bOut = (*add1b)(Signals{add1Out}).at(0);

    auto add1delayed = Func::newFunc<AddConstFunc>("add1delayed", Value(1));
    auto add1delayedOut = (*add1delayed)(Signals{add1Out},Func::ParamMap{{"output[-1]","input"}}).at(0);

    auto finalAdd = Func::newFunc<SumFunc>("finalAdd",2); // combine the signals
    auto finalOutput = (*finalAdd)(Signals{add1bOut, add1delayedOut}).at(0);

    // Collect output values
    std::vector<Value> outValues;
    finalOutput->addValueChangedCallback(
        [&](TimePoint t, ptr<Signal> s, const Value& v) {
            outValues.push_back(v);
        }
    );

    // Track execution order
    std::vector<std::string> execFuncNames;
    auto funcExecuteCallback = [&](TimePoint t, ptr<Func> f, const Values& inputs, const Values& outputs) {
        // std::string inputValues = join(inputs, ", ");
        // std::string outputValues = join(outputs, ", ");
        // std::cout << inputValues << " -> " << f->name() << " -> " << outputValues << std::endl;
        execFuncNames.push_back(f->name());
    };

    // Add execution callbacks to all functions
    add1->addExecutionCallback(funcExecuteCallback);
    add1b->addExecutionCallback(funcExecuteCallback);
    add1delayed->addExecutionCallback(funcExecuteCallback);
    finalAdd->addExecutionCallback(funcExecuteCallback);

    engine->runFor(TimeDuration::milliSecs(100));

    // Verify results
    // expected output:
    std::vector<Value> expected {
        Value(7), Value(9), Value(11), Value(13), Value(15), Value(17), Value(19), Value(21)
    };
    bool correctValues = !outValues.empty();
    if (correctValues) {
        for(auto i=0; i< std::min(outValues.size(), expected.size()); ++i) {
            if (outValues[i].asInt(false) != expected[i].asInt()) {
                correctValues = false;
                break;
            }
        }
    }

    auto last = execFuncNames.size()-1;
    bool correctOrder = !execFuncNames.empty() && last > 10;
    correctOrder = correctOrder
                && (  execFuncNames.at(last-3) == "add1" )
                && ( (execFuncNames.at(last-2) == "add1delayed") || (execFuncNames.at(last-2) == "add1b"))
                && ( (execFuncNames.at(last-1) == "add1b") || (execFuncNames.at(last-1) == "add1delayed"))
                && (  execFuncNames.at(last) == "finalAdd" );


    // Print results for debugging
    #if 0
    std::cout << "Execution order: ";
    for (const auto& name : execFuncNames)
        std::cout << name << " ";
    std::cout << std::endl;

    std::cout << "Output values: ";
    for (const auto& value : outValues)
        std::cout << value.asInt() << " ";
    std::cout << std::endl;
    #endif

    return correctValues && correctOrder;
}


bool df::feedbackTest()
{
    // Create a clock signal at 100 Hz
    auto clockSignal = Signal::newClockSignal(100.0, "clock");
    auto add1 = Func::newFunc<AddConstFunc>("add1", Value(1));
    auto add1Out = (*add1)(Signals{clockSignal}).at(0)->rename("add1Out");

    auto sum1 = Func::newFunc<SumFunc>("sum1",2);
    auto sum2 = Func::newFunc<SumFunc>("sum2",2); // combine the signals

    auto sum1Out = (*sum1)(Signals{add1Out, sum2->outputs().at(0)},Func::ParamMap{{"add1Out","input0"},{"sum[-1]","input1"}}).at(0)->rename("sum1Out");
    sum1->setInputDefault("input1", Value(0)); // default for input1 since value not initially available due to feedback loop

    auto sum2Out = (*sum2)(Signals{add1Out, sum1Out}).at(0)->rename("sum2Out");

    auto add2 = Func::newFunc<AddConstFunc>("add2", Value(1));
    auto add2Out = (*add2)(Signals{sum2Out}).at(0)->rename("add2Out");

    // Collect output values
    std::vector<Value> outValues;
    add2Out->addValueChangedCallback(
        [&](TimePoint t, ptr<Signal> s, const Value& v) {
            outValues.push_back(v);
        }
    );

    // Track execution order
    std::vector<std::string> execFuncNames;
    auto funcExecuteCallback = [&](TimePoint t, ptr<Func> f, const Values& inputs, const Values& outputs) {
        // std::string inputValues = join(inputs, ", ");
        // std::string outputValues = join(outputs, ", ");
        // std::cout << inputValues << " -> " << f->name() << " -> " << outputValues << std::endl;
        execFuncNames.push_back(f->name());
    };

    // Add execution callbacks to all functions
    add1->addExecutionCallback(funcExecuteCallback);
    sum1->addExecutionCallback(funcExecuteCallback);
    add2->addExecutionCallback(funcExecuteCallback);
    sum2->addExecutionCallback(funcExecuteCallback);

    engine->runFor(TimeDuration::milliSecs(100));

    // Verify results
    // expected output:
    std::vector<Value> expected {
        Value(5), Value(11), Value(19), Value(29), Value(41), Value(55), Value(71)
    };
    bool correctValues = !outValues.empty();
    if (correctValues) {
        for(auto i=0; i< std::min(outValues.size(), expected.size()); ++i) {
            if (outValues[i].asInt(false) != expected[i].asInt()) {
                correctValues = false;
                break;
            }
        }
    }

    bool correctOrder = !execFuncNames.empty() && execFuncNames.size() >= 8;
    execFuncNames.resize(8);
    correctOrder = correctOrder
        && (execFuncNames == std::vector<std::string>{"add1","sum1","sum2","add2","add1","sum1","sum2","add2"});


    // Print results for debugging
    #if 0
    std::cout << "Execution order: ";
    for (const auto& name : execFuncNames)
        std::cout << name << " ";
    std::cout << std::endl;

    std::cout << "Output values: ";
    for (const auto& value : outValues)
        std::cout << value.asInt() << " ";
    std::cout << std::endl;
    #endif

    return correctValues && correctOrder;
}


bool df::restartEngineTest()
{
    // Create a clock signal at 100 Hz
    auto clockSignal = Signal::newClockSignal(100.0, "clock");
    auto add1 = Func::newFunc<AddConstFunc>("add1", Value(1));
    auto add1Out = (*add1)(Signals{clockSignal}).at(0)->rename("add1Out");

    engine->runFor(TimeDuration::milliSecs(100));

    auto output1Value = add1Out->lastValue();

    engine->runFor(TimeDuration::milliSecs(100));

    auto output2Value = add1Out->lastValue();

    return output2Value.asInt() > output1Value.asInt();
}



bool df::UserTickTest()
{
    auto clockSignal = Signal::newClockSignal(100.0, "clock");  // 100Hz
    auto clockSignal2 = Signal::newClockSignal(50.0, "clock2"); // 50Hz

    engine->tick();
    engine->tick();

    if (clockSignal->lastValue().asInt() != 2)
        return false;
    if (clockSignal2->lastValue().asInt() != 1)
        return false;

    engine->tick();
    engine->tick();

    if (clockSignal->lastValue().asInt() != 4)
        return false;
    if (clockSignal2->lastValue().asInt() != 2)
        return false;

    if (engine->tickPeriod().frequency() != 100.0)
        return false;

    return true;
}



bool df::testSplitJoin()
{
    auto clockSignal = Signal::newClockSignal(100.0, "clock");  // 100Hz

    auto const1 = Func::newFunc<ConstFunc>("const1", df::doubleVector({1.0,2.0,3.0,4.0}));
    auto const1Out = (*const1)(Signals{clockSignal}).at(0);

    auto split = Func::newFunc<Split>("split",4);
    auto splitOut = (*split)(Signals{const1Out}); // 4 signals

    auto join = Func::newFunc<Join>("join",4);
    auto joinOut = (*join)(splitOut).at(0);

    engine->tick();

    auto joinOutVal = joinOut->lastValue();

    bool correctValue = df::equals(joinOutVal, df::doubleVector({1.0,2.0,3.0,4.0}));

    return correctValue;
}



bool df::testVectorOperations()
{
    auto clockSignal = Signal::newClockSignal(100.0, "clock");  // 100Hz

    auto const1 = Func::newFunc<ConstFunc>("const1", df::doubleVector({1.1,2.2,3.3,4.4}));
    auto const1Out = (*const1)(Signals{clockSignal}).at(0);

    auto const2 = Func::newFunc<ConstFunc>("const2", df::doubleVector({4.4,3.3,2.2,1.1}));
    auto const2Out = (*const2)(Signals{clockSignal}).at(0);

    auto add = Func::newFunc<Add>("add");
    auto addOut = (*add)(Signals{const1Out,const2Out}).at(0);

    auto sub = Func::newFunc<Subtract>("sub");
    auto subOut = (*sub)(Signals{const1Out,const2Out}).at(0);

    auto sconst = Func::newFunc<ConstFunc>("sconst", Value(2.0));
    auto sconstOut = (*sconst)(Signals{clockSignal}).at(0);

    auto scalarMulit = Func::newFunc<Multiply>("scalarMult");
    auto scalarMulitOut = (*scalarMulit)(Signals{sconstOut,const1Out}).at(0);

    engine->tick();

    auto addOutVal = addOut->lastValue();
    auto subOutVal = subOut->lastValue();
    auto scalarMulitOutVal = scalarMulitOut->lastValue();

    bool correctAddValue = df::equals(addOutVal, df::doubleVector({5.5,5.5,5.5,5.5}));
    bool correctSubValue = df::equals(subOutVal, df::doubleVector({-3.3,-1.1,1.1,3.3}));
    bool correctMultValue = df::equals(scalarMulitOutVal, df::doubleVector({2.2,4.4,6.6,8.8}));

    return correctAddValue && correctSubValue && correctMultValue;
}






std::vector<std::tuple<std::string, bool, std::string>> df::runTests(bool saveNetworkGraphs, bool saveSignalGraphs)
{
    std::vector<std::tuple<std::string, bool, std::string>> results {};

    std::vector<std::pair<std::string,std::function<bool()>>> tests {
        {"clockTest1", df::clockTest1},
        {"executionSeqTest1",df::executionSeqTest1},
        {"skipConnectionTest",df::skipConnectionTest},
        {"simpleLatencyTest",df::simpleLatencyTest},
        {"feedbackTest",df::feedbackTest},
        {"restartEngineTest",df::restartEngineTest},
        {"UserTickTest",df::UserTickTest},
        {"testSplitJoin",df::testSplitJoin},
        {"testVectorOperations",df::testVectorOperations},
    };


    for(const auto& [name, test] : tests) {
        try {
            if (df::runTest(name, saveNetworkGraphs, saveSignalGraphs, test))
                results.push_back({name,true,"ok"});
            else
                results.push_back({name,false,"failed"});
        } catch (std::exception& e) {
            results.push_back({name,false, std::string("failed with exception: ") + e.what()});
        }
    }
    return results;
}
