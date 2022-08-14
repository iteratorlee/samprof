#include "common.h"

ProfilerConf *GetProfilerConf() {
  static ProfilerConf *conf = new ProfilerConf();
  return conf;
}