#define UNW_LOCAL_ONLY
#ifndef __BT_INCLUDED__
#define __BT_INCLUDED__
#include <mutex>

#include <cxxabi.h>
#include <Python.h>
#include <libunwind.h>
#include <frameobject.h>

#include "utils.h"
#include "common.h"
#include "cpu_sampler.h"
#include "calling_ctx_tree.h"

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

static void pyBackTrace(std::queue<UNWValue>& pyFrameQueue) {
    DEBUG_LOG("[py back trace] entered\n");
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

class BackTracer {
public:
    BackTracer(const BackTracer&) = delete;
    BackTracer& operator=(const BackTracer) = delete;

    static BackTracer* GetBackTracerSingleton();
    CallStackStatus GenCallStack(std::stack<UNWValue> &q, bool verbose=false);
    void DoBackTrace(bool verbose);

    void SetCorId2ActivePCID(uint32_t corId) {
        corId2ActivePCIDMap.insert(std::make_pair(corId, activeCPUPCID));
        DEBUG_LOG("corId %u --> active PC ID %lu\n", corId, activeCPUPCID);
    }

    bool handlingRemoteUnwinding;
    std::stack<UNWValue> g_callStack;


protected:
    explicit BackTracer(ProfilerConf* _profilerConf);

private:
    ProfilerConf* profilerConf;
    
    std::unordered_map<uint64_t, uint64_t> esp2pcIdMap;
    CCTMAP_t CPUCCTMap;
    
    std::recursive_mutex activeCPUPCIDMutex;
    unw_word_t activeCPUPCID;
    

    std::mutex CPUCCTNodeIdMutex;
    uint64_t CPUCCTNodeId = 1;

    std::unordered_map<uint32_t, unw_word_t> corId2ActivePCIDMap;

friend class CPUCallStackSampler;
};

BackTracer* GetBackTracer();
#endif