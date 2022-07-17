#ifndef __CPU_SAMPLER_INCLUDED__
#define __CPU_SAMPLER_INCLUDED__
#include <poll.h>
#include <unistd.h>
#include <memory.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

#include "common.h"

class CPUCallStackSampler {
public:

    struct CallStack {
        uint64_t time;
        uint32_t pid, tid;
        uint64_t depth;
        const uint64_t *pcs;
    };

    CPUCallStackSampler(const CPUCallStackSampler&) = delete;
    CPUCallStackSampler& operator=(const CPUCallStackSampler) = delete;

    ~CPUCallStackSampler();

    // enable/disable sampler
    void EnableSampling();
    void DisableSampling();

    int CollectData(int32_t timeout, uint64_t maxDepth, struct CallStack* callStack);

    explicit CPUCallStackSampler(pid_t pid, uint64_t period, uint64_t pages);

private:
    int fd;
    void* mem;
    uint64_t pages;
    uint64_t offset;
};

CPUCallStackSampler* GetCPUCallStackSampler(pid_t pid);

#endif
