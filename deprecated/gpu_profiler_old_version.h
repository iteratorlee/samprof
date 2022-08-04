#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "cupti.h"
#include <cupti_pcsampling.h>
#include <cupti_pcsampling_util.h>

#include "utils.h"
#include "common.h"
#include "back_tracer.h"
#include "./cpp-gen/gpu_profiling.grpc.pb.h"

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

std::unique_ptr<Server> server;
pid_t g_rpcServerThreadPid;
std::thread g_rpcServerThreadHandle;
bool g_initializedInjection = false;
std::mutex g_initializeInjectionMutex;
CUpti_SubscriberHandle subscriber;
bool g_pcSamplingStarted = false;
bool g_pcSamplingConfigured = false;
std::unordered_set<CUcontext> g_cuCtxSet;

// recording pc samples collected using old version api
std::unordered_map<uint32_t, CUpti_ActivitySourceLocator*> g_sourceLocatorMap;
std::unordered_map<uint32_t, std::vector<CUpti_ActivityPCSampling3*>> g_pcSampling3Map;
std::unordered_map<uint32_t, std::vector<CUpti_ActivityPCSamplingRecordInfo*>> g_recordInfoMap;
std::unordered_map<uint32_t, CUpti_ActivityFunction*> g_functionMap;
