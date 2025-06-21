#include <iostream>
#include <thread>
#include <complex>
#include <fstream>

#include "Value.h"
#include "Signal.h"
#include "Func.h"
#include "DataflowEngine.h"

#include "Tests.h"


int main()
{
    auto results = df::runTests(/*saveNetworkGraphs=*/false, /*saveSignalGraphs=*/false);

    int passes = 0;
    int fails = 0;
    for(const auto& result : results) {
        std::cout << "Test: " << std::get<0>(result) << " ";
        bool passed = std::get<1>(result);
        if (passed) {
            std::cout << "passed";
            passes++;
        }
        else {
            std::cout << "failed";
            fails++;
        }
        std::cout << " " << std::get<2>(result) << std::endl;
    }

    std::cout << "Passed " << passes << " failed " << fails << std::endl;

    return 0;
}
