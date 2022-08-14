#pragma once

#include <sys/time.h>
#include <unordered_map>

#include <cassert>
#include <chrono>
#include <string>

class Timer {
public:
  Timer() : accumulated(0){};

  void start() { t_start = std::chrono::system_clock::now(); }

  void stop() {
    auto t_end = std::chrono::system_clock::now();
    elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start)
            .count();
    accumulated += elapsed;
  }

  void reset() { accumulated = 0; }

  uint64_t getElapsedTimeInt() { return elapsed; }

  uint64_t getAccumulatedTimeInt() { return accumulated; }

  double getElapsedTime() {
    auto num = std::chrono::microseconds::period::num;
    auto den = std::chrono::microseconds::period::den;
    return double(elapsed) * num / den;
  }

  double getAccumulatedTime() {
    auto num = std::chrono::microseconds::period::num;
    auto den = std::chrono::microseconds::period::den;
    return double(accumulated) * num / den;
  }

  static Timer *GetGlobalTimer(std::string name) {
    static std::unordered_map<std::string, Timer *> timerMap;
    if (timerMap.find(name) != timerMap.end()) {
      return timerMap[name];
    } else {
      Timer *timer = new Timer();
      timerMap.insert(std::make_pair(name, timer));
      return timer;
    }
  }

  static uint64_t GetMilliSeconds() {
    struct timeval now;
    assert(gettimeofday(&now, 0) == 0);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
  }

private:
  std::chrono::system_clock::time_point t_start;
  int64_t accumulated;
  int64_t elapsed;
};
