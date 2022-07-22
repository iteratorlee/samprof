#ifndef __COMMON_INCLUDED__
#define __COMMON_INCLUDED__
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>

#include <stack>
#include <queue>
#include <string>
#include <thread>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif

#define DEBUG true
#define DEBUG_LOG_LENGTH 4096

#define min2(a, b) ((a) < (b) ? (a) : (b))

#define DEBUG_LOG(format, args...)                                          \
do {                                                                        \
    if (DEBUG) {                                                            \
        printf("[DEBUG LOG] " format, ##args);                              \
    }                                                                       \
} while (0)

#define CUPTI_CALL(call)                                                    \
do {                                                                        \
 CUptiResult _status = call;                                                \
 if (_status != CUPTI_SUCCESS)                                              \
    {                                                                       \
     const char* errstr;                                                    \
     cuptiGetResultString(_status, &errstr);                                \
     fprintf(stderr, "%s:%d: error: function %s failed with error %s.\n",   \
             __FILE__,                                                      \
             __LINE__,                                                      \
             #call,                                                         \
             errstr);                                                       \
     exit(-1);                                                              \
    }                                                                       \
} while (0)

#define MEMORY_ALLOCATION_CALL(var)                                             \
do {                                                                            \
    if (var == NULL) {                                                          \
        fprintf(stderr, "%s:%d: Error: Memory Allocation Failed \n",            \
                __FILE__, __LINE__);                                            \
        exit(-1);                                                               \
    }                                                                           \
} while (0)

#define TOP2(s1, s2, val)                                       \
do {                                                            \
    if (s1.size()) val = s1.top();                              \
    else val = s2.top();                                        \
} while (0)

#define POP2(s1, s2)                                            \
do {                                                            \
    if (s1.size()) s1.pop();                                    \
    else s2.pop();                                              \
} while (0)

#define THREAD_SLEEP_TIME 100 // in ms
#define FUNC_NAME_LENGTH 4096

class ProfilerConf {
public:
    ProfilerConf() {
        ReadEnvVars();
    }

    // gpu pc sampling configurations
    uint32_t samplingPeriod = 0;
    size_t scratchBufSize = 0;
    size_t hwBufSize = 0;
    size_t pcConfigBufRecordCount = 1000;
    size_t circularbufCount = 10;
    size_t circularbufSize = 500;

    // cpu sampling configurations
    uint64_t cpuSamplingPeriod = 1000;
    uint64_t cpuSamplingPages = 128;
    int32_t cpuSamplingTimeout = -1;
    uint64_t cpuSamplingMaxDepth = 256;

    // event-driven cpu cct contruction configurations
    bool fakeBT = false;
    bool doCPUCallStackUnwinding = true;
    bool pruneCCT = true;
    bool checkRSP = true;
    bool syncBeforeStart = false;
    bool backTraceVerbose = false;
    bool doPyUnwinding = false;
    bool noRPC = false;
    bool noSampling = false;
    
    std::string backEnd = "TORCH";
    std::string pyFileName = "main.py";
    std::string dumpFileName = "profiling_response.pb.gz";

    pthread_t mainThreadTid;

    void PrintProfilerConf() {
        std::cout << std::endl;
        std::cout << "============ Configuration Details : ============" << std::endl;
        std::cout << "gpu pc sampling period       : " << samplingPeriod << std::endl;
        std::cout << "scratch buffer size          : " << scratchBufSize << std::endl;
        std::cout << "hw buffer size               : " << hwBufSize << std::endl;
        std::cout << "configuration buffer size    : " << pcConfigBufRecordCount << std::endl;
        std::cout << "circular buffer count        : " << circularbufCount << std::endl;
        std::cout << "circular buffer record count : " << circularbufSize << std::endl;

        std::cout << "cpu pc sampling period       : " << cpuSamplingPeriod << std::endl;
        std::cout << "cpu pc sampling buffer pages : " << cpuSamplingPages << std::endl;
        std::cout << "cpu pc sampling timeout      : " << cpuSamplingTimeout << std::endl;
        std::cout << "cpu pc sampling max depth    : " << cpuSamplingMaxDepth << std::endl;

        std::cout << "fake CCT                     : " << fakeBT << std::endl;
        std::cout << "do CPU call stack unwinding  : " << doCPUCallStackUnwinding << std::endl;
        std::cout << "check rsp                    : " << checkRSP << std::endl;
        std::cout << "prune cct                    : " << pruneCCT << std::endl;
        std::cout << "sync before start/stop       : " << syncBeforeStart << std::endl;
        std::cout << "backtrace verbose            : " << backTraceVerbose << std::endl;
        std::cout << "do py unwinding              : " << doPyUnwinding << std::endl;
        std::cout << "no RPC                       : " << noRPC << std::endl;
        std::cout << "no Sampling                  : " << noSampling << std::endl;

        std::cout << "dl backend                   : " << backEnd << std::endl;
        std::cout << "python file name             : " << pyFileName << std::endl;
        if (noRPC) {
            std::cout << "dump file name (no)          : " << dumpFileName << std::endl;
        }

        std::cout << "main thread tid              : " << mainThreadTid << std::endl;
        std::cout << "=================================================" << std::endl;
        std::cout << std::endl;
    }


private:
    void ReadEnvVars() {
        char* s;
        if ((s = getenv("CUPTI_SAMPLING_PERIOD")) != nullptr) {
            samplingPeriod = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("CUPTI_BUF_SIZE")) != nullptr) {
            scratchBufSize = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("CUPTI_HWBUF_SIZE")) != nullptr) {
            hwBufSize = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("CUPTI_PC_CONFIG_BUF_RECORD_COUNT")) != nullptr) {
            pcConfigBufRecordCount = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("CUPTI_CIRCULAR_BUF_COUNT")) != nullptr) {
            circularbufCount = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("CUPTI_CIRCULAR_BUF_SIZE")) != nullptr) {
            circularbufSize = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("RETURN_CUDA_PC_SAMPLE_ONLY")) != nullptr) {
            fakeBT = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("DO_CPU_CALL_STACK_UNWINDING")) != nullptr) {
            doCPUCallStackUnwinding = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("PRUNE_CCT")) != nullptr) {
            pruneCCT = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("DL_BACKEND")) != nullptr) {
            backEnd = s;
        }
        if ((s = getenv("CHECK_RSP")) != nullptr) {
            checkRSP = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("SYNC_BEFORE_START")) != nullptr) {
            syncBeforeStart = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("BT_VERBOSE")) != nullptr) {
            backTraceVerbose = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("PY_FILENAME")) != nullptr) {
            pyFileName = s;
        }
        if (backEnd == "TORCH") {
            // TODO: how to decide py unwinding?
            doPyUnwinding = true;
        }
        if ((s = getenv("NO_RPC")) != nullptr) {
            noRPC = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("DUMP_FN")) != nullptr) {
            dumpFileName = s;
        }
        if ((s = getenv("NO_SAMPLING")) != nullptr) {
            noSampling = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("CPU_SAMPLING_PERIOD")) != nullptr) {
            cpuSamplingPeriod = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("CPU_SAMPLING_BUFFER_PAGES")) != nullptr) {
            cpuSamplingPages = std::strtoul(s, nullptr, 10);
        }
        if ((s = getenv("CPU_SAMPLING_TIMEOUT")) != nullptr) {
            cpuSamplingTimeout = std::strtol(s, nullptr, 10);
        }
        if ((s = getenv("CPU_SAMPLING_MAX_DEPTH")) != nullptr) {
            cpuSamplingMaxDepth = std::strtoul(s, nullptr, 10);
        }
    }
};

ProfilerConf* GetProfilerConf();
#endif