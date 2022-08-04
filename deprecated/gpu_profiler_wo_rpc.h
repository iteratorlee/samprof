#define UNW_LOCAL_ONLY
#include <map>
#include <queue>
#include <mutex>
#include <stack>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <unordered_set>

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <cxxabi.h>
#include <pthread.h>
#include <inttypes.h>
#include <libunwind.h>
#include <sys/types.h>

#include <Python.h>
#include <frameobject.h>

#include "cuda.h"
#include "cupti.h"
#include <cupti_pcsampling.h>
#include <cupti_pcsampling_util.h>

#include "utils.h"
#include "calling_ctx_tree.h"

#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif

using namespace CUPTI::PcSamplingUtil;

#define DEBUG true
#define DEBUG_LOG_LENGTH 4096

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

#define THREAD_SLEEP_TIME 100 // in ms
#define FUNC_NAME_LENGTH 4096

typedef struct contextInfo
{
    uint32_t contextUid;
    CUpti_PCSamplingData pcSamplingData;
    std::vector<CUpti_PCSamplingConfigurationInfo> pcSamplingConfigurationInfo;
    PcSamplingStallReasons pcSamplingStallReasons;
} ContextInfo;

typedef struct unwvalue {
    unw_word_t pc;
    unw_word_t offset;
    std::string funcName;
    std::string fileName;
    CCTNodeType nodeType;

    unwvalue() {};

    unwvalue(unw_word_t _pc, unw_word_t _offset, std::string _funcName):
        pc(_pc), offset(_offset), funcName(_funcName), nodeType(CCTNODE_TYPE_CXX) {};

    unwvalue(std::string _fileName, std::string _funcName, unw_word_t _offset):
        fileName(_fileName), funcName(_funcName), offset(_offset), nodeType(CCTNODE_TYPE_PY) {};
} UNWValue;

typedef enum {
    CALL_STACK_HAS_PY = 1,
    CALL_STACK_NOT_HAS_PY = 2
} CallStackStatus;

// For multi-gpu we are preallocating buffers only for first context creation,
// so preallocated buffer stall reason size will be equal to max stall reason for first context GPU
size_t stallReasonsCount = 0;
// consider firstly queried stall reason count using cuptiPCSamplingGetNumStallReasons() to allocate memory for circular buffers.
bool g_collectedStallReasonsCount = false;
std::mutex g_stallReasonsCountMutex;

// Variables related to circular buffer.
std::vector<CUpti_PCSamplingData> g_circularBuffer;
int g_put = 0;
int g_get = 0;
std::vector<bool> g_bufferEmptyTrackerArray; // true - used, false - free.
std::mutex g_circularBufferMutex;
bool g_buffersGetUtilisedFasterThanStore = false;
bool g_allocatedCircularBuffers = false;

// Variables related to context info book keeping.
std::map<CUcontext, ContextInfo*> g_contextInfoMap;
std::recursive_mutex g_contextInfoMutex;
std::vector<ContextInfo*> g_contextInfoToFreeInEndVector;

// Variables related to global queuing
std::queue<std::pair<CUpti_PCSamplingData*, ContextInfo*>> g_pcSampDataQueue;
std::recursive_mutex g_pcSampDataQueueMutex;

// Variables related to start/stop sampling
std::thread g_rpcServerThreadHandle;
std::thread g_signalListeningThreadHandle;
bool g_pcSamplingStarted = false;
std::recursive_mutex g_stopSamplingMutex;
pid_t g_mainThreadPid;
pthread_t g_mainThreadTid;
pid_t g_rpcServerThreadPid;
// recording all the threads launching cuda kernels
std::unordered_set<pthread_t> g_kernelThreadTids;
std::unordered_map<pthread_t, bool> g_kernelThreadSyncedMap;
bool g_kernelThreadSynced = false;
// id of selected thread to execute cuptiStart/StopPCSampling
pthread_t selectedTid;

// For testing overhead
uint64_t g_samplingDuration = 2000;
// continuous sampling with a fixed duration
uint64_t g_nativeLatency = 0;

// Variables related to pro-active profiling
bool g_noRPC;
std::vector<CUpti_PCSamplingData*> g_pcSampleVector;
std::thread g_copyPCSamplesThreadHandle;
std::string g_dumpFileName;

// Variables related to cpu cct
typedef std::unordered_map<pthread_t, CPUCCT*> CCTMAP_t;
CCTMAP_t g_CPUCCTMap;
std::mutex g_cpuCallingCtxTreeMutex;
unw_word_t g_activeCPUPCID;
std::recursive_mutex g_activeCPUPCIDMutex;
std::unordered_map<CUpti_PCSamplingPCData*, unw_word_t> g_GPUPCSamplesParentCPUPCIDs;
std::mutex g_GPUPCSamplesParentCPUPCIDsMutex;
uint64_t g_CPUCCTNodeId = 1;
std::mutex g_CPUCCTNodeIdMutex;
bool g_pruneCCT = true;
std::unordered_map<uint64_t, uint64_t> g_esp2pcIdMap;
bool g_checkRSP = true;
std::stack<UNWValue> g_callStack;
bool g_genCallStack = false;
bool g_backTraceVerbose = false;

// Variables related to initialize injection once.
bool g_initializedInjection = false;
std::mutex g_initializeInjectionMutex;

// cupti args
CUpti_PCSamplingCollectionMode g_pcSamplingCollectionMode = CUPTI_PC_SAMPLING_COLLECTION_MODE_CONTINUOUS;
uint32_t g_samplingPeriod = 0;
size_t g_scratchBufSize = 0;
size_t g_hwBufSize = 0;
size_t g_pcConfigBufRecordCount = 1000;
size_t g_circularbufCount = 10;
size_t g_circularbufSize = 500;
bool g_verbose = true;
CUpti_SubscriberHandle subscriber;

// return pc samples only
bool g_fakeBT = false;
bool g_DoCPUCallstackUnwinding = true;

// variables related to python back tracing
std::string g_backEnd = "TORCH"; // TF or TORCH
// std::queue<UNWValue> g_pyCallPathQueue;
// std::mutex g_pyBackTraceMutex;
std::string g_pyFileName = "main.py";

// for debug
bool g_syncBeforeStart = false;
