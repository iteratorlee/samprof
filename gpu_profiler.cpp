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

#include "gpu_profiler.h"

static inline void PrintUNWValue(UNWValue& val) {
    pid_t pid = gettid();
    pthread_t tid = pthread_self();
    DEBUG_LOG("[pid=%u, tid=%u] unwinding: pc=%lx:[%s+%lx]\n", (uint32_t)pid, (uint32_t)tid, val.pc, val.funcName.c_str(), val.offset);
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

static void PrintCCTMap() {
    for (auto itr: g_CPUCCTMap) {
        itr.second->printTree();
    }
}

static CallStackStatus GenCallStack(std::stack<UNWValue> &q, bool verbose=false) {
    #if DEBUG
    Timer* genCallStackTimer = Timer::GetGlobalTimer("gen_call_stack");
    genCallStackTimer->start();
    #endif
    std::queue<UNWValue> pyFrameQueue;
    CallStackStatus status;
    if (GetProfilerConf()->backEnd == "TORCH") pyBackTrace(pyFrameQueue);
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

        if (GetProfilerConf()->backEnd == "TORCH" && std::string(outer_name).find("_PyEval_EvalFrameDefault") != std::string::npos) {
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
    // maintaining a cpu cct for each thread
    pthread_t tid = pthread_self();
    if (g_CPUCCTMap.find(tid) == g_CPUCCTMap.end()) {
        DEBUG_LOG("new CCT, tid=%d\n", gettid());
        CPUCCT* newCCT = new CPUCCT();
        // set a virtual root node of the new added CCT
        CPUCCTNode* vRootNode = new CPUCCTNode();

        g_CPUCCTNodeIdMutex.lock();
        vRootNode->id = g_CPUCCTNodeId;
        ++g_CPUCCTNodeId;
        g_CPUCCTNodeIdMutex.unlock();

        vRootNode->funcName = "thread:" + std::to_string(gettid()) + "::id:" + std::to_string(vRootNode->id);
        vRootNode->pc = 0;
        vRootNode->offset = 0;
        vRootNode->nodeType = CCTNODE_TYPE_CXX;

        newCCT->setRootNode(vRootNode);
        g_CPUCCTMap.insert(std::make_pair(tid, newCCT));
    }

    CPUCCT* cpuCCT = g_CPUCCTMap[tid];

    // if GetProfilerConf()->fakeBT is true, do not perform cpu call stack unwinding
    if (GetProfilerConf()->fakeBT) {
        g_activeCPUPCIDMutex.lock();
        if (verbose) DEBUG_LOG("active pc changed to %lu:%p\n", cpuCCT->root->id, (void *)(cpuCCT->root->pc));
        g_activeCPUPCID = cpuCCT->root->id;
        g_activeCPUPCIDMutex.unlock();
        return;
    }

    // optimization of cpu call stack unwinding: check the rsp register first
    uint64_t rsp;
    getRSP(&rsp);
    if (verbose) DEBUG_LOG("rsp=%p\n", (void *)rsp);
    if (GetProfilerConf()->checkRSP && g_esp2pcIdMap.find(rsp) != g_esp2pcIdMap.end()) {
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

    // if the backend is Pytorch, and current thread has not PyFrame
    // go to the main thread for PyFrame
    if (GetProfilerConf()->doPyUnwinding && status == CALL_STACK_NOT_HAS_PY) {
        DEBUG_LOG("this thread has not PyFrame, going to the main thread\n");
        g_genCallStack = true;
        pthread_kill(GetProfilerConf()->mainThreadTid, SIGUSR1);
        while (g_genCallStack);
        toInsertUNWMain = g_callStack;
        // TODO: clear the stack or not ?
        while (!g_callStack.empty()) g_callStack.pop();
    }

    CPUCCTNode* parentNode = cpuCCT->root;
    while (!toInsertUNW.empty()) {
        UNWValue value;
        TOP2(toInsertUNWMain, toInsertUNW, value);
        CPUCCTNode* childNode = parentNode->getChildbyPC(value.pc);
        if (childNode) {
            parentNode = childNode;
            POP2(toInsertUNWMain, toInsertUNW);
        } else {
            break;
        }
    }

    // the call path has been searched before
    if (toInsertUNW.empty()) {
        g_activeCPUPCIDMutex.lock();
        g_activeCPUPCID = parentNode->id;
        if (verbose) DEBUG_LOG("old pc, active pc changed to %lu:%p\n", parentNode->id, (void *)(parentNode->pc));
        g_activeCPUPCIDMutex.unlock();
    }

    // the call path has unsearched suffix
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
            newNode->funcName = value.fileName + "::" + value.funcName + "_" + std::to_string(value.offset); // + "_" + std::to_string(newNode->id);
        }

        // leaf node
        if (toInsertUNW.size() == 1) {
            g_activeCPUPCIDMutex.lock();
            if (verbose) DEBUG_LOG("active pc changed to %lu:%p\n", newNode->id, (void *)(newNode->pc));
            g_activeCPUPCID = newNode->id;
            g_esp2pcIdMap[rsp] = newNode->id;
            g_activeCPUPCIDMutex.unlock();
        }

        cpuCCT->insertNode(parentNode, newNode);
        parentNode = newNode;
        POP2(toInsertUNWMain, toInsertUNW);
    }
}

static void CopyCPUCCT2ProtoCPUCCT(CPUCCT* cct, CPUCallingContextTree*& tree) {
    if (!cct->root) return;
    tree->set_rootid(cct->root->id);
    tree->set_rootpc(cct->root->pc);
    for (auto node: cct->nodeMap) {
        CPUCallingContextNode protoNode;
        protoNode.set_id(node.first);
        protoNode.set_pc(node.second->pc);
        protoNode.set_parentid(node.second->parentID);
        protoNode.set_parentpc(node.second->parentPC);
        protoNode.set_offset(node.second->offset);
        protoNode.set_funcname(node.second->funcName);
        for (auto id2child: node.second->id2ChildNodes) {
            protoNode.add_childids(id2child.first);
        }
        for (auto pc2child: node.second->pc2ChildNodes) {
            protoNode.add_childpcs(pc2child.first);
        }
        (*(tree->mutable_nodemap()))[node.second->id] = protoNode;
    }
}

static CriticalNodeType IsCriticalNode(CPUCCTNode* node) {
    std::regex torchOPRegex("at::_ops::(\\S+)::call(\\S+)");
    std::regex tfOPRegex("(\\S+)Op(Kernel)?.+::Compute");

    // keep python nodes
    if (node->nodeType == CCTNODE_TYPE_PY) {
        if (node->funcName.find("python3") == std::string::npos) {
            if (node->funcName.find("backward") != std::string::npos) {
                DEBUG_LOG("critical node, kind=backward, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
                return CRITICAL_TYPE_PY_BACKWARD;
            }
            if (node->funcName.find(GetProfilerConf()->pyFileName) != std::string::npos && node->funcName.find("loss") != std::string::npos) {
                DEBUG_LOG("critical node, kind=loss, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
                return CRITICAL_TYPE_PY_LOSS;
            }
            if (node->funcName.find("forward") != std::string::npos) {
                DEBUG_LOG("critical node, kind=forward, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
                return CRITICAL_TYPE_PY_FORWARD;
            }
        }
    }

    // Pytorch OP regex
    std::smatch results;
    if (std::regex_search(node->funcName, results, torchOPRegex)) {
        DEBUG_LOG("critical node, kind=torch regex, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
        return CRITICAL_TYPE_TORCH_OP;
    }

    // TF OP regex
    if (std::regex_search(node->funcName, results, tfOPRegex)) {
        DEBUG_LOG("critical node, kind=tf regex, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
        return CRITICAL_TYPE_TF_OP;
    }
 
    // leaf node
    if (node->childNodes.size() == 0) {
        DEBUG_LOG("critical node, kind=leaf, funcName=%s, id=%lu\n", node->funcName.c_str(), node->id);
        return CRITICAL_TYPE_LEAF;
    }

    return NOT_CRITICAL_NODE;
}

static void PruneTreeRecursively(CPUCCT* newTree, CPUCCT* oldTree, uint64_t currNewNodeId,
                        uint64_t currOldNodeId) {
    auto currNewNode = newTree->nodeMap[currNewNodeId];
    auto currOldNode = oldTree->nodeMap[currOldNodeId];
    for (auto child: currOldNode->childNodes) {
        if (IsCriticalNode(child) != NOT_CRITICAL_NODE) {
            // for continus op calling, keep the first one/merge them?
            if (currOldNode->childNodes.size() == 1
                && IsCriticalNode(currNewNode) == CRITICAL_TYPE_TORCH_OP
                && IsCriticalNode(child) == CRITICAL_TYPE_TORCH_OP
                ) {
                currNewNode->funcName += ("::" + child->funcName.substr(10));
                PruneTreeRecursively(newTree, oldTree, currNewNodeId, child->id);
            } else {
                CPUCCTNode* newChild = new CPUCCTNode();
                CPUCCTNode::copyNodeWithoutRelation(child, newChild);
                newTree->insertNode(currNewNode, newChild, true);
                PruneTreeRecursively(newTree, oldTree, newChild->id, child->id);
            }
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

static void CopyCPUCCT2ProtoCPUCCTV2(GPUProfilingResponse* reply) {
    CCTMAP_t PrunedCPUCCTMap;
    if (GetProfilerConf()->pruneCCT) PruneCPUCCT(PrunedCPUCCTMap);
    else PrunedCPUCCTMap = g_CPUCCTMap;
    for (auto itr: PrunedCPUCCTMap) {
        CPUCCT *cct = itr.second;
        if (!cct->root) return;
        CPUCallingContextTree* tree = reply->add_cpucallingctxtree();
        tree->set_rootid(cct->root->id);
        tree->set_rootpc(cct->root->pc);
        for (auto node: cct->nodeMap) {
            CPUCallingContextNode protoNode;
            protoNode.set_id(node.first);
            protoNode.set_pc(node.second->pc);
            protoNode.set_parentid(node.second->parentID);
            protoNode.set_parentpc(node.second->parentPC);
            protoNode.set_offset(node.second->offset);
            protoNode.set_funcname(node.second->funcName);
            for (auto id2child: node.second->id2ChildNodes) {
                protoNode.add_childids(id2child.first);
            }
            for (auto pc2child: node.second->pc2ChildNodes) {
                protoNode.add_childpcs(pc2child.first);
            }
            (*(tree->mutable_nodemap()))[node.second->id] = protoNode;
        }
    }
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
    g_put = (g_put+1) % GetProfilerConf()->circularbufCount;
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
    for (size_t buffers=0; buffers<GetProfilerConf()->circularbufCount; buffers++)
    {
        g_circularBuffer[buffers].size = sizeof(CUpti_PCSamplingData);
        g_circularBuffer[buffers].collectNumPcs = GetProfilerConf()->circularbufSize;
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
    for (size_t buffers=0; buffers<GetProfilerConf()->circularbufCount; buffers++)
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
        for (uint32_t i = 0; i < GetProfilerConf()->pcConfigBufRecordCount; i++)
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
        for (uint32_t i = 0; i < GetProfilerConf()->pcConfigBufRecordCount; i++)
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
    contextStateMapItr->second->pcSamplingData.collectNumPcs = GetProfilerConf()->pcConfigBufRecordCount;
    contextStateMapItr->second->pcSamplingData.pPcData = (CUpti_PCSamplingPCData *)malloc(GetProfilerConf()->pcConfigBufRecordCount * sizeof(CUpti_PCSamplingPCData));
    MEMORY_ALLOCATION_CALL(contextStateMapItr->second->pcSamplingData.pPcData);
    for (uint32_t i = 0; i < GetProfilerConf()->pcConfigBufRecordCount; i++)
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
    if (GetProfilerConf()->samplingPeriod)
    {
        sampPeriod.attributeData.samplingPeriodData.samplingPeriod = GetProfilerConf()->samplingPeriod;
        pcSamplingConfigurationInfo.push_back(sampPeriod);
    }

    scratchBufferSize.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_SCRATCH_BUFFER_SIZE;
    if (GetProfilerConf()->scratchBufSize)
    {
        scratchBufferSize.attributeData.scratchBufferSizeData.scratchBufferSize = GetProfilerConf()->scratchBufSize;
        pcSamplingConfigurationInfo.push_back(scratchBufferSize);
    }

    hwBufferSize.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_HARDWARE_BUFFER_SIZE;
    if (GetProfilerConf()->hwBufSize)
    {
        hwBufferSize.attributeData.hardwareBufferSizeData.hardwareBufferSize = GetProfilerConf()->hwBufSize;
        pcSamplingConfigurationInfo.push_back(hwBufferSize);
    }

    collectionMode.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_COLLECTION_MODE;
    collectionMode.attributeData.collectionModeData.collectionMode = g_pcSamplingCollectionMode;
    pcSamplingConfigurationInfo.push_back(collectionMode);

    pcSamplingConfigurationInfo.push_back(stallReason);
    pcSamplingConfigurationInfo.push_back(samplingDataBuffer);
    
    enableStartStop.attributeType = CUPTI_PC_SAMPLING_CONFIGURATION_ATTR_TYPE_ENABLE_START_STOP_CONTROL;
    uint32_t enableStartStopControl = GetProfilerConf()->noRPC && !GetProfilerConf()->noSampling ? 0 : 1;
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

    return;
}

// forward declaration
static void RPCCopyTracingData(GPUProfilingResponse* reply);

void AtExitHandler()
{
    // Check for any error occured while pc sampling 
    CUPTI_CALL(cuptiGetLastError());
    if (GetProfilerConf()->noRPC) g_pcSamplingStarted = false;
    if (g_pcSamplingStarted){
        DEBUG_LOG("waiting for pc sampling stopping\n");
        while (g_pcSamplingStarted);
    }
    DEBUG_LOG("profiling stopped\n");

    for(auto& itr: g_contextInfoMap) {
        // disable pc sampling at exit
        CUpti_PCSamplingDisableParams pcSamplingDisableParams = {};
        pcSamplingDisableParams.size = CUpti_PCSamplingDisableParamsSize;
        pcSamplingDisableParams.ctx = itr.first;
        CUPTI_CALL(cuptiPCSamplingDisable(&pcSamplingDisableParams));
        DEBUG_LOG("pc sampling disabled for context %u\n", itr.second->contextUid);
    }

    if (g_buffersGetUtilisedFasterThanStore) {
        std::cout << "WARNING : Buffers get used faster than get stored in file. Suggestion is either increase size of buffer or increase number of buffers" << std::endl;
    }

    if (GetProfilerConf()->noRPC) {
        g_cpuSamplerCollection->DisableSampling();
        if (g_rpcReplyCopyThreadHandle.joinable()) {
            g_rpcReplyCopyThreadHandle.join();
        }
        if (GetProfilerConf()->noSampling) {
            RPCCopyTracingData(g_reply);
        }
        CopyCPUCCT2ProtoCPUCCTV2(g_reply);
        g_reply->set_message("profiling completed");
        if (DumpSamplingResults(*g_reply, GetProfilerConf()->dumpFileName)) {
            DEBUG_LOG("dumping to %s successfully\n", GetProfilerConf()->dumpFileName.c_str());
        } else {
            DEBUG_LOG("dumping to %s failed\n", GetProfilerConf()->dumpFileName.c_str());
        }
    } else {
        server->Shutdown();
        DEBUG_LOG("grpc server shutdown\n");
        if (g_rpcServerThreadHandle.joinable()) {
            g_rpcServerThreadHandle.join();
        }
    }

    FreePreallocatedMemory();
    if (g_cpuSamplerCollection) {
        delete g_cpuSamplerCollection;
    }
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
                        // recording all the threads launching kernels
                        pthread_t tid = pthread_self();
                        if (g_kernelThreadTids.find(tid) == g_kernelThreadTids.end()) {
                            DEBUG_LOG("thread [pthread id=%u] is launching kernel\n", (uint32_t)gettid());
                            g_kernelThreadTids.insert(tid);
                            g_pidt2pthreadt.insert(std::make_pair(gettid(), tid));
                            g_pthreadt2pidt.insert(std::make_pair(tid, gettid()));
                            g_kernelThreadSyncedMap.insert(std::make_pair(tid, false));
                            g_cpuSamplerCollection->RegisterSampler(gettid());
                        }
                        if (GetProfilerConf()->noSampling) {
                            if (GetProfilerConf()->doCPUCallStackUnwinding && g_tracingStarted) {
                                DoBackTrace(GetProfilerConf()->backTraceVerbose);

                                std::string tRecordKey = std::to_string(g_activeCPUPCID) + "::" + cbInfo->symbolName;
                                g_corID2TracingKey.insert(
                                    std::make_pair(cbInfo->correlationId, tRecordKey)
                                );
  
                                auto itr  = g_tracingRecords.find(tRecordKey);
                                if (itr == g_tracingRecords.end()) {
                                    auto tRecord = new CUptiTracingRecord();
                                    tRecord->funcName = cbInfo->symbolName;
                                    tRecord->parentCPUPCID = g_activeCPUPCID;
                                    g_tracingRecords.insert(
                                        std::make_pair(tRecordKey, tRecord)
                                    );
                                    DEBUG_LOG("adding tracing record: %s\n", tRecordKey.c_str());
                                }

                                Timer *timer = Timer::GetGlobalTimer(tRecordKey);
                                timer->start();
                            }
                        } else {
                            if (GetProfilerConf()->doCPUCallStackUnwinding && g_pcSamplingStarted){
                                DoBackTrace(GetProfilerConf()->backTraceVerbose);
                            }
                        }
                    }
                    if (cbInfo->callbackSite == CUPTI_API_EXIT)
                    {
                        if (GetProfilerConf()->noSampling) {
                            if (GetProfilerConf()->doCPUCallStackUnwinding && g_tracingStarted) {
                                std::string tRecordKey;
                                auto itr1 = g_corID2TracingKey.find(cbInfo->correlationId);
                                if (itr1 == g_corID2TracingKey.end()) {
                                    DEBUG_LOG("correlation ID %u not recorded at API_ENTER\n", cbInfo->correlationId);
                                } else {
                                    tRecordKey = itr1->second;
                                }

                                auto itr = g_tracingRecords.find(tRecordKey);
                                if (itr == g_tracingRecords.end()) {
                                    DEBUG_LOG("kernel %s not recorded at API_ENTER\n", tRecordKey.c_str());
                                } else {
                                    Timer *kTimer = Timer::GetGlobalTimer(tRecordKey);
                                    kTimer->stop();
                                    itr->second->duration += kTimer->getElapsedTimeInt();
                                }
                            }
                        } else {
                            if (g_pcSamplingStarted) {
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
                                else if(contextStateMapItr->second->pcSamplingData.remainingNumPcs >= GetProfilerConf()->circularbufSize)
                                {
                                    CUpti_PCSamplingGetDataParams pcSamplingGetDataParams = {};
                                    pcSamplingGetDataParams.size = CUpti_PCSamplingGetDataParamsSize;
                                    pcSamplingGetDataParams.ctx = cbInfo->context;

                                    GetPcSamplingDataFromCupti(pcSamplingGetDataParams, contextStateMapItr->second);
                                }
                            }
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
                        DEBUG_LOG("Injection - Context created\n");

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

                        // raise(SIGUSR1); // DEBUG
                        
                        g_circularBufferMutex.unlock();
                    }
                }
                break;
                case CUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING:
                {
                    DEBUG_LOG("Injection - Context destroy starting");
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

static void RPCCopyTracingData(GPUProfilingResponse* reply) {
    DEBUG_LOG("rpc copy started [tracing]\n");

    gpuprofiling::CUptiPCSamplingData* pcSampDataProto = reply->add_pcsamplingdata();

    pcSampDataProto->set_size(sizeof(CUpti_PCSamplingData));
    pcSampDataProto->set_collectnumpcs(g_tracingRecords.size());
    pcSampDataProto->set_totalsamples(g_tracingRecords.size());
    pcSampDataProto->set_droppedsamples(0);
    pcSampDataProto->set_totalnumpcs(g_tracingRecords.size());
    pcSampDataProto->set_remainingnumpcs(0);
    pcSampDataProto->set_rangeid(0);
    pcSampDataProto->set_nonusrkernelstotalsamples(0);

    for (auto itr: g_tracingRecords) {
        std::string key = itr.first;
        size_t pos = key.find("::");
        if (pos == std::string::npos) {
            DEBUG_LOG("bad format: %s\n", key.c_str());
            continue;
        }

        uint64_t parentCPUPCID = std::stoul(key.substr(0, pos));
        // std::string funcName = key.substr(pos + 2);

        auto tRecord = itr.second;
        auto pcDataProto = pcSampDataProto->add_ppcdata();
        pcDataProto->set_size(sizeof(CUpti_PCSamplingPCData));
        pcDataProto->set_functionname(tRecord->funcName);
        pcDataProto->set_cubincrc(0);
        pcDataProto->set_parentcpupcid(parentCPUPCID);
        pcDataProto->set_cubincrc(0);
        pcDataProto->set_pcoffset(0);
        pcDataProto->set_functionindex(0);
        pcDataProto->set_pad(0);
        pcDataProto->set_stallreasoncount(1);
        auto stallResProto = pcDataProto->add_stallreason();
        stallResProto->set_pcsamplingstallreasonindex(28);
        uint64_t duration = tRecord->duration; //(uint64_t)(tRecord->duration * 100000000);
        stallResProto->set_samples(duration);
        // DEBUG_LOG("[in tracing copy] fn: %s, duration: %lu\n", tRecord->funcName.c_str(), duration);
    }
}

static void RPCCopyPCSamplingData(GPUProfilingResponse* reply) {
    bool to_break = false;
    DEBUG_LOG("rpc copy thread created [sampling]\n");
    while (true) {
        if (!g_pcSamplingStarted) {
            DEBUG_LOG("pc sampling stopped, rpc copy about to quit\n");
            to_break = true;
        }
        g_pcSampDataQueueMutex.lock();
        while (!g_pcSampDataQueue.empty()) {
            CUpti_PCSamplingData *pcSampData = g_pcSampDataQueue.front().first;
            gpuprofiling::CUptiPCSamplingData* pcSampDataProto = reply->add_pcsamplingdata();
            
            pcSampDataProto->set_size(pcSampData->size);
            pcSampDataProto->set_collectnumpcs(pcSampData->collectNumPcs);
            pcSampDataProto->set_totalsamples(pcSampData->totalSamples);
            pcSampDataProto->set_droppedsamples(pcSampData->droppedSamples);
            pcSampDataProto->set_totalnumpcs(pcSampData->totalNumPcs);
            pcSampDataProto->set_remainingnumpcs(pcSampData->remainingNumPcs);
            pcSampDataProto->set_rangeid(pcSampData->rangeId);
            pcSampDataProto->set_nonusrkernelstotalsamples(pcSampData->nonUsrKernelsTotalSamples);
            
            for (int i = 0; i < pcSampData->totalNumPcs; ++i) {
                gpuprofiling::CUptiPCSamplingPCData* pcDataProto = pcSampDataProto->add_ppcdata();
                CUpti_PCSamplingPCData* pcData = &pcSampData->pPcData[i];
                pcDataProto->set_size(pcData->size);
                pcDataProto->set_cubincrc(pcData->cubinCrc);
                pcDataProto->set_pcoffset(pcData->pcOffset);
                pcDataProto->set_functionindex(pcData->functionIndex);
                pcDataProto->set_pad(pcData->pad);
                pcDataProto->set_functionname(std::string(pcData->functionName));
                pcDataProto->set_stallreasoncount(pcData->stallReasonCount);
                pcDataProto->set_parentcpupcid(g_GPUPCSamplesParentCPUPCIDs[pcData]);
                for (int j = 0; j < pcData->stallReasonCount; ++j) {
                    gpuprofiling::PCSamplingStallReason* stallResProto = pcDataProto->add_stallreason();
                    CUpti_PCSamplingStallReason stallRes = pcData->stallReason[j];
                    stallResProto->set_pcsamplingstallreasonindex(stallRes.pcSamplingStallReasonIndex);
                    stallResProto->set_samples(stallRes.samples);
                }
            }

            g_pcSampDataQueue.pop();
            g_bufferEmptyTrackerArray[g_get] = false;
            g_get = (g_get + 1) % GetProfilerConf()->circularbufCount;
        }
        g_pcSampDataQueueMutex.unlock();
        if (to_break) break;
    }
}

static inline bool checkSyncMap() {
    for (auto ts: g_kernelThreadSyncedMap) {
        if (!ts.second) return false;
    }
    return true;
}

void startCUptiPCSamplingHandler(int signum) {
    // start CUPTI pc sampling when receiving signal SIGUSR1
    if (signum == SIGUSR1) {
        // do_backtrace();
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
}

void stopCUptiPCSamplingHandler(int signum) {
    // stop CUPTI pc sampling when receiving signal SIGUSR2
    if (g_pcSamplingStarted && signum == SIGUSR2)
    {
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
}

void startPCThreadSyncHanlder(int signum) {
    if (signum == SIGUSR1 && !g_genCallStack) {
        pthread_t tid = pthread_self();
        DEBUG_LOG("[pid=%u, tid=%u] in start, synchronizing\n", (uint32_t)gettid(), (uint32_t)pthread_self());
        cudaDeviceSynchronize();
        DEBUG_LOG("[pid=%u, tid=%u] in start, synchronized\n", (uint32_t)gettid(), (uint32_t)pthread_self());
        g_kernelThreadSyncedMap[tid] = true;
        if (tid == selectedTid) {
            DEBUG_LOG("[pid=%u, tid=%u] in start, waiting for all threads sync\n", (uint32_t)gettid(), (uint32_t)pthread_self());
            while (!checkSyncMap());
            DEBUG_LOG("[pid=%u, tid=%u] in start, all kernel-launching thread synced\n", (uint32_t)gettid(), (uint32_t)pthread_self());
            startCUptiPCSamplingHandler(signum);
        } else {
            DEBUG_LOG("[pid=%u, tid=%u] in start, thread not selected, waiting for starting\n", (uint32_t)gettid(), (uint32_t)pthread_self());
            while (!g_pcSamplingStarted);
        }
        g_kernelThreadSyncedMap[tid] = false;
        DEBUG_LOG("[pid=%u, tid=%u] PC sampling started, continue launching kernels\n", (uint32_t)gettid(), (uint32_t)pthread_self());
    } else if (g_genCallStack) {
        DEBUG_LOG("back trace signal received\n");
        GenCallStack(g_callStack);
        g_genCallStack = false;
    }
}

void stopPCThreadSyncHandler(int signum) {
    if (signum == SIGUSR2) {
        pthread_t tid = pthread_self();
        DEBUG_LOG("[pid=%u, tid=%u] in stop, synchronizing\n", (uint32_t)gettid(), (uint32_t)pthread_self());
        cudaDeviceSynchronize();
        DEBUG_LOG("[pid=%u, tid=%u] in stop, synchronized\n", (uint32_t)gettid(), (uint32_t)pthread_self());
        g_kernelThreadSyncedMap[tid] = true;
        if (tid == selectedTid) {
            DEBUG_LOG("[pid=%u, tid=%u] in stop, waiting for all threads sync\n", (uint32_t)gettid(), (uint32_t)pthread_self());
            while (!checkSyncMap());
            DEBUG_LOG("[pid=%u, tid=%u] in stop, all kernel-launching thread synced\n", (uint32_t)gettid(), (uint32_t)pthread_self());
            stopCUptiPCSamplingHandler(signum);
        } else {
            DEBUG_LOG("[pid=%u, tid=%u] in stop, thread not selected, waiting for stopping\n", (uint32_t)gettid(), (uint32_t)pthread_self());
            while (g_pcSamplingStarted);
        }
        g_kernelThreadSyncedMap[tid] = false;
        DEBUG_LOG("[pid=%u, tid=%u] PC sampling stopped, continue launching kernels\n", (uint32_t)gettid(), (uint32_t)pthread_self());
    }
}

void UpdateCCT(pid_t pid, CPUCallStackSampler::CallStack callStack) {
    pthread_t tid = g_pidt2pthreadt[pid];
    if (g_CPUCCTMap.find(tid) == g_CPUCCTMap.end()) {
        DEBUG_LOG("new CCT, tid=%d\n", gettid());
        CPUCCT* newCCT = new CPUCCT();
        // set a virtual root node of the new added CCT
        CPUCCTNode* vRootNode = new CPUCCTNode();

        g_CPUCCTNodeIdMutex.lock();
        vRootNode->id = g_CPUCCTNodeId;
        ++g_CPUCCTNodeId;
        g_CPUCCTNodeIdMutex.unlock();

        vRootNode->funcName = "thread:" + std::to_string(pid) + "::id:" + std::to_string(vRootNode->id);
        vRootNode->pc = 0;
        vRootNode->offset = 0;
        vRootNode->nodeType = CCTNODE_TYPE_CXX;

        newCCT->setRootNode(vRootNode);
        g_CPUCCTMap.insert(std::make_pair(tid, newCCT));
    }

    CPUCCT* cpuCCT = g_CPUCCTMap[tid];

    auto parentNode = cpuCCT->root;
    int i;
    for (i = callStack.depth - 1; i >= 0; --i) {
        if (callStack.fnames[i].length() == 0) {
            break;
        }
        if (HasExcludePatterns(callStack.fnames[i])) {
            break;
        }
        std::string funcName = callStack.fnames[i];
        uint64_t pc = callStack.pcs[i];

        auto childNode = parentNode->getChildbyPC(pc);
        if (childNode) {
            parentNode = childNode;
        } else {
            break;
        }
    }
}

void CollectCPUSamplerData() {
    while (g_cpuSamplerCollection->IsRunning()) {
        auto tid2CallStack = g_cpuSamplerCollection->CollectData();
        for (auto itr: tid2CallStack) {
            auto pid = itr.first;
            auto callStack = itr.second;
            UpdateCCT(pid, callStack);
        }
    }
}

class GPUProfilingServiceImpl final: public GPUProfilingService::Service {
    Status PerformGPUProfiling(ServerContext* context, const GPUProfilingRequest* request, GPUProfilingResponse* reply) override {
        auto rpcTimer = Timer::GetGlobalTimer("rpc");
        rpcTimer->start();
        DEBUG_LOG("pc sampling request received, duration=%u\n", request->duration());

        // erasing exited threads
        std::vector<pthread_t> toEraseTids;
        for (auto tid: g_kernelThreadTids) {
            int res_kill = pthread_kill(tid, 0);
            if (res_kill == ESRCH) {
                DEBUG_LOG("thread [pthreadId=%u] does no exist, about to erase\n", (uint32_t)tid);
                toEraseTids.push_back(tid);
            }
        }

        for (auto tid: toEraseTids) {
            auto itr1 = g_kernelThreadTids.find(tid);
            if (itr1 != g_kernelThreadTids.end()) g_kernelThreadTids.erase(itr1);
            auto itr2 = g_kernelThreadSyncedMap.find(tid);
            if (itr2 != g_kernelThreadSyncedMap.end()) g_kernelThreadSyncedMap.erase(itr2);
            auto itr3 = g_pthreadt2pidt.find(tid);
            if (itr3 != g_pthreadt2pidt.end()) {
                g_cpuSamplerCollection->DeleteSampler(itr3->second);
                g_pidt2pthreadt.erase(itr3->second);
                g_pthreadt2pidt.erase(itr3);
            }
        }

        if (GetProfilerConf()->noSampling) {
            g_tracingStarted = true;
        } else {
            for (auto tid: g_kernelThreadTids) {
                selectedTid = tid;
                break;
            }

            if (GetProfilerConf()->syncBeforeStart){
                DEBUG_LOG("selected tid: %u\n", (uint32_t)selectedTid);
                for (auto tid: g_kernelThreadTids) {
                    pthread_kill(tid, SIGUSR1);
                }
            } else {
                startCUptiPCSamplingHandler(SIGUSR1);
            }

            DEBUG_LOG("in rpc server, waiting for pc sampling starting\n");
            while(!g_pcSamplingStarted);
        }

        if (!GetProfilerConf()->noSampling) {
            g_rpcReplyCopyThreadHandle = std::thread(RPCCopyPCSamplingData, reply);
        }

        // enable cpu call stack sampling
        g_cpuSamplerCollection->EnableSampling();

        if (request->duration() > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(request->duration()));
        }
        else
        {
            std::cout << "Duration should be a positive number (larger than 1000 recommended)" << std::endl;
            return Status::CANCELLED;
        }

        // disable cpu call stack sampling
        g_cpuSamplerCollection->DisableSampling();

        if (GetProfilerConf()->noSampling) {
            g_tracingStarted = false;
        } else {
            if (GetProfilerConf()->syncBeforeStart) {
                for (auto tid: g_kernelThreadTids) {
                    pthread_kill(tid, SIGUSR2);
                }
            } else {
                stopCUptiPCSamplingHandler(SIGUSR2);
            }
        }

        if (!GetProfilerConf()->noSampling) {
            if (g_rpcReplyCopyThreadHandle.joinable()) {
                g_rpcReplyCopyThreadHandle.join();
            }
        } else {
            RPCCopyTracingData(reply);
        }
        CopyCPUCCT2ProtoCPUCCTV2(reply);
        reply->set_message("pc sampling completed");
        rpcTimer->stop();
        DEBUG_LOG("requested duration=%lf, actual processing duration=%lf\n", request->duration() / 1000.0, rpcTimer->getAccumulatedTime());
        Timer* genCallStackTimer = Timer::GetGlobalTimer("gen_call_stack");
        DEBUG_LOG("gen callstack overhead: %lf\n", genCallStackTimer->getAccumulatedTime());
        Timer* getProcTimer = Timer::GetGlobalTimer("unwinding_get_proc_name");
        DEBUG_LOG("unwind get proc timer: %lf\n", getProcTimer->getAccumulatedTime());
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

extern "C" int InitializeInjection(void)
{
    g_initializeInjectionMutex.lock();
    if (!g_initializedInjection)
    {
        DEBUG_LOG("... Initialize injection ...\n");

        g_cpuSamplerCollection = new CPUCallStackSamplerCollection();

        g_circularBuffer.resize(GetProfilerConf()->circularbufCount);
        g_bufferEmptyTrackerArray.resize(GetProfilerConf()->circularbufCount, false);

        // CUpti_SubscriberHandle subscriber;
        CUPTI_CALL(cuptiSubscribe(&subscriber, (CUpti_CallbackFunc)&CallbackHandler, NULL));

        // Subscribe for all domains
        CUPTI_CALL(cuptiEnableAllDomains(1, subscriber));

        g_initializedInjection = true;
    }
    
    signal(SIGUSR1, startPCThreadSyncHanlder);
    signal(SIGUSR2, stopPCThreadSyncHandler);

    if (GetProfilerConf()->noRPC){
        g_pcSamplingStarted = true;
        g_cpuSamplerCollection->EnableSampling();
        g_tracingStarted = true;
        g_reply = new GPUProfilingResponse();
        if (!GetProfilerConf()->noSampling) {
            g_rpcReplyCopyThreadHandle = std::thread(RPCCopyPCSamplingData, g_reply);
        }
    } else {
        g_rpcServerThreadHandle = std::thread(RunServer);
    }

    DEBUG_LOG("main thread pid=%u\n", (uint32_t)getpid());
    GetProfilerConf()->mainThreadTid = pthread_self();

    registerAtExitHandler();
    g_initializeInjectionMutex.unlock();

    return 1;
}

int main() {
    InitializeInjection();
    return 0;
}
