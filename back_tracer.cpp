#include "back_tracer.h"

void getRSP(uint64_t *rsp) {
  __asm__ __volatile__("mov %%rsp, %0" : "=m"(*rsp)::"memory");
}

BackTracer::BackTracer(ProfilerConf *_profilerConf) {
  profilerConf = _profilerConf;
}

CallStackStatus BackTracer::GenerateCallStack(std::stack<UNWValue> &q,
                                         bool verbose /*=false*/) {
  std::queue<UNWValue> pyFrameQueue;
  CallStackStatus status;
  if (profilerConf->doPyUnwinding)
    pyBackTrace(pyFrameQueue);
  if (pyFrameQueue.size())
    status = CALL_STACK_HAS_PY;
  else
    status = CALL_STACK_NOT_HAS_PY;

  unw_cursor_t cursor;
  unw_context_t context;

  unw_getcontext(&context);
  unw_init_local(&cursor, &context);

  while (unw_step(&cursor) > 0) {
    unw_word_t offset, pc;
    char fname[FUNC_NAME_LENGTH];
    char *outer_name;

    unw_get_reg(&cursor, UNW_REG_IP, &pc);
    unw_get_proc_name(&cursor, fname, sizeof(fname), &offset);
    int status = 99;
    if ((outer_name = abi::__cxa_demangle(fname, nullptr, nullptr, &status)) ==
        0) {
      outer_name = fname;
    }

    // skip cupti-related stack frames
    if (HasExcludePatterns(outer_name))
      continue;

    if (profilerConf->doPyUnwinding &&
        std::string(outer_name).find("_PyEval_EvalFrameDefault") !=
            std::string::npos) {
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

  return status;
}

void BackTracer::DoBackTrace(bool verbose) {
#if DEBUG
  Timer *timer = Timer::GetGlobalTimer("back_tracer");
  timer->start();
#endif
  pthread_t tid = pthread_self();
  if (CPUCCTMap.find(tid) == CPUCCTMap.end()) {
    DEBUG_LOG("new CCT, tid=%d\n", gettid());
    CPUCCT *newCCT = new CPUCCT();
    CPUCCTNode *vRootNode = new CPUCCTNode();

    CPUCCTNodeIdMutex.lock();
    vRootNode->id = CPUCCTNodeId;
    ++CPUCCTNodeId;
    CPUCCTNodeIdMutex.unlock();

    vRootNode->funcName = "thread:" + std::to_string(gettid()) +
                          "::id:" + std::to_string(vRootNode->id);
    vRootNode->pc = 0;
    vRootNode->offset = 0;
    vRootNode->nodeType = CCTNODE_TYPE_CXX;

    newCCT->setRootNode(vRootNode);
    CPUCCTMap.insert(std::make_pair(tid, newCCT));
  }

  CPUCCT *cpuCCT = CPUCCTMap[tid];

  // If GetProfilerConf()->fakeBT is true, do not perform cpu
  // call stack unwinding.
  if (GetProfilerConf()->fakeBT) {
    activeCPUPCIDMutex.lock();
    if (verbose) {
      DEBUG_LOG("active PC changed to %lu:%p\n", cpuCCT->root->id,
                (void *)(cpuCCT->root->pc));
    }
    activeCPUPCID = cpuCCT->root->id;
    activeCPUPCIDMutex.unlock();
    return;
  }

  // Optimization of cpu call stack unwinding: check the rsp register first.
  uint64_t rsp;
  getRSP(&rsp);
  if (verbose)
    DEBUG_LOG("rsp=%p\n", (void *)rsp);
  if (profilerConf->checkRSP && esp2pcIdMap.find(rsp) != esp2pcIdMap.end()) {
    uint64_t pcId = esp2pcIdMap[rsp];
    activeCPUPCIDMutex.lock();
    activeCPUPCID = pcId;
    activeCPUPCIDMutex.unlock();
    DEBUG_LOG("already unwound, active pc id changed to %lu\n", pcId);
    return;
  }

  // nodes to be inserted to the cpu calling context tree
  std::stack<UNWValue> toInsertUNW;
  std::stack<UNWValue> toInsertUNWMain;

  auto status = GenerateCallStack(toInsertUNW, verbose);

  // if the backend is Pytorch, and current thread has not PyFrame
  // go to the main thread for PyFrame
  if (profilerConf->doPyUnwinding && status == CALL_STACK_NOT_HAS_PY) {
    DEBUG_LOG("this thread has not PyFrame, going to the main thread\n");
    handlingRemoteUnwinding = true;
    pthread_kill(profilerConf->mainThreadTid, SIGUSR1);
    while (handlingRemoteUnwinding) {
    }
    toInsertUNWMain = g_callStack;
    // TODO: clear the stack or not ?
    while (!g_callStack.empty())
      g_callStack.pop();
  }



  CPUCCTNode *parentNode = cpuCCT->root;
  while (!toInsertUNW.empty()) {
    UNWValue value;
    TOP2(toInsertUNWMain, toInsertUNW, value);
    CPUCCTNode *childNode = parentNode->getChildbyPC(value.pc);
    // if finding a CXX node with _PyEval_EvalFrameDefault (C2P node)
    //     replace it with PY node
    if (childNode) {
      if (childNode->nodeType == CCTNODE_TYPE_C2P) {
        if (value.nodeType == CCTNODE_TYPE_PY) {
          childNode->nodeType = CCTNODE_TYPE_PY;
          childNode->funcName = value.funcName;
          DEBUG_LOG("py node renamed in unwinding: %s\n",
                    value.funcName.c_str());
        } else {
          DEBUG_LOG("wrong cct node type matching: %d/%d\n",
                    childNode->nodeType, value.nodeType);
        }
      }
      parentNode = childNode;
      POP2(toInsertUNWMain, toInsertUNW);
    } else {
      break;
    }
  }

  // the call path has been searched before
  if (toInsertUNW.empty()) {
    activeCPUPCIDMutex.lock();
    activeCPUPCID = parentNode->id;
    if (verbose)
      DEBUG_LOG("old pc, active pc changed to %lu:%p\n", parentNode->id,
                (void *)(parentNode->pc));
    activeCPUPCIDMutex.unlock();
  }

  // the call path has unsearched prefix
  while (!toInsertUNW.empty()) {
    UNWValue value;
    TOP2(toInsertUNWMain, toInsertUNW, value);
    CPUCCTNode *newNode = new CPUCCTNode(value.nodeType);

    newNode->pc = value.pc;
    newNode->offset = value.offset;

    CPUCCTNodeIdMutex.lock();
    newNode->id = CPUCCTNodeId;
    ++CPUCCTNodeId;
    CPUCCTNodeIdMutex.unlock();

    if (value.nodeType == CCTNODE_TYPE_CXX) {
      newNode->funcName = 
          value.funcName; // + "_" + std::to_string(newNode->id);
    } else {
      newNode->funcName = value.fileName + "::" + value.funcName + "_" +
                          std::to_string(value.offset) + "_";
                          // + std::to_string(newNode->id);
    }

    // leaf node
    if (toInsertUNW.size() == 1) {
      activeCPUPCIDMutex.lock();
      if (verbose)
        DEBUG_LOG("active pc changed to %lu:%p\n", newNode->id,
                  (void *)(newNode->pc));
      activeCPUPCID = newNode->id;
      esp2pcIdMap[rsp] = newNode->id;
      activeCPUPCIDMutex.unlock();
    }

    cpuCCT->insertNode(parentNode, newNode);
    parentNode = newNode;
    POP2(toInsertUNWMain, toInsertUNW);
  }
#if DEBUG
  timer->stop();
#endif
}

BackTracer *BackTracer::GetBackTracerSingleton() {
  static auto singleton = new BackTracer(GetProfilerConf());
  return singleton;
}

BackTracer *GetBackTracer() { return BackTracer::GetBackTracerSingleton(); }
