#include "cpu_sampler.h"

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
    struct CallStack* callStack) {
    if (callStack == 0) {
        throw std::runtime_error("arg <callStack> is null");
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    //TODO collect sample data
    //TODO update cpu cct
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
