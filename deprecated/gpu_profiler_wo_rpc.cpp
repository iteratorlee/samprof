/*
 * Work flow in brief:
 *
 *    Subscribed for all the launch callbacks and required resource callbacks like module and context callbacks
 *        Context created callback:
 *            Enable PC sampling using cuptiPCSamplingEnable() CUPTI API.
 *            Configure PC sampling for that context in ConfigureActivity() function.
 *                ConfigureActivity():
 *                    Get count of all stall reasons supported on GPU using cuptiPCSamplingGetNumStallReasons() CUPTI API.
 *                    Get all stall reasons names and its indexes using cuptiPCSamplingGetStallReasons() CUPTI API.
 *                    Configure PC sampling with provide parameters and to sample all stall reasons using
 *                    cuptiPCSamplingSetConfigurationAttribute() CUPTI API.
 *            queue of buffers into the file.
 *            Only for first context creation, allocate memory for circular buffers which will hold flushed data from cupti.
 *
 *        Launch callbacks:
 *           If serialized mode is enabled then every time if cupti has PC records then flush all records using
 *           cuptiPCSamplingGetData() and push buffer in queue with context info to fill the rpc reply.
 *           If continuous mode is enabled then if cupti has more records than size of single circular buffer
 *           then flush records in one circular buffer using cuptiPCSamplingGetData() and push it in queue with
 *           context info to fill the rpc reply.
 *
 *        Module load:
 *           This callback covers case when module get unloaded and new module get loaded then cupti flush
 *           all records into the provided buffer during configuration.
 *           So in this callback if provided buffer during configuration has any records then flush all records into
 *           the circular buffers and push them into the queue with context info to fill them into the rpc reply.
 *
 *        Context destroy starting:
 *           Disable PC sampling using cuptiPCSamplingDisable() CUPTI API
 *
 *    AtExitHandler
 *        If PC sampling is not disabled for any context then disable it using cuptiPCSamplingDisable().
 *        Push PC sampling buffer in queue which provided during configuration with context info for each context
 *        as cupti flush all remaining PC records into this buffer in the end.
 *        Free allocated memory for circular buffer, stall reason names, stall reasons indexes and
 *        PC sampling buffers provided during configuration.
 *
 *    RPC server:
 *        A RPC server is started once the libaray is loaded. The server is responsible for recieving the request to
 *        perform a PC sampling for a specific time interval with a <Duration> parameter indicated.
 */

#include "gpu_profiler_wo_rpc.h"

static void InitCUptiSettings() {
    char* s;
    if ((s = getenv("CUPTI_SAMPLING_PERIOD")) != nullptr) {
        g_samplingPeriod = std::strtoul(s, nullptr, 10);
    }
    if ((s = getenv("CUPTI_BUF_SIZE")) != nullptr) {
        g_scratchBufSize = std::strtoul(s, nullptr, 10);
    }
    if ((s = getenv("CUPTI_HWBUF_SIZE")) != nullptr) {
        g_hwBufSize = std::strtoul(s, nullptr, 10);
    }
    if ((s = getenv("CUPTI_PC_CONFIG_BUF_RECORD_COUNT")) != nullptr) {
        g_pcConfigBufRecordCount = std::strtoul(s, nullptr, 10);
    }
    if ((s = getenv("CUPTI_CIRCULAR_BUF_COUNT")) != nullptr) {
        g_circularbufCount = std::strtoul(s, nullptr, 10);
    }
    if ((s = getenv("CUPTI_CIRCULAR_BUF_SIZE")) != nullptr) {
        g_circularbufSize = std::strtoul(s, nullptr, 10);
    }
    if ((s = getenv("RETURN_CUDA_PC_SAMPLE_ONLY")) != nullptr) {
        g_fakeBT = std::strtol(s, nullptr, 10);
    }
    if ((s = getenv("DO_CPU_CALL_STACK_UNWINDING")) != nullptr) {
        g_DoCPUCallstackUnwinding = std::strtol(s, nullptr, 10);
    }
    if ((s = getenv("PRUNE_CCT")) != nullptr) {
        g_pruneCCT = std::strtol(s, nullptr, 10);
    }
    if ((s = getenv("DL_BACKEND")) != nullptr) {
        g_backEnd = s;
    }
    if ((s = getenv("CHECK_RSP")) != nullptr) {
        g_checkRSP = std::strtol(s, nullptr, 10);
    }
    if ((s = getenv("SYNC_BEFORE_START")) != nullptr) {
        g_syncBeforeStart = std::strtol(s, nullptr, 10);
    }
    if ((s = getenv("BT_VERBOSE")) != nullptr) {
        g_backTraceVerbose = std::strtol(s, nullptr, 10);
    }
    if ((s = getenv("PY_FILENAME")) != nullptr) {
        g_pyFileName = s;
    }
    if ((s = getenv("NO_RPC")) != nullptr) {
        g_noRPC = std::strtol(s, nullptr, 10);
    }
    if ((s = getenv("DUMP_FILENAME")) != nullptr) {
        g_dumpFileName = s;
    } else {
        g_dumpFileName = "profile_result.dat";
    }
    if ((s = getenv("SAMPLING_DURATION")) != nullptr) {
        g_samplingDuration = std::strtoul(s, nullptr, 10);
    }
    if ((s = getenv("NATIVE_LATENCY")) != nullptr) {
        g_nativeLatency = std::strtoul(s, nullptr, 10);
    }
}

static inline void PrintUNWValue(UNWValue& val) {
    pid_t pid = gettid();
    pthread_t tid = pthread_self();
    DEBUG_LOG("[pid=%u, tid=%u] unwinding: pc=%lu:[%s+%lu]\n", (uint32_t)pid, (uint32_t)tid, val.pc, val.funcName.c_str(), val.offset);
}

static const char* PyObj2Str(PyObject* obj) {
    PyObject* str = PyUnicode_AsEncodedString(obj, "utf-8", "~E~");
    const char* bytes = PyBytes_AS_STRING(str);
    return bytes;
}

static std::string GetPyLine(std::string pyFileName, int pyLineNumer) {
    std::fstream inFile;
    std::string lineStr;
    inFile.open(pyFileName);
    int i = 1;
    while (std::getline(inFile, lineStr) && i < pyLineNumer) ++i;
    inFile.close();
    lineStr.erase(std::remove(lineStr.begin(), lineStr.end(), ' '), lineStr.end());
    return lineStr;
}

void pyBackTrace(std::queue<UNWValue>& pyFrameQueue) {
    // DEBUG_LOG("[py back trace] entered\n");
    PyInterpreterState* mainInterpState = PyInterpreterState_Main();
    //PyThreadState* pyState = PyInterpreterState_ThreadHead(mainInterpState); //PyGILState_GetThisThreadState();
    PyThreadState* pyState = PyGILState_GetThisThreadState();
    PyFrameObject* frame = pyState->frame;
    while (frame) {
        PyObject* fileNameObj = frame->f_code->co_filename;
        PyObject* funcNameObj = frame->f_code->co_name;
        const char* fileNameStr = PyObj2Str(fileNameObj);
        const char* funcNameStr = PyObj2Str(funcNameObj);
        int lineNumber = PyFrame_GetLineNumber(frame);
        std::string lineContent = GetPyLine(fileNameStr, lineNumber);
        // DEBUG_LOG("[py back trace] fileName: %s, funcName:%s, lineNumber:%d, lineContent:%s\n", fileNameStr, funcNameStr, lineNumber, lineContent.c_str());
        pyFrameQueue.push(UNWValue(fileNameStr, std::string(funcNameStr) + "::" + lineContent, lineNumber));
        frame = frame->f_back;
    }
}

