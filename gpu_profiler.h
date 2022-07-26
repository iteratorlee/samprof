#define UNW_LOCAL_ONLY
#include <map>
#include <queue>
#include <mutex>
#include <stack>
#include <regex>
#include <memory>
#include <string>
#include <vector>
#include <thread>
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

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "utils.h"
#include "cpu_sampler.h"
#include "tools/tools.h"
#include "calling_ctx_tree.h"
#include "./cpp-gen/gpu_profiling.grpc.pb.h"

using namespace CUPTI::PcSamplingUtil;
using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerCompletionQueue;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using gpuprofiling::GPUProfilingRequest;
using gpuprofiling::GPUProfilingResponse;
using gpuprofiling::GPUProfilingService;
using gpuprofiling::CPUCallingContextTree;
using gpuprofiling::CPUCallingContextNode;

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

typedef enum {
    // OP nodes
    CRITICAL_TYPE_TORCH_OP = 1,
    CRITICAL_TYPE_TF_OP = 2,
    // Leaf nodes
    CRITICAL_TYPE_LEAF = 3,
    // Py nodes
    CRITICAL_TYPE_PY_FORWARD = 4,
    CRITICAL_TYPE_PY_BACKWARD = 5,
    CRITICAL_TYPE_PY_LOSS = 6,
    // Not critical
    NOT_CRITICAL_NODE = 0x7fffffff
} CriticalNodeType;

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
bool g_pcSamplingStarted = false;
std::recursive_mutex g_stopSamplingMutex;
pid_t g_rpcServerThreadPid;
// recording all the threads launching cuda kernels
std::unordered_set<pthread_t> g_kernelThreadTids;
std::unordered_map<pid_t, pthread_t> g_pidt2pthreadt;
std::unordered_map<pthread_t, pid_t> g_pthreadt2pidt;
std::unordered_map<pthread_t, bool> g_kernelThreadSyncedMap;
bool g_kernelThreadSynced = false;
// id of selected thread to execute cuptiStart/StopPCSampling
pthread_t selectedTid;

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
std::unordered_map<uint64_t, uint64_t> g_esp2pcIdMap;
std::stack<UNWValue> g_callStack;
bool g_genCallStack = false;
CPUCallStackSamplerCollection* g_cpuSamplerCollection;
std::thread g_cpuSamplerThreadHandle;

// Variables related to initialize injection once.
bool g_initializedInjection = false;
std::mutex g_initializeInjectionMutex;

// the standby grpc server
std::unique_ptr<Server> server;
std::thread g_rpcReplyCopyThreadHandle;
GPUProfilingResponse* g_reply;

// cupti args
CUpti_PCSamplingCollectionMode g_pcSamplingCollectionMode = CUPTI_PC_SAMPLING_COLLECTION_MODE_CONTINUOUS;
CUpti_SubscriberHandle subscriber;

// return pc samples only
bool g_tracingStarted = false;

class CUptiTracingRecord {
public:
    CUptiTracingRecord(){};

    // indicating call path
    unw_word_t parentCPUPCID;
 
    // kernel name
    std::string funcName;

    // accumulated execution time, attributed to each <parentCPUPCID, funcName> pair
    uint64_t duration;
};

std::unordered_map<std::string, CUptiTracingRecord*> g_tracingRecords;
std::unordered_map<uint32_t, std::string> g_corID2TracingKey;
