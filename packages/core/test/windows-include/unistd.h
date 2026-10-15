// <unistd.h> for the native tests on Windows (see ../native-host.sh). The test
// hosts include it; the Windows CRT has no such header. The POSIX calls a test
// could reach are the CRT's, declared in these three -- plus alarm() below.
#pragma once
#include <direct.h>
#include <io.h>
#include <process.h>

#ifdef __cplusplus
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

// The app pipelines arm alarm(5) as a watchdog and catch SIGALRM to fail a
// hung test. Windows has neither: SIGALRM is SIGTERM here, whose handler the
// CRT does run, and alarm() raises it from a timer thread unless re-armed or
// cancelled (alarm(0)) first. It answers 0, as no test reads the remainder.
#ifndef SIGALRM
#define SIGALRM SIGTERM
#endif
inline unsigned alarm(unsigned seconds) {
  static std::atomic<unsigned long long> generation{0};
  const unsigned long long armed = ++generation;
  if (seconds > 0)
    std::thread([armed, seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      if (generation.load() == armed) std::raise(SIGALRM);
    }).detach();
  return 0;
}
#endif