void getRSP(uint64_t *rsp) {
    __asm__ __volatile__ (
        "mov %%rsp, %0"
        :"=m"(*rsp)
        ::"memory"
    );
}

static CallStackStatus GenCallStack(std::stack<UNWValue> &q, bool verbose=false) {
    #if DEBUG
    Timer* genCallStackTimer = Timer::GetGlobalTimer("gen_call_stack");
    genCallStackTimer->start();
    #endif
    std::queue<UNWValue> pyFrameQueue;
    CallStackStatus status;
    if (g_backEnd == "TORCH") pyBackTrace(pyFrameQueue);
    if (pyFrameQueue.size()) status = CALL_STACK_HAS_PY;
    else status = CALL_STACK_NOT_HAS_PY;

    unw_cursor_t cursor;
    unw_context_t context;

    unw_getcontext(&context);
    unw_init_local(&cursor, &context);

    while (unw_step(&cursor) > 0) {
        unw_word_t offset, pc;
        char fname[FUNC_NAME_LENGTH];
        char* outer_name;
        
        unw_get_reg(&cursor, UNW_REG_IP, &pc);
        auto getProcTimer = Timer::GetGlobalTimer("unwinding_get_proc_name");
        getProcTimer->start();
        unw_get_proc_name(&cursor, fname, sizeof(fname), &offset);
        getProcTimer->stop();
        int status = 99;
        if ((outer_name = abi::__cxa_demangle(fname, nullptr, nullptr, &status)) == 0) {
            outer_name = fname;
        }

        // skip cupti-related stack frames
        if (HasExcludePatterns(outer_name)) continue;

        if (g_backEnd == "TORCH" && std::string(outer_name).find("_PyEval_EvalFrameDefault") != std::string::npos) {
            UNWValue value = pyFrameQueue.front();
            value.pc = pc + value.offset; // use native pc plus offset as PyFrame pc
            q.push(value);
            pyFrameQueue.pop();
        } else {
            UNWValue value(pc, offset, std::string(outer_name));
            q.push(value);
        }
        if (verbose) {
            PrintUNWValue(q.top());
        }
    }

    #if DEBUG
    genCallStackTimer->stop();
    #endif
    return status;
}

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

static void DoBackTrace(bool verbose=false) {
    uint64_t rsp;
    getRSP(&rsp);
    if (verbose) DEBUG_LOG("rsp=%p\n", (void *)rsp);
    if (g_checkRSP && g_esp2pcIdMap.find(rsp) != g_esp2pcIdMap.end()) {
        uint64_t pcId = g_esp2pcIdMap[rsp];
        g_activeCPUPCIDMutex.lock();
        g_activeCPUPCID = pcId;
        g_activeCPUPCIDMutex.unlock();
        if (verbose) DEBUG_LOG("already unwound, active pc id changed to %lu\n", pcId);
        return;
    }

    // nodes to be inserted to the cpu calling context tree
    std::stack<UNWValue> toInsertUNW;
    std::stack<UNWValue> toInsertUNWMain;

    auto status = GenCallStack(toInsertUNW, verbose);
    if (g_backEnd == "TORCH" && status == CALL_STACK_NOT_HAS_PY) {
        DEBUG_LOG("this thread has not PyFrame, going to the main thread\n");
        g_genCallStack = true;
        pthread_kill(g_mainThreadTid, SIGUSR1);
        while (g_genCallStack);
        toInsertUNWMain = g_callStack;
        // TODO: clear the stack or not ?
        // while (!g_callStack.empty()) g_callStack.pop();
    }

    pthread_t tid = pthread_self();
    if (g_CPUCCTMap.find(tid) == g_CPUCCTMap.end()) {
        CPUCCT* newCCT = new CPUCCT();
        g_CPUCCTMap.insert(std::make_pair(tid, newCCT));
    }

    CPUCCT* cpuCCT = g_CPUCCTMap[tid];

    if (cpuCCT->root == nullptr) {
        // the root has not been set
        UNWValue value;
        TOP2(toInsertUNWMain, toInsertUNW, value);
        CPUCCTNode* newNode = new CPUCCTNode(value.nodeType);
        newNode->pc = value.pc;
        newNode->offset = value.offset;
        g_CPUCCTNodeIdMutex.lock();
        newNode->id = g_CPUCCTNodeId;
        ++g_CPUCCTNodeId;
        g_CPUCCTNodeIdMutex.unlock();
        if (value.nodeType == CCTNODE_TYPE_CXX) {
            newNode->funcName = value.funcName;// + "_" + std::to_string(newNode->id);
        } else {
            // std::string lineContent = GetPyLine(value.fileName, value.pc);
            // lineContent.erase(std::remove_if(lineContent.begin(), lineContent.end(), std::isspace), lineContent.end());
            newNode->funcName = value.fileName + "::" + value.funcName + "_" + std::to_string(value.offset)  + "_" + std::to_string(newNode->id);
        }
        cpuCCT->setRootNode(newNode);
        if (g_fakeBT) {
            g_activeCPUPCIDMutex.lock();
            if (verbose) DEBUG_LOG("active pc changed to %lu:%p\n", newNode->id, (void *)(newNode->pc));
            g_activeCPUPCID = newNode->id;
            g_activeCPUPCIDMutex.unlock();
            return;
        }
        // toInsertUNW.pop();
        POP2(toInsertUNWMain, toInsertUNW);
    } else {
        if (g_fakeBT) return;
        if (cpuCCT->root->pc != toInsertUNW.top().pc) {
            DEBUG_LOG("WARNING: duplicate root pc: old pc: %p, new pc: %p\n", (void *)cpuCCT->root->pc, (void *)toInsertUNW.top().pc);
        }
        // toInsertUNW.pop();
        POP2(toInsertUNWMain, toInsertUNW);
    }

    CPUCCTNode* parentNode = cpuCCT->root;
    while (!toInsertUNW.empty()) {
        UNWValue value;
        TOP2(toInsertUNWMain, toInsertUNW, value);
        CPUCCTNode* childNode = parentNode->getChildbyPC(value.pc);
        if (childNode) {
            parentNode = childNode;
            // toInsertUNW.pop();
            POP2(toInsertUNWMain, toInsertUNW);
        } else {
            break;
        }
    }

    if (toInsertUNW.empty()) {
        g_activeCPUPCIDMutex.lock();
        g_activeCPUPCID = parentNode->id;
        if (verbose) DEBUG_LOG("old pc, active pc changed to %lu:%p\n", parentNode->id, (void *)(parentNode->pc));
        g_activeCPUPCIDMutex.unlock();
    }

    while (!toInsertUNW.empty()) {
        UNWValue value;
        TOP2(toInsertUNWMain, toInsertUNW, value);
        CPUCCTNode* newNode = new CPUCCTNode(value.nodeType);
        newNode->pc = value.pc;
        newNode->offset = value.offset;
        g_CPUCCTNodeIdMutex.lock();
        newNode->id = g_CPUCCTNodeId;
        ++g_CPUCCTNodeId;
        g_CPUCCTNodeIdMutex.unlock();
        if (value.nodeType == CCTNODE_TYPE_CXX) {
            newNode->funcName = value.funcName;// + "_" + std::to_string(newNode->id);
        } else {
            // newNode->funcName = value.fileName + "::" + value.funcName + "_" + std::to_string(newNode->id);
            newNode->funcName = value.fileName + "::" + value.funcName + "_" + std::to_string(value.offset)  + "_" + std::to_string(newNode->id);
        }
        if (toInsertUNW.size() == 1) {
            g_activeCPUPCIDMutex.lock();
            if (verbose) DEBUG_LOG("active pc changed to %lu:%p\n", newNode->id, (void *)(newNode->pc));
            g_activeCPUPCID = newNode->id;
            g_esp2pcIdMap[rsp] = newNode->id;
            g_activeCPUPCIDMutex.unlock();
        }
        cpuCCT->insertNode(parentNode, newNode);
        parentNode = newNode;
        // toInsertUNW.pop();
        POP2(toInsertUNWMain, toInsertUNW);
    }
}

