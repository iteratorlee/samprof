#pragma once
#include <cxxabi.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <memory.h>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"

class CPUCallStackSampler {
public:
  struct CallStack {
    uint64_t time;
    uint32_t pid, tid;
    uint64_t depth;
    const uint64_t *pcs;
    std::vector<std::string> fnames;
  };

  explicit CPUCallStackSampler(pid_t pid, uint64_t period, uint64_t pages);
  ~CPUCallStackSampler();

  void EnableSampling();
  void DisableSampling();
  int CollectData(int32_t timeout, uint64_t maxDepth,
                  struct CallStack &callStack);

  CPUCallStackSampler(const CPUCallStackSampler &) = delete;
  CPUCallStackSampler &operator=(const CPUCallStackSampler) = delete;
private:
  int fd;
  void *mem;
  uint64_t pages;
  uint64_t offset;
};

CPUCallStackSampler *GetOrCreateCPUCallStackSampler(pid_t pid);

class CPUCallStackSamplerCollection {
public:
  CPUCallStackSamplerCollection(){};
  ~CPUCallStackSamplerCollection();

  void RegisterSampler(pid_t pid);
  void DeleteSampler(pid_t pid);
  void EnableSampling();
  void DisableSampling();
  bool IsRunning();

  std::unordered_map<pid_t, CPUCallStackSampler::CallStack> CollectData();

  CPUCallStackSamplerCollection(const CPUCallStackSamplerCollection &) = delete;
  CPUCallStackSamplerCollection &
  operator=(const CPUCallStackSamplerCollection) = delete;
private:
  std::unordered_map<pid_t, CPUCallStackSampler *> samplers;
  bool running;
  std::mutex statusMutex;
};

static std::string ParseBTSymbol(std::string rawStr) {
  std::string s;
  if (rawStr.length() && rawStr[0] != '[') {
    auto pos1 = rawStr.find('(');
    auto pos2 = rawStr.find('+');
    if (pos2 - pos1 > 1) {
      auto rawFuncName = rawStr.substr(pos1 + 1, pos2 - pos1 - 1);
      char *realFuncName;
      int status = 99;
      if ((realFuncName = abi::__cxa_demangle(rawFuncName.c_str(), nullptr,
                                              nullptr, &status)) != 0) {
        s = std::string(realFuncName);
      } else {
        s = rawFuncName;
      }
    }
  }
  return s;
}
