#include <dlfcn.h>
#include <execinfo.h>

#include "utils.h"
#include "cpu_sampler.h"

static std::vector<std::string> GetCallStackSymbols(const uint64_t* stack, uint64_t depth) {
    void* stackPointers[depth];
    char **symbols;
    std::vector<std::string> ret;

    for (int i = 0; i < depth; ++i) {
        stackPointers[i] = (void *)stack[i];
    }
    symbols = backtrace_symbols(stackPointers, depth);

    if (symbols) {
        for (int i = 0; i < depth; ++i) {
            ret.push_back(
                ParseBTSymbol(std::string(symbols[i]))
            );
            //free(symbols[i]);
        }
    }
    free(symbols);

    return ret;
}

static int perf_event_open(struct perf_event_attr* attr,
    pid_t pid, int cpu, int group_fd, uint64_t flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

CPUCallStackSampler::CPUCallStackSampler(pid_t pid, uint64_t period, uint64_t pages) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(struct perf_event_attr));

    attr.size = sizeof(struct perf_event_attr);
    // disable at init time
    attr.disabled = 1;
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.sample_period = period;
    attr.sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN;
    // notify every one overflow
    attr.wakeup_events = 1;

    // open perf fd
    fd = perf_event_open(&attr, pid, -1, -1, 0);
    if (fd < 0) {
        throw std::runtime_error("perf_event_open() failed");
    }

    // create a shared memory to read perf samples from kernel
    mem = mmap(0, (1 + pages) * 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (mem == 0) {
        throw std::runtime_error("mmap() failed");
    }

    this->pages = pages;
    this->offset = 0;
}

CPUCallStackSampler::~CPUCallStackSampler() {
    DisableSampling();
    // unmap shared memory
    munmap(mem, (1 + pages) * 4096);
    // close perf fd
    close(fd);
}

void CPUCallStackSampler::EnableSampling() {
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
}

void CPUCallStackSampler::DisableSampling() {
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
}

int CPUCallStackSampler::CollectData(int32_t timeout, uint64_t maxDepth,
    struct CallStack& callStack) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    uint64_t start = Timer::GetMilliSeconds();
    while (true) {
        uint64_t now = Timer::GetMilliSeconds();
        int32_t toWait;
        if (timeout < 0) {
            toWait = -1;
        } else {
            toWait = timeout - (int32_t)(now - start);
            if (toWait < 0) {
                // timeout
                return -1;
            }
        }

        int ret = poll(&pfd, 1, toWait);
        if (ret == 0) {
            return -1;
        } else if (ret == -1) {
            return errno;
        }

        struct sample {
            struct perf_event_header header;
            uint32_t pid, tid;
            uint64_t time;
            uint64_t nr;
            uint64_t pcs[0];
        }* sample = (struct sample*)((uint8_t*)mem + 4096 + offset);

        struct perf_event_mmap_page* info = (struct perf_event_mmap_page*)mem;
        offset = info->data_head % (pages * 4096);
        if (sample->header.type != PERF_RECORD_SAMPLE) {
            continue;
        }

        callStack.time = sample->time;
        callStack.pid = sample->pid;
        callStack.tid = sample->tid;
        callStack.depth = min2(maxDepth, sample->nr);
        callStack.pcs = sample->pcs;
        callStack.fnames = GetCallStackSymbols(callStack.pcs, callStack.depth);

        return 0;
    }
}

CPUCallStackSamplerCollection::~CPUCallStackSamplerCollection() {
    for (auto itr: samplers) {
        delete itr.second;
    }
}

void CPUCallStackSamplerCollection::RegisterSampler(pid_t pid) {
    if (samplers.find(pid) == samplers.end()) {
        auto sampler = GetCPUCallStackSampler(pid);
        samplers.insert(std::make_pair(pid, sampler));
    }
}

void CPUCallStackSamplerCollection::DeleteSampler(pid_t pid) {
    auto itr = samplers.find(pid);
    if (itr != samplers.end()) {
        delete itr->second;
        samplers.erase(pid);
    }
    else {
        DEBUG_LOG("sampler %d does not exist\n", pid);
    }
}

void CPUCallStackSamplerCollection::EnableSampling() {
    statusMutex.lock();
    for (auto itr: samplers) {
        itr.second->EnableSampling();
    }
    running = true;
    statusMutex.unlock();
}

void CPUCallStackSamplerCollection::DisableSampling() {
    statusMutex.lock();
    for (auto itr: samplers) {
        itr.second->DisableSampling();
    }
    running = false;
    statusMutex.unlock();
}

bool CPUCallStackSamplerCollection::IsRunning() {
    return running;
}

std::unordered_map<pid_t, CPUCallStackSampler::CallStack>
CPUCallStackSamplerCollection::CollectData() {
    statusMutex.lock();
    std::unordered_map<pid_t, CPUCallStackSampler::CallStack> ret;
    for (auto itr: samplers) {
        CPUCallStackSampler::CallStack callStack;
        itr.second->CollectData(
            GetProfilerConf()->cpuSamplingTimeout,
            GetProfilerConf()->cpuSamplingMaxDepth,
            callStack
        );
        ret.insert(
            std::make_pair(itr.first, callStack)
        );
    }
    statusMutex.unlock();

    return ret;
}

CPUCallStackSampler* GetCPUCallStackSampler(pid_t pid) {
    auto profilerConf = GetProfilerConf();
    static std::unordered_map<pid_t, CPUCallStackSampler*> samplerMap;
    if (samplerMap.find(pid) != samplerMap.end()) {
        return samplerMap[pid];
    } else {
        samplerMap.insert(
            std::make_pair(pid, new CPUCallStackSampler(
                pid, profilerConf->cpuSamplingPeriod, profilerConf->cpuSamplingPages))
        );
        return samplerMap[pid];
    }
}