static bool IsCriticalNode(CPUCCT* tree, CPUCCTNode* node) {
    std::unordered_set<std::string> keptParentNames = {"BaseGPUDevice"};
    std::unordered_set<std::string> keptChildNames = {"wrap_kernel_functor_unboxed_"};

    // keep python nodes
    if (node->nodeType == CCTNODE_TYPE_PY) {
        if (node->funcName.find("python3") == std::string::npos) {
            if (node->funcName.find("backward") != std::string::npos) {
                DEBUG_LOG("critical node, kind=backward, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
                return true;
            }
            if (node->funcName.find(g_pyFileName) != std::string::npos && node->funcName.find("loss") != std::string::npos) {
                DEBUG_LOG("critical node, kind=loss, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
                return true;
            }
            if (node->funcName.find("forward") != std::string::npos) {
                DEBUG_LOG("critical node, kind=forward, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
                return true;
            }
        }
    }

    // leaf node
    if (node->childNodes.size() == 0) {
        DEBUG_LOG("critical node, kind=leaf, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
        return true;
    }

    // kept nodes due to parent node name rules
    std::string parentFuncName = tree->nodeMap[node->parentID]->funcName;
    for (auto kn: keptParentNames) {
        if (parentFuncName.find(kn) != std::string::npos){
            DEBUG_LOG("critical node, kind=kept parent, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
            return true;
        }
    }

    // kept nodes due to child node name rules
    for (auto itr: node->id2ChildNodes){
        std::string childFuncName = itr.second->funcName;
        for (auto kn: keptChildNames) {
            if (childFuncName.find(kn) != std::string::npos){
                DEBUG_LOG("critical node, kind=kept child, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
                return true;
            }
        }
    }

    // there are pc samples depending on this node
    for (auto itr: g_GPUPCSamplesParentCPUPCIDs) {
        if (node->id == itr.second) {
            DEBUG_LOG("critical node, kind=active pc, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
            return true;
        }
    }
    return false;
}

static void PruneTreeRecursively(CPUCCT* newTree, CPUCCT* oldTree, uint64_t currNewNodeId,
                        uint64_t currOldNodeId) {
    for (auto itr: oldTree->nodeMap[currOldNodeId]->id2ChildNodes) {
        auto id = itr.first;
        auto child = itr.second;
        if (IsCriticalNode(oldTree, child)) {
            CPUCCTNode* newChild = new CPUCCTNode();
            CPUCCTNode::copyNodeWithoutRelation(child, newChild);
            newTree->insertNode(newTree->nodeMap[currNewNodeId], newChild, true);
            PruneTreeRecursively(newTree, oldTree, newChild->id, child->id);
        } else {
            PruneTreeRecursively(newTree, oldTree, currNewNodeId, child->id);
        }
    }
}

static void PruneCPUCCT(CCTMAP_t& cctMap) {
    DEBUG_LOG("pruning cpu cct\n");
    for (auto itr: g_CPUCCTMap) {
        auto key = itr.first;
        CPUCCT* oldCCT = itr.second;
        CPUCCT* newCCT = new CPUCCT();
        cctMap.insert(std::make_pair(key, newCCT));
        CPUCCTNode* oldRootNode = oldCCT->root;
        CPUCCTNode* newRootNode = new CPUCCTNode();
        CPUCCTNode::copyNodeWithoutRelation(oldRootNode, newRootNode);
        newCCT->setRootNode(newRootNode);
        PruneTreeRecursively(newCCT, oldCCT, newRootNode->id, oldRootNode->id);
    }
}

static void DumpCPUCCT() {
    int cctCnt = 0;
    std::ofstream dFile;
    dFile.open(g_dumpFileName, std::ios::out | std::ios::app);
    for (auto cctItr: g_CPUCCTMap) {
        // printf("cct #%d\n", cctCnt);
        dFile << "cct #" << cctCnt << std::endl;
        ++cctCnt;
        auto cct = cctItr.second;
        for (auto nodeItr: cct->nodeMap) {
            auto node = nodeItr.second;
            // printf("id=%lu,pc=%lu,parentID=%lu,parentPC=%lu,funcName=%s,childs=",
            //     node->id, node->pc, node->parentID, node->parentPC, node->funcName.c_str());
            dFile << "id=" << node->id << ",pc=" << node->pc << ",parentID=" << node->parentID
                  << ",parentPC=" << node->parentPC << ",funcName=" << node->funcName << ",children=";
            for (auto childNode: node->childNodes) {
                // printf("%lu ", childNode->id);
                dFile << childNode->id << " ";
            }
            dFile << std::endl;
            // printf("\n");
        }
        // printf("\n");
        dFile << std::endl;
    }
    dFile.close();
    DEBUG_LOG("cct dumped to %s\n", g_dumpFileName.c_str());
}

static void DumpPCSamples(std::vector<CUpti_PCSamplingData*> v) {
    int pcSampleCnt = 0;
    std::ofstream dFile;
    dFile.open(g_dumpFileName, std::ios::out | std::ios::app);
    for (auto pcSampData: v) {
        // printf("pc sample data #%d, sample num=%lu\n", pcSampleCnt, pcSampData->totalNumPcs);
        dFile << "pc sample data #" << pcSampleCnt << ", sample num=" << pcSampData->totalNumPcs << std::endl;
        ++pcSampleCnt;
        for (int i = 0; i < pcSampData->totalNumPcs; ++i) {
            auto pcData = pcSampData->pPcData[i];
            // printf("functionId=%u,functionName=%s,cubinCrc=%lu,pcOffset=%lu,stallReasonCnt=%lu\n",
            // pcData.functionIndex, pcData.functionName, pcData.cubinCrc, pcData.pcOffset, pcData.stallReasonCount);
            dFile << "functionId=" << pcData.functionIndex /*<< ",functionName=" << std::string(pcData.functionName)*/
                  << ",cubinCrc=" << pcData.cubinCrc << ",pcOffset=" << pcData.pcOffset
                  << ",stallReasonCnt=" << pcData.stallReasonCount
                  << ",parentPCID=" << g_GPUPCSamplesParentCPUPCIDs[&pcData] <<std::endl;
            for (int j = 0; j < pcData.stallReasonCount; ++j) {
                auto reason = pcData.stallReason[j];
                // printf("reason=%u,sample cnt=%u\n", reason.pcSamplingStallReasonIndex, reason.samples);
                dFile << "reason=" << reason.pcSamplingStallReasonIndex << "/" << reason.samples << " ";
            }
            dFile << std::endl;
        }
        dFile << std::endl;
    }
    dFile.close();
    DEBUG_LOG("pc samples dumped to %s\n", g_dumpFileName.c_str());
}

static void StorePCSamplesParents(CUpti_PCSamplingData* pPcSamplingData) {
    for (int i = 0; i < pPcSamplingData->totalNumPcs; ++i) {
        CUpti_PCSamplingPCData* pPcData = &pPcSamplingData->pPcData[i];
        g_GPUPCSamplesParentCPUPCIDs[pPcData] = g_activeCPUPCID;
    }
}

static void GetPcSamplingDataFromCupti(CUpti_PCSamplingGetDataParams &pcSamplingGetDataParams, ContextInfo *contextInfo)
{
    CUpti_PCSamplingData *pPcSamplingData = NULL;

    g_circularBufferMutex.lock();
    while (g_bufferEmptyTrackerArray[g_put])
    {
        g_buffersGetUtilisedFasterThanStore = true;
    }

    pcSamplingGetDataParams.pcSamplingData = (void *)&g_circularBuffer[g_put];
    pPcSamplingData = &g_circularBuffer[g_put];

    g_bufferEmptyTrackerArray[g_put] = true;
    g_put = (g_put+1) % g_circularbufCount;
    g_circularBufferMutex.unlock();

    CUPTI_CALL(cuptiPCSamplingGetData(&pcSamplingGetDataParams));

    g_pcSampDataQueueMutex.lock();
    g_pcSampDataQueue.push(std::make_pair(pPcSamplingData, contextInfo));
    StorePCSamplesParents(pPcSamplingData);
    g_pcSampDataQueueMutex.unlock();
}

static void CollectPCSamples() {
    for(auto& itr: g_contextInfoMap)
    {
        DEBUG_LOG("collecting pc samples left in context %u\n", itr.second->contextUid);
        CUpti_PCSamplingGetDataParams pcSamplingGetDataParams = {};
        pcSamplingGetDataParams.size = CUpti_PCSamplingGetDataParamsSize;
        pcSamplingGetDataParams.ctx = itr.first;

        while (itr.second->pcSamplingData.remainingNumPcs > 0 || itr.second->pcSamplingData.totalNumPcs > 0)
        {
            DEBUG_LOG("remainingNumPcs=%lu, totoalNumPcs=%lu\n", itr.second->pcSamplingData.remainingNumPcs, itr.second->pcSamplingData.totalNumPcs);
            GetPcSamplingDataFromCupti(pcSamplingGetDataParams, itr.second);
        }
        DEBUG_LOG("collecting remaining pc samples finished\n");

        if (itr.second->pcSamplingData.totalNumPcs > 0)
        {
            g_pcSampDataQueueMutex.lock();
            // It is quite possible that after pc sampling disabled cupti fill remaining records
            // collected lately from hardware in provided buffer during configuration.
            g_pcSampDataQueue.push(std::make_pair(&itr.second->pcSamplingData, itr.second));
            g_pcSampDataQueueMutex.unlock();
        }
    }
    DEBUG_LOG("collecting left pc samples finished\n");
}

static void PreallocateBuffersForRecords()
{
    for (size_t buffers=0; buffers<g_circularbufCount; buffers++)
    {
        g_circularBuffer[buffers].size = sizeof(CUpti_PCSamplingData);
        g_circularBuffer[buffers].collectNumPcs = g_circularbufSize;
        g_circularBuffer[buffers].pPcData = (CUpti_PCSamplingPCData *)malloc(g_circularBuffer[buffers].collectNumPcs * sizeof(CUpti_PCSamplingPCData));
        MEMORY_ALLOCATION_CALL(g_circularBuffer[buffers].pPcData);
        for (size_t i = 0; i < g_circularBuffer[buffers].collectNumPcs; i++)
        {
            g_circularBuffer[buffers].pPcData[i].stallReason = (CUpti_PCSamplingStallReason *)malloc(stallReasonsCount * sizeof(CUpti_PCSamplingStallReason));
            MEMORY_ALLOCATION_CALL(g_circularBuffer[buffers].pPcData[i].stallReason);
        }
    }
}

static void FreePreallocatedMemory()
{
    for (size_t buffers=0; buffers<g_circularbufCount; buffers++)
    {
        for (size_t i = 0; i < g_circularBuffer[buffers].collectNumPcs; i++)
        {
            free(g_circularBuffer[buffers].pPcData[i].stallReason);
        }

        free(g_circularBuffer[buffers].pPcData);
    }

    for(auto& itr: g_contextInfoMap)
    {
        // free PC sampling buffer
        for (uint32_t i = 0; i < g_pcConfigBufRecordCount; i++)
        {
            free(itr.second->pcSamplingData.pPcData[i].stallReason);
        }
        free(itr.second->pcSamplingData.pPcData);

        for (size_t i = 0; i < itr.second->pcSamplingStallReasons.numStallReasons; i++)
        {
            free(itr.second->pcSamplingStallReasons.stallReasons[i]);
        }
        free(itr.second->pcSamplingStallReasons.stallReasons);
        free(itr.second->pcSamplingStallReasons.stallReasonIndex);

        free(itr.second);
    }

    for(auto& itr: g_contextInfoToFreeInEndVector)
    {
        // free PC sampling buffer
        for (uint32_t i = 0; i < g_pcConfigBufRecordCount; i++)
        {
            free(itr->pcSamplingData.pPcData[i].stallReason);
        }
        free(itr->pcSamplingData.pPcData);

        for (size_t i = 0; i < itr->pcSamplingStallReasons.numStallReasons; i++)
        {
            free(itr->pcSamplingStallReasons.stallReasons[i]);
        }
        free(itr->pcSamplingStallReasons.stallReasons);
        free(itr->pcSamplingStallReasons.stallReasonIndex);

        free(itr);
    }
}

void ConfigureActivity(CUcontext cuCtx)
{
    std::map<CUcontext, ContextInfo*>::iterator contextStateMapItr = g_contextInfoMap.find(cuCtx);
    if (contextStateMapItr == g_contextInfoMap.end())
    {
        std::cout << "Error : No ctx found" << std::endl;
        exit (-1);
    }

    CUpti_PCSamplingConfigurationInfo sampPeriod = {};
    CUpti_PCSamplingConfigurationInfo stallReason = {};
    CUpti_PCSamplingConfigurationInfo scratchBufferSize = {};
    CUpti_PCSamplingConfigurationInfo hwBufferSize = {};
    CUpti_PCSamplingConfigurationInfo collectionMode = {};
    CUpti_PCSamplingConfigurationInfo enableStartStop = {};
    CUpti_PCSamplingConfigurationInfo outputDataFormat = {};

    // Get number of supported counters and counter names
    size_t numStallReasons = 0;
    CUpti_PCSamplingGetNumStallReasonsParams numStallReasonsParams = {};
    numStallReasonsParams.size = CUpti_PCSamplingGetNumStallReasonsParamsSize;
    numStallReasonsParams.ctx = cuCtx;
    numStallReasonsParams.numStallReasons = &numStallReasons;

    g_stallReasonsCountMutex.lock();
    CUPTI_CALL(cuptiPCSamplingGetNumStallReasons(&numStallReasonsParams));

    if (!g_collectedStallReasonsCount)
    {
        stallReasonsCount = numStallReasons;
        g_collectedStallReasonsCount = true;
    }
    g_stallReasonsCountMutex.unlock();

    char **pStallReasons = (char **)malloc(numStallReasons * sizeof(char*));
    MEMORY_ALLOCATION_CALL(pStallReasons);
    for (size_t i = 0; i < numStallReasons; i++)
    {
        pStallReasons[i] = (char *)malloc(CUPTI_STALL_REASON_STRING_SIZE * sizeof(char));
        MEMORY_ALLOCATION_CALL(pStallReasons[i]);
    }
    uint32_t *pStallReasonIndex = (uint32_t *)malloc(numStallReasons * sizeof(uint32_t));
    MEMORY_ALLOCATION_CALL(pStallReasonIndex);

    CUpti_PCSamplingGetStallReasonsParams stallReasonsParams = {};
    stallReasonsParams.size = CUpti_PCSamplingGetStallReasonsParamsSize;
    stallReasonsParams.ctx = cuCtx;
    stallReasonsParams.numStallReasons = numStallReasons;
    stallReasonsParams.stallReasonIndex = pStallReasonIndex;
    stallReasonsParams.stallReasons = pStallReasons;
    CUPTI_CALL(cuptiPCSamplingGetStallReasons(&stallReasonsParams));

    // User buffer to hold collected PC Sampling data in PC-To-Counter format
    size_t pcSamplingDataSize = sizeof(CUpti_PCSamplingData);
    contextStateMapItr->second->pcSamplingData.size = pcSamplingDataSize;
    contextStateMapItr->second->pcSamplingData.collectNumPcs = g_pcConfigBufRecordCount;
    contextStateMapItr->second->pcSamplingData.pPcData = (CUpti_PCSamplingPCData *)malloc(g_pcConfigBufRecordCount * sizeof(CUpti_PCSamplingPCData));
    MEMORY_ALLOCATION_CALL(contextStateMapItr->second->pcSamplingData.pPcData);
    for (uint32_t i = 0; i < g_pcConfigBufRecordCount; i++)
    {
        contextStateMapItr->second->pcSamplingData.pPcData[i].stallReason = (CUpti_PCSamplingStallReason *)malloc(numStallReasons * sizeof(CUpti_PCSamplingStallReason));
        MEMORY_ALLOCATION_CALL(contextStateMapItr->second->pcSamplingData.pPcData[i].stallReason);
    }

    std::vector<CUpti_PCSamplingConfigurationInfo> pcSamplingConfigurationInfo;

    stallReason.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_STALL_REASON;
    stallReason.attributeData.stallReasonData.stallReasonCount = numStallReasons;
    stallReason.attributeData.stallReasonData.pStallReasonIndex = pStallReasonIndex;

    // set a buffer for each cu context to hold pc samples from cupti
    CUpti_PCSamplingConfigurationInfo samplingDataBuffer = {};
    samplingDataBuffer.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_SAMPLING_DATA_BUFFER;
    samplingDataBuffer.attributeData.samplingDataBufferData.samplingDataBuffer = (void *)&contextStateMapItr->second->pcSamplingData;

    sampPeriod.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_SAMPLING_PERIOD;
    if (g_samplingPeriod)
    {
        sampPeriod.attributeData.samplingPeriodData.samplingPeriod = g_samplingPeriod;
        pcSamplingConfigurationInfo.push_back(sampPeriod);
    }

    scratchBufferSize.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_SCRATCH_BUFFER_SIZE;
    if (g_scratchBufSize)
    {
        scratchBufferSize.attributeData.scratchBufferSizeData.scratchBufferSize = g_scratchBufSize;
        pcSamplingConfigurationInfo.push_back(scratchBufferSize);
    }

    hwBufferSize.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_HARDWARE_BUFFER_SIZE;
    if (g_hwBufSize)
    {
        hwBufferSize.attributeData.hardwareBufferSizeData.hardwareBufferSize = g_hwBufSize;
        pcSamplingConfigurationInfo.push_back(hwBufferSize);
    }

    collectionMode.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_COLLECTION_MODE;
    collectionMode.attributeData.collectionModeData.collectionMode = g_pcSamplingCollectionMode;
    pcSamplingConfigurationInfo.push_back(collectionMode);

    pcSamplingConfigurationInfo.push_back(stallReason);
    pcSamplingConfigurationInfo.push_back(samplingDataBuffer);
    
    enableStartStop.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_ENABLE_START_STOP_CONTROL;
    uint32_t enableStartStopControl = g_noRPC ? 0 : 1;
    enableStartStop.attributeData.enableStartStopControlData.enableStartStopControl = enableStartStopControl;
    pcSamplingConfigurationInfo.push_back(enableStartStop);

    CUpti_PCSamplingConfigurationInfoParams pcSamplingConfigurationInfoParams = {};
    pcSamplingConfigurationInfoParams.size = CUpti_PCSamplingConfigurationInfoParamsSize;
    pcSamplingConfigurationInfoParams.pPriv = NULL;
    pcSamplingConfigurationInfoParams.ctx = cuCtx;
    pcSamplingConfigurationInfoParams.numAttributes = pcSamplingConfigurationInfo.size();
    pcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo = pcSamplingConfigurationInfo.data();

    CUPTI_CALL(cuptiPCSamplingSetConfigurationAttribute(&pcSamplingConfigurationInfoParams));

    // Store all stall reasons info in context info to dump into the file.
    contextStateMapItr->second->pcSamplingStallReasons.numStallReasons = numStallReasons;
    contextStateMapItr->second->pcSamplingStallReasons.stallReasons = pStallReasons;
    contextStateMapItr->second->pcSamplingStallReasons.stallReasonIndex = pStallReasonIndex;

    // Find configuration info and store it in context info to dump in file.
    scratchBufferSize.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_SCRATCH_BUFFER_SIZE;
    hwBufferSize.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_HARDWARE_BUFFER_SIZE;
    enableStartStop.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_ENABLE_START_STOP_CONTROL;
    outputDataFormat.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_OUTPUT_DATA_FORMAT;
    outputDataFormat.attributeData.outputDataFormatData.outputDataFormat = CUPTI_PC_SAMPLING_OUTPUT_DATA_FORMAT_PARSED;

    std::vector<CUpti_PCSamplingConfigurationInfo> pcSamplingRetrieveConfigurationInfo;
    pcSamplingRetrieveConfigurationInfo.push_back(collectionMode);
    pcSamplingRetrieveConfigurationInfo.push_back(sampPeriod);
    pcSamplingRetrieveConfigurationInfo.push_back(scratchBufferSize);
    pcSamplingRetrieveConfigurationInfo.push_back(hwBufferSize);
    pcSamplingRetrieveConfigurationInfo.push_back(enableStartStop);

    CUpti_PCSamplingConfigurationInfoParams getPcSamplingConfigurationInfoParams = {};
    getPcSamplingConfigurationInfoParams.size = CUpti_PCSamplingConfigurationInfoParamsSize;
    getPcSamplingConfigurationInfoParams.pPriv = NULL;
    getPcSamplingConfigurationInfoParams.ctx = cuCtx;
    getPcSamplingConfigurationInfoParams.numAttributes = pcSamplingRetrieveConfigurationInfo.size();
    getPcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo = pcSamplingRetrieveConfigurationInfo.data();

    CUPTI_CALL(cuptiPCSamplingGetConfigurationAttribute(&getPcSamplingConfigurationInfoParams));

    for (size_t i = 0; i < getPcSamplingConfigurationInfoParams.numAttributes; i++)
    {
        contextStateMapItr->second->pcSamplingConfigurationInfo.push_back(getPcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo[i]);
    }

    contextStateMapItr->second->pcSamplingConfigurationInfo.push_back(outputDataFormat);
    contextStateMapItr->second->pcSamplingConfigurationInfo.push_back(stallReason);

    if (g_verbose)
    {
        std::cout << std::endl;
        std::cout << "============ Configuration Details : ============" << std::endl;
        std::cout << "requested stall reason count : " << numStallReasons << std::endl;
        std::cout << "collection mode              : " << getPcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo[0].attributeData.collectionModeData.collectionMode << std::endl;
        std::cout << "sampling period              : " << getPcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo[1].attributeData.samplingPeriodData.samplingPeriod << std::endl;
        std::cout << "scratch buffer size (Bytes)  : " << getPcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo[2].attributeData.scratchBufferSizeData.scratchBufferSize << std::endl;
        std::cout << "hardware buffer size (Bytes) : " << getPcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo[3].attributeData.hardwareBufferSizeData.hardwareBufferSize << std::endl;
        std::cout << "start stop control           : " << getPcSamplingConfigurationInfoParams.pPCSamplingConfigurationInfo[4].attributeData.enableStartStopControlData.enableStartStopControl << std::endl;
        std::cout << "configuration buffer size    : " << g_pcConfigBufRecordCount << std::endl;
        std::cout << "circular buffer count        : " << g_circularbufCount << std::endl;
        std::cout << "circular buffer record count : " << g_circularbufSize << std::endl;
        std::cout << "sampling duration            : " << g_samplingDuration << std::endl;
        std::cout << "check rsp                    : " << g_checkRSP << std::endl;
        std::cout << "dl backend                   : " << g_backEnd << std::endl;
        std::cout << "prune cct                    : " << g_pruneCCT << std::endl;
        std::cout << "sync before start/stop       : " << g_syncBeforeStart << std::endl;
        std::cout << "backtrace verbose            : " << g_backTraceVerbose << std::endl;
        std::cout << "=================================================" << std::endl;
        std::cout << std::endl;
    }

    return;
}

void AtExitHandler()
{
    // Check for any error occured while pc sampling 
    CUPTI_CALL(cuptiGetLastError());
    if (g_noRPC) g_pcSamplingStarted = false;
    if (g_pcSamplingStarted){
        DEBUG_LOG("waiting for pc sampling stopping\n");
        while (g_pcSamplingStarted);
    }
    DEBUG_LOG("pc sampling stopped\n");

    for(auto& itr: g_contextInfoMap)
    {
        // disable pc sampling at exit
        CUpti_PCSamplingDisableParams pcSamplingDisableParams = {};
        pcSamplingDisableParams.size = CUpti_PCSamplingDisableParamsSize;
        pcSamplingDisableParams.ctx = itr.first;
        CUPTI_CALL(cuptiPCSamplingDisable(&pcSamplingDisableParams));
        DEBUG_LOG("pc sampling disabled for context %u\n", itr.second->contextUid);

    }

    if (g_buffersGetUtilisedFasterThanStore)
    {
        std::cout << "WARNING : Buffers get used faster than get stored in file. Suggestion is either increase size of buffer or increase number of buffers" << std::endl;
    }

    if (g_signalListeningThreadHandle.joinable()) {
        g_signalListeningThreadHandle.join();
        DEBUG_LOG("signal listening thread shutdown\n");
    }

    if (g_noRPC){
        if (g_copyPCSamplesThreadHandle.joinable()) {
            g_copyPCSamplesThreadHandle.join();
            DEBUG_LOG("copy pc samples thread shutdown\n");
        }
        DumpCPUCCT();
        DumpPCSamples(g_pcSampleVector);
    }

    FreePreallocatedMemory();

}

void registerAtExitHandler(void) {
    atexit(&AtExitHandler);
}

#define DUMP_CUBIN 0
#define OFFLINE 0

void CUPTIAPI DumpCudaModule(CUpti_CallbackId cbid, void* resourceDescriptor) {
    const char* pCubin;
    size_t cubinSize;
    uint32_t moduleId;
    CUpti_ModuleResourceData* moduleResourceData = (CUpti_ModuleResourceData*)resourceDescriptor;

    pCubin = moduleResourceData->pCubin;
    cubinSize = moduleResourceData->cubinSize;
    moduleId = moduleResourceData->moduleId;

    if (cbid == CUPTI_CBID_RESOURCE_MODULE_LOADED) {
        char *cubinFileName = (char*)malloc(10 * sizeof(char));
        sprintf(cubinFileName, "%u.cubin", moduleId);
        DEBUG_LOG("module loaded cubinSize=%lu, moduleId=%u, dumping to cubin file: %s\n", cubinSize, moduleId, cubinFileName);
        #if DUMP_CUBIN
        FILE* cubin;
        cubin = fopen(cubinFileName, "wb");
        fwrite(pCubin, sizeof(uint8_t), cubinSize, cubin);
        fclose(cubin);
        #endif
    }
}

void CallbackHandler(void* userdata, CUpti_CallbackDomain domain, CUpti_CallbackId cbid, void* cbdata)
{
    switch (domain)
    {
        case CUPTI_CB_DOMAIN_DRIVER_API:
        {
            const CUpti_CallbackData* cbInfo = (CUpti_CallbackData*)cbdata;

            switch (cbid)
            {
                case CUPTI_DRIVER_TRACE_CBID_cuLaunch:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchGrid:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchGridAsync:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel_ptsz:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel_ptsz:
                case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice:
                {
                    if (cbInfo->callbackSite == CUPTI_API_ENTER)
                    {
                        // DEBUG_LOG("correlation id:%u\n", cbInfo->correlationId);
                        pthread_t tid = pthread_self();
                        if (g_kernelThreadTids.find(tid) == g_kernelThreadTids.end()) {
                            DEBUG_LOG("thread [pthread id=%u] is launching kernel\n", (uint32_t)gettid());
                            g_kernelThreadTids.insert(tid);
                            g_kernelThreadSyncedMap.insert(std::make_pair(tid, false));
                        }
                        // DEBUG_LOG("symbolName=%s, funcName=%s\n", cbInfo->symbolName, cbInfo->functionName);
                        if (g_DoCPUCallstackUnwinding && g_pcSamplingStarted) {
                            DoBackTrace(g_backTraceVerbose);
                        }
                    }
                    if (g_pcSamplingStarted && cbInfo->callbackSite == CUPTI_API_EXIT)
                    {
                        std::map<CUcontext, ContextInfo*>::iterator contextStateMapItr = g_contextInfoMap.find(cbInfo->context);
                        if (contextStateMapItr == g_contextInfoMap.end())
                        {
                            std::cout << "Error : Context not found in map" << std::endl;
                            exit(-1);
                        }
                        if (!contextStateMapItr->second->contextUid)
                        {
                            contextStateMapItr->second->contextUid = cbInfo->contextUid;
                        }
                        // Get PC sampling data from cupti for each range. In such case records will get filled in provided buffer during configuration.
                        // It is recommend to collect those record using cuptiPCSamplingGetData() API.
                        // For _KERNEL_SERIALIZED mode each kernel data is one range.
                        if (g_pcSamplingCollectionMode == CUPTI_PC_SAMPLING_COLLECTION_MODE_KERNEL_SERIALIZED)
                        {
                            // collect all available records.
                            CUpti_PCSamplingGetDataParams pcSamplingGetDataParams = {};
                            pcSamplingGetDataParams.size = CUpti_PCSamplingGetDataParamsSize;
                            pcSamplingGetDataParams.ctx = cbInfo->context;

                            // collect all records filled in provided buffer during configuration.
                            while (contextStateMapItr->second->pcSamplingData.totalNumPcs > 0)
                            {
                                GetPcSamplingDataFromCupti(pcSamplingGetDataParams, contextStateMapItr->second);
                            }
                            // collect if any extra records which could not accommodated in provided buffer during configuration.
                            while (contextStateMapItr->second->pcSamplingData.remainingNumPcs > 0)
                            {
                                GetPcSamplingDataFromCupti(pcSamplingGetDataParams, contextStateMapItr->second);
                            }
                        }
                        else if(contextStateMapItr->second->pcSamplingData.remainingNumPcs >= g_circularbufSize)
                        {
                            CUpti_PCSamplingGetDataParams pcSamplingGetDataParams = {};
                            pcSamplingGetDataParams.size = CUpti_PCSamplingGetDataParamsSize;
                            pcSamplingGetDataParams.ctx = cbInfo->context;

                            GetPcSamplingDataFromCupti(pcSamplingGetDataParams, contextStateMapItr->second);
                        }
                    }
                }
                break;
            }
        }
        break;
        case CUPTI_CB_DOMAIN_RESOURCE:
        {
            const CUpti_ResourceData* resourceData = (CUpti_ResourceData*)cbdata;

            switch(cbid)
            {
                case CUPTI_CBID_RESOURCE_CONTEXT_CREATED:
                {
                    {
                        if (g_verbose)
                        {
                            std::cout << "Injection - Context created" << std::endl;
                        }

                        // insert new entry for context.
                        ContextInfo *contextInfo = (ContextInfo *)calloc(1, sizeof(ContextInfo));
                        MEMORY_ALLOCATION_CALL(contextInfo);
                        g_contextInfoMutex.lock();
                        g_contextInfoMap.insert(std::make_pair(resourceData->context, contextInfo));
                        g_contextInfoMutex.unlock();

                        CUpti_PCSamplingEnableParams pcSamplingEnableParams = {};
                        pcSamplingEnableParams.size = CUpti_PCSamplingEnableParamsSize;
                        pcSamplingEnableParams.ctx = resourceData->context;
                        CUPTI_CALL(cuptiPCSamplingEnable(&pcSamplingEnableParams));
                        
                        ConfigureActivity(resourceData->context);

                        g_circularBufferMutex.lock();
                        if (!g_allocatedCircularBuffers)
                        {
                            PreallocateBuffersForRecords();
                            g_allocatedCircularBuffers = true;
                        }

                        g_circularBufferMutex.unlock();
                    }
                }
                break;
                case CUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING:
                {
                    if (g_verbose)
                    {
                        std::cout << "Injection - Context destroy starting" << std::endl;
                    }
                    std::map<CUcontext, ContextInfo*>::iterator itr;
                    g_contextInfoMutex.lock();
                    itr = g_contextInfoMap.find(resourceData->context);
                    if (itr == g_contextInfoMap.end())
                    {
                        std::cout << "Warning : This context not found in map of context which enabled PC sampling." << std::endl;
                    }
                    g_contextInfoMutex.unlock();

                    CUpti_PCSamplingGetDataParams pcSamplingGetDataParams = {};
                    pcSamplingGetDataParams.size = CUpti_PCSamplingGetDataParamsSize;
                    pcSamplingGetDataParams.ctx = itr->first;

                    while (itr->second->pcSamplingData.remainingNumPcs > 0 || itr->second->pcSamplingData.totalNumPcs > 0)
                    {
                        GetPcSamplingDataFromCupti(pcSamplingGetDataParams, itr->second);
                    }

                    CUpti_PCSamplingDisableParams pcSamplingDisableParams = {};
                    pcSamplingDisableParams.size = CUpti_PCSamplingDisableParamsSize;
                    pcSamplingDisableParams.ctx = resourceData->context;
                    CUPTI_CALL(cuptiPCSamplingDisable(&pcSamplingDisableParams));

                    // It is quite possible that after pc sampling disabled cupti fill remaining records
                    // collected lately from hardware in provided buffer during configuration.
                    if (itr->second->pcSamplingData.totalNumPcs > 0)
                    {
                        g_pcSampDataQueueMutex.lock();
                        g_pcSampDataQueue.push(std::make_pair(&itr->second->pcSamplingData, itr->second));
                        g_pcSampDataQueueMutex.unlock();
                    }

                    g_contextInfoMutex.lock();
                    g_contextInfoToFreeInEndVector.push_back(itr->second);
                    g_contextInfoMap.erase(itr);
                    g_contextInfoMutex.unlock();
                }
                break;
                case CUPTI_CBID_RESOURCE_MODULE_LOADED:
                {
                    #if OFFLINE
                    CUpti_ResourceData* resourceData = (CUpti_ResourceData*)cbdata;
                    DumpCudaModule(cbid, resourceData->resourceDescriptor);
                    #endif
                    g_contextInfoMutex.lock();
                    std::map<CUcontext, ContextInfo*>::iterator contextStateMapItr = g_contextInfoMap.find(resourceData->context);
                    if (contextStateMapItr == g_contextInfoMap.end())
                    {
                        std::cout << "Error : Context not found in map" << std::endl;
                        exit(-1);
                    }
                    g_contextInfoMutex.unlock();
                    // Get PC sampling data from cupti for each range. In such case records will get filled in provided buffer during configuration.
                    // It is recommend to collect those record using cuptiPCSamplingGetData() API.
                    // If module get unloaded then afterwards records will belong to a new range.
                    CUpti_PCSamplingGetDataParams pcSamplingGetDataParams = {};
                    pcSamplingGetDataParams.size = CUpti_PCSamplingGetDataParamsSize;
                    pcSamplingGetDataParams.ctx = resourceData->context;

                    // collect all records filled in provided buffer during configuration.
                    while (contextStateMapItr->second->pcSamplingData.totalNumPcs > 0)
                    {
                        GetPcSamplingDataFromCupti(pcSamplingGetDataParams, contextStateMapItr->second);
                    }
                    // collect if any extra records which could not accommodated in provided buffer during configuration.
                    while (contextStateMapItr->second->pcSamplingData.remainingNumPcs > 0)
                    {
                        GetPcSamplingDataFromCupti(pcSamplingGetDataParams, contextStateMapItr->second);
                    }
                }
                break;
            }
        }
        break;
        default :
            break;
    }
}

static void CopyPCSamplingData(std::vector<CUpti_PCSamplingData*>* v) {
    bool to_break = false;
    DEBUG_LOG("rpc copy thread created\n");
    while (true) {
        if (!g_pcSamplingStarted) {
            DEBUG_LOG("pc sampling stopped, rpc copy about to quit\n");
            to_break = true;
        }
        g_pcSampDataQueueMutex.lock();
        while (!g_pcSampDataQueue.empty()) {
            CUpti_PCSamplingData *pcSampData = g_pcSampDataQueue.front().first;
            v->push_back(pcSampData);
            g_pcSampDataQueue.pop();
            g_bufferEmptyTrackerArray[g_get] = false;
            g_get = (g_get + 1) % g_circularbufCount;
        }
        g_pcSampDataQueueMutex.unlock();
        if (to_break) break;
    }
}

void startCUptiPCSampling() {
    DEBUG_LOG("pc sampling start signal received\n");
    g_contextInfoMutex.lock();
    for (auto& itr: g_contextInfoMap)
    {
        CUcontext ctx = itr.first;
        CUpti_PCSamplingStartParams pcSamplingStartParams = {};
        pcSamplingStartParams.size = CUpti_PCSamplingStartParamsSize;
        pcSamplingStartParams.ctx = ctx;

        DEBUG_LOG("starting pc sampling for context %u\n", itr.second->contextUid);
        CUPTI_CALL(cuptiPCSamplingStart(&pcSamplingStartParams));
    }
    g_contextInfoMutex.unlock();
    g_stopSamplingMutex.lock();
    g_pcSamplingStarted = true;
    DEBUG_LOG("g_pcSamplingStarted set to true\n");
    g_stopSamplingMutex.unlock();
}

void stopCUptiPCSampling() {
    DEBUG_LOG("stop pc sampling signal received\n");
    CollectPCSamples();

    g_contextInfoMutex.lock();
    for (auto& itr: g_contextInfoMap)
    {
        DEBUG_LOG("stopping pc sampling for context %u\n", itr.second->contextUid);
        CUcontext ctx = itr.first;
        CUpti_PCSamplingStopParams pcSamplingStopParams = {};
        pcSamplingStopParams.size = CUpti_PCSamplingStopParamsSize;
        pcSamplingStopParams.ctx = ctx;
        CUPTI_CALL(cuptiPCSamplingStop(&pcSamplingStopParams));
    }
    g_contextInfoMutex.unlock();
    DEBUG_LOG("stop pc sampling finished\n");

    DEBUG_LOG("collecting left pc samples after stop\n");
    CollectPCSamples();

    if (g_buffersGetUtilisedFasterThanStore)
    {
        std::cout << "WARNING : Buffers get used faster than get stored in file. Suggestion is either increase size of buffer or increase number of buffers" << std::endl;
    }

    g_stopSamplingMutex.lock();
    g_pcSamplingStarted = false;
    DEBUG_LOG("g_pcSamplingStarted set to false\n");
    g_stopSamplingMutex.unlock();
}

void genCallStackHanlder(int signum) {
    if (signum == SIGUSR1 && g_genCallStack) {
        DEBUG_LOG("back trace signal received\n");
        GenCallStack(g_callStack);
        g_genCallStack = false;
    }
}

void DoProfiling(){
    auto signalTimer = Timer::GetGlobalTimer("rpc");
    signalTimer->start();

    startCUptiPCSampling();

    g_copyPCSamplesThreadHandle = std::thread(CopyPCSamplingData, &g_pcSampleVector);

    if (g_samplingDuration > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_samplingDuration));
    }

    stopCUptiPCSampling();
    if (g_copyPCSamplesThreadHandle.joinable()) {
        g_copyPCSamplesThreadHandle.join();
    }

    signalTimer->stop();
    DumpCPUCCT();
    DumpPCSamples(g_pcSampleVector);
    DEBUG_LOG("requested duration=%lf, actual processing duration=%lf\n", g_samplingDuration / 1000.0, signalTimer->getAccumulatedTime());
    Timer* genCallStackTimer = Timer::GetGlobalTimer("gen_call_stack");
    DEBUG_LOG("gen callstack overhead: %lf\n", genCallStackTimer->getAccumulatedTime());
    Timer* getProcTimer = Timer::GetGlobalTimer("unwinding_get_proc_name");
    DEBUG_LOG("unwind get proc timer: %lf\n", getProcTimer->getAccumulatedTime());
}

void HandleProfilingSignal(int signum) {
    if (g_signalListeningThreadHandle.joinable()) {
        DEBUG_LOG("last profiling not finished\n");
        return;
    }
    g_signalListeningThreadHandle = std::thread(DoProfiling);
}

extern "C" int InitializeInjection(void)
{
    InitCUptiSettings();
    g_initializeInjectionMutex.lock();
    if (!g_initializedInjection)
    {
        DEBUG_LOG("... Initialize injection ...\n");

        g_circularBuffer.resize(g_circularbufCount);
        g_bufferEmptyTrackerArray.resize(g_circularbufCount, false);

        // CUpti_SubscriberHandle subscriber;
        CUPTI_CALL(cuptiSubscribe(&subscriber, (CUpti_CallbackFunc)&CallbackHandler, NULL));

        // Subscribe for all domains
        CUPTI_CALL(cuptiEnableAllDomains(1, subscriber));

        g_initializedInjection = true;
    }
    
    signal(SIGUSR1, genCallStackHanlder);
    signal(SIGUSR2, HandleProfilingSignal);

    g_mainThreadPid = getpid();
    DEBUG_LOG("main thread pid=%u\n", (uint32_t)g_mainThreadPid);
    g_mainThreadTid = pthread_self();

    if (g_noRPC) {
        g_pcSamplingStarted = true;
        g_copyPCSamplesThreadHandle = std::thread(CopyPCSamplingData, &g_pcSampleVector);
    }

    registerAtExitHandler();
    g_initializeInjectionMutex.unlock();

    return 1;
}

// for debug
int main() {
   InitializeInjection();
}
