#include "gpu_profiler_old_version.h"

#define BUF_SIZE (32 * 1024)
#define ALIGN_SIZE (8)

static const char *
getStallReasonString(CUpti_ActivityPCSamplingStallReason reason)
{
    switch (reason) {
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_INVALID:
        return "Invalid";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_NONE:
        return "Selected";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_INST_FETCH:
        return "Instruction fetch";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_EXEC_DEPENDENCY:
        return "Execution dependency";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_MEMORY_DEPENDENCY:
        return "Memory dependency";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_TEXTURE:
        return "Texture";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_SYNC:
        return "Sync";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_CONSTANT_MEMORY_DEPENDENCY:
        return "Constant memory dependency";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_PIPE_BUSY:
        return "Pipe busy";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_MEMORY_THROTTLE:
        return "Memory throttle";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_NOT_SELECTED:
        return "Not selected";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_OTHER:
        return "Other";
    case CUPTI_ACTIVITY_PC_SAMPLING_STALL_SLEEPING:
        return "Sleeping";
    default:
        break;
    }

    return "<unknown>";
}

static void
fakePrintActivity(CUpti_Activity *record) {
    static int cnt = 0;
    if (++cnt % 10000 == 0) {
        printf("handling pc samples #%d\n", cnt);
    }
}

static void
processActivity(CUpti_Activity *record)
{
    switch (record->kind) {
        case CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR:
        {
            CUpti_ActivitySourceLocator *sourceLocator = (CUpti_ActivitySourceLocator *)record;
            printf("Source Locator Id %d, File %s Line %d\n", sourceLocator->id, sourceLocator->fileName, sourceLocator->lineNumber);
            if (g_sourceLocatorMap.find(sourceLocator->id) == g_sourceLocatorMap.end()) {
                g_sourceLocatorMap.insert(std::make_pair((uint32_t)sourceLocator->id, sourceLocator));
            } else {
                DEBUG_LOG("duplicated source locator id: %d\n", sourceLocator->id);
            }
            break;
        }
        case CUPTI_ACTIVITY_KIND_PC_SAMPLING:
        {
            CUpti_ActivityPCSampling3 *psRecord = (CUpti_ActivityPCSampling3 *)record;
            printf("source %u, functionId %u, pc 0x%llx, corr %u, samples %u",
                  psRecord->sourceLocatorId,
                  psRecord->functionId,
                  (unsigned long long)psRecord->pcOffset,
                  psRecord->correlationId,
                  psRecord->samples
                  );
            auto iter = g_pcSampling3Map.find(psRecord->correlationId);
            if (iter != g_pcSampling3Map.end()) {
                iter->second.push_back(psRecord);
            } else {
                std::vector<CUpti_ActivityPCSampling3*> psVec;
                psVec.push_back(psRecord);
                g_pcSampling3Map.insert(std::make_pair((uint32_t)psRecord->correlationId, psVec));
            }

            // latencySamples Field is valid for devices with compute capability 6.0 and higher.
            printf(", latency samples %u", psRecord->latencySamples);

            printf(", stallreason %s\n", getStallReasonString(psRecord->stallReason));
            break;
        }
        case CUPTI_ACTIVITY_KIND_PC_SAMPLING_RECORD_INFO:
        {
            CUpti_ActivityPCSamplingRecordInfo *pcsriResult =
                                (CUpti_ActivityPCSamplingRecordInfo *)(void *)record;

            printf("corr %u, totalSamples %llu, droppedSamples %llu, samplingPeriodInCycles %llu\n",
                  pcsriResult->correlationId,
                  (unsigned long long)pcsriResult->totalSamples,
                  (unsigned long long)pcsriResult->droppedSamples,
                  (unsigned long long)pcsriResult->samplingPeriodInCycles);
            auto iter = g_recordInfoMap.find(pcsriResult->correlationId);
            if (iter != g_recordInfoMap.end()) {
                iter->second.push_back(pcsriResult);
            } else {
                std::vector<CUpti_ActivityPCSamplingRecordInfo*> pcsriVec;
                pcsriVec.push_back(pcsriResult);
                g_recordInfoMap.insert(std::make_pair((uint32_t)pcsriResult->correlationId, pcsriVec));
            }
            break;
        }
        case CUPTI_ACTIVITY_KIND_FUNCTION:
        {
            CUpti_ActivityFunction *fResult =
                (CUpti_ActivityFunction *)record;

            printf("id %u, ctx %u, moduleId %u, functionIndex %u, name %s\n",
                fResult->id,
                fResult->contextId,
                fResult->moduleId,
                fResult->functionIndex,
                fResult->name);
            if (g_functionMap.find(fResult->id) == g_functionMap.end()) {
                g_functionMap.insert(std::make_pair((uint32_t)fResult->id, fResult));
            } else {
                DEBUG_LOG("duplicated function id: %d\n", fResult->id);
            }
            break;
        }
        default:
            printf("unknown\n");
            break;
    }
}

static void
convertPCSampling3Records2PCSamplingPCData() {
    std::cout << "source locators:" << std::endl;
    for (auto iter: g_sourceLocatorMap) {
    }
}

static void CUPTIAPI
bufferRequested(uint8_t **buffer, size_t *size, size_t *maxNumRecords)
{
    *size = BUF_SIZE + ALIGN_SIZE;
    *buffer = (uint8_t*) calloc(1, *size);
    *maxNumRecords = 0;
    if (*buffer == NULL) {
        printf("Error: out of memory\n");
        exit(-1);
    }
}

static void CUPTIAPI
bufferCompleted(CUcontext ctx, uint32_t streamId, uint8_t *buffer, size_t size, size_t validSize)
{
    CUptiResult status;
    CUpti_Activity *record = NULL;
    do {
        status = cuptiActivityGetNextRecord(buffer, validSize, &record);
        if(status == CUPTI_SUCCESS) {
            processActivity(record);
            // fakePrintActivity(record);
        }
        else if (status == CUPTI_ERROR_MAX_LIMIT_REACHED) {
            break;
        }
        else {
            CUPTI_CALL(status);
        }
    } while (1);

    size_t dropped;
    CUPTI_CALL(cuptiActivityGetNumDroppedRecords(ctx, streamId, &dropped));
    if (dropped != 0) {
        printf("Dropped %u activity records\n", (unsigned int)dropped);
    }

    free(buffer);
}

static void configuraPCSampling() {
    for (auto ctx: g_cuCtxSet) {
        CUpti_ActivityPCSamplingConfig configPC;
        configPC.size = sizeof(CUpti_ActivityPCSamplingConfig);
        configPC.samplingPeriod = CUPTI_ACTIVITY_PC_SAMPLING_PERIOD_MIN;
        configPC.samplingPeriod2 = 0;
        cuptiActivityConfigurePCSampling(ctx, &configPC);
    }
}

static void startPCSampling() {
    if (!g_pcSamplingConfigured) {
        configuraPCSampling();
        g_pcSamplingConfigured = true;
    }
    cuptiActivityEnable(CUPTI_ACTIVITY_KIND_PC_SAMPLING);
    g_pcSamplingStarted = true;
}

static void stopPCSampling() {
    cuptiActivityDisable(CUPTI_ACTIVITY_KIND_PC_SAMPLING);
    g_pcSamplingStarted = false;
}

void callStackUnwindingHandler(int signum) {
    auto backTracer = GetBackTracer();
    if (backTracer->handlingRemoteUnwinding) {
        backTracer->GenCallStack(backTracer->g_callStack);
        backTracer->handlingRemoteUnwinding = false;
    }
}

