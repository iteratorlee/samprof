#include "common.h"
#include "back_tracer.h"
#include "cpu_sampler.h"

bool verbose = true;

void TestProfilerConf() {
    auto profilerConf = GetProfilerConf();
    profilerConf->PrintProfilerConf();
}

void barFunc() {
    auto backTracer = GetBackTracer();
    backTracer->DoBackTrace(verbose);
}

void barFuncV2() {
    auto backTracer = GetBackTracer();
    backTracer->DoBackTrace(verbose);
}

void fooFunc() {
    barFunc();
}

void TestBackTracerOverheadS1() {
    std::cout << "********** TestBackTracerOverheadS1 **********" << std::endl;
    fooFunc();
}

void TestBackTracerRecursive(int depth) {
    if (depth <= 0) {
        auto backTracer = GetBackTracer();
        backTracer->DoBackTrace(verbose);
    } else {
        TestBackTracerRecursive(depth - 1);
    }
}

void TestBackTracerOverheadR1(int depth) {
    std::cout << "********** TestBackTracerOverheadR1 **********" << std::endl;
    auto timer = Timer::GetGlobalTimer("test_back_tracer_overhead");
    timer->start();
    TestBackTracerRecursive(depth);
    timer->stop();
    std::cout << "overhead of simple sample: " << timer->getAccumulatedTime() << std::endl;
    timer->reset();
}
namespace TestA {
namespace TestA1 {

class A {
public:
    void foo(int depth, int i1, int i2, int i3, float f1, float f2, float f3) {
        if (depth <= 0) {
            GetBackTracer()->DoBackTrace(verbose);
        } else {
            bar(depth - 1, i1 + 1, i2 + 1, i3 + 1, f1 + 1, f2 + 1, f3 + 1);
        }
    }

    void bar(int depth, int i1, int i2, int i3, float f1, float f2, float f3) {
        if (depth <= 0) {
            GetBackTracer()->DoBackTrace(verbose);
        } else {
            foo(depth - 1, i1 + 1, i2 + 1, i3 + 1, f1 + 1, f2 + 1, f3 + 1);
        }
    }
};

}
}

void TestBackTracerOverheadR2(int depth) {
    std::cout << "********** TestBackTracerOverheadR2 **********" << std::endl;
    auto timer = Timer::GetGlobalTimer("test_BT_overhead_complex");
    timer->start();
    auto a = new TestA::TestA1::A();
    a->foo(depth, 1, 2, 3, 4.0, 5.0, 6.0);
    timer->stop();
    std::cout << "overhead of complex sample: " << timer->getAccumulatedTime() << std::endl;
}

void TestCppStackPointer() {
    std::cout << "********** TestCppStackPointer **********" << std::endl;
    barFunc();
    barFuncV2();
    barFunc();
}

int main(int argc, char** argv) {
    if (argc > 1) verbose = std::atoi(argv[2]);
    TestProfilerConf();
    TestBackTracerOverheadS1();
    TestBackTracerOverheadR1(std::atoi(argv[1]));
    TestBackTracerOverheadR2(std::atoi(argv[1]));
    TestCppStackPointer();
    return 0;
}