void CallbackHandler(void* userdata, CUpti_CallbackDomain domain, CUpti_CallbackId cbid,
    void* cbdata) {
    switch (domain) {
        case CUPTI_CB_DOMAIN_RESOURCE: {
            const CUpti_ResourceData* resourceData = (CUpti_ResourceData*)cbdata;

            switch (cbid) {
                case CUPTI_CBID_RESOURCE_CONTEXT_CREATED: {
                    DEBUG_LOG("context inserted\n");
                    g_cuCtxSet.insert(resourceData->context);
                }
                break;
                case CUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING: {
                    DEBUG_LOG("context erased\n");
                    g_cuCtxSet.erase(resourceData->context);
                }
                break;
                default:
                break;
            }
        }
        break;
        case CUPTI_CB_DOMAIN_DRIVER_API: {
            const CUpti_CallbackData* cbInfo = (CUpti_CallbackData*)cbdata;
            switch (cbid) {
                case CUPTI_DRIVER_TRACE_CBID_cuLaunch:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchGrid:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchGridAsync:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel_ptsz:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel_ptsz:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice: {
                    if (g_pcSamplingStarted && cbInfo->callbackSite == CUPTI_API_ENTER) {
                        pthread_t tid = pthread_self();
                        if (GetProfilerConf()->doCPUCallStackUnwinding && g_pcSamplingStarted) {
                            GetBackTracer()->DoBackTrace(GetProfilerConf()->backTraceVerbose);
                            GetBackTracer()->SetCorId2ActivePCID(cbInfo->correlationId);
                        }
                    }
                }
                break;
            }
        }
        break;
        default:
        break;
    }
}

class GPUProfilingServiceImpl final: public GPUProfilingService::Service {
    Status PerformGPUProfiling(ServerContext* context, const GPUProfilingRequest* request, 
        GPUProfilingResponse* reply) {
        DEBUG_LOG("profiling request received, duration=%u\n", request->duration());
        startPCSampling();
        DEBUG_LOG("pc sampling started, sleeping\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(request->duration()));
        stopPCSampling();
        DEBUG_LOG("pc sampling stopped\n");
        Timer* backTracerTimer = Timer::GetGlobalTimer("back_tracer");
        DEBUG_LOG("backtracer overhead: %lf\n", backTracerTimer->getAccumulatedTime());
        reply->set_message("ok");
        return Status::OK;
    }
};

void RunServer() {
    std::string server_address("0.0.0.0:8886");
    ServerBuilder builder;
    GPUProfilingServiceImpl service;

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    server = builder.BuildAndStart();
    DEBUG_LOG("Server listeninig on %s\n", server_address.c_str());

    server->Wait();
}

void AtExitHandler() {
    if (!GetProfilerConf()->noRPC) {
        server->Shutdown();
        DEBUG_LOG("grpc server shutdown\n");
    }
    if (!GetProfilerConf()->noRPC && g_rpcServerThreadHandle.joinable()) {
        g_rpcServerThreadHandle.join();
    }
}

void registerAtExitHandler(void) {
    DEBUG_LOG("AtExitHandler registered\n");
    atexit(&AtExitHandler);
}

extern "C" int InitializeInjection(void) {
    g_initializeInjectionMutex.lock();
    if (!g_initializedInjection) {
        DEBUG_LOG("... Initialize injection ...\n");
        auto profilerConf = GetProfilerConf();
        profilerConf->mainThreadTid = pthread_self();
        CUPTI_CALL(cuptiActivityRegisterCallbacks(bufferRequested, bufferCompleted));

        CUPTI_CALL(cuptiSubscribe(&subscriber, (CUpti_CallbackFunc)&CallbackHandler, NULL));
        DEBUG_LOG("subscriber registered\n");
        // CUPTI_CALL(cuptiEnableCallback(1, subscriber, CUPTI_CB_DOMAIN_RESOURCE, CUPTI_CBID_RESOURCE_CONTEXT_CREATED));
        // CUPTI_CALL(cuptiEnableCallback(1, subscriber, CUPTI_CB_DOMAIN_RESOURCE, CUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING));
        CUPTI_CALL(cuptiEnableAllDomains(1, subscriber));
        DEBUG_LOG("callback enabled\n");

        g_initializedInjection = true;

    }

    signal(SIGUSR1, callStackUnwindingHandler);
    if (GetProfilerConf()->noRPC) {
        startPCSampling();
    } else {
        g_rpcServerThreadHandle = std::thread(RunServer);
    }
    registerAtExitHandler();
    g_initializeInjectionMutex.unlock();

    return 1;
}