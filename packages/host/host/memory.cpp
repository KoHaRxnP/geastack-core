// SPDX-License-Identifier: Apache-2.0
#include "gea/embedded-host.h"
#include "memory.h"

namespace gea::framework::memory {

double MemoryBackend::internalFree() {
  return static_cast<double>(gea::platform::memory::Memory::internalFree());
}

double MemoryBackend::internalLargestFreeBlock() {
  return static_cast<double>(gea::platform::memory::Memory::internalLargestFreeBlock());
}

double MemoryBackend::internalMinimumFree() {
  return static_cast<double>(gea::platform::memory::Memory::internalMinimumFree());
}

double MemoryBackend::psramFree() {
  return static_cast<double>(gea::platform::memory::Memory::psramFree());
}

double MemoryBackend::currentTaskStackHighWaterMark() {
  return static_cast<double>(gea::platform::memory::Memory::currentTaskStackHighWaterMark());
}

double MemoryBackend::geaMainStackBytes() {
  return static_cast<double>(gea::platform::memory::Memory::geaMainStackBytes());
}

double MemoryBackend::geaInitStackBytes() {
  return static_cast<double>(gea::platform::memory::Memory::geaInitStackBytes());
}

double MemoryBackend::appFrameStackWords() {
  return static_cast<double>(gea::platform::memory::Memory::appFrameStackWords());
}

double MemoryBackend::appFrameStackBytes() {
  return static_cast<double>(gea::platform::memory::Memory::appFrameStackBytes());
}

double MemoryBackend::displayFlushConfiguredRows() {
  return static_cast<double>(gea::platform::memory::Memory::displayFlushConfiguredRows());
}

double MemoryBackend::displayFlushConfiguredDepth() {
  return static_cast<double>(gea::platform::memory::Memory::displayFlushConfiguredDepth());
}

double MemoryBackend::displayFlushBufferMaxBytes() {
  return static_cast<double>(gea::platform::memory::Memory::displayFlushBufferMaxBytes());
}

double MemoryBackend::displayFlushRows() {
  return static_cast<double>(gea::platform::memory::Memory::displayFlushRows());
}

double MemoryBackend::displayFlushDepth() {
  return static_cast<double>(gea::platform::memory::Memory::displayFlushDepth());
}

double MemoryBackend::displayFlushBufferBytes() {
  return static_cast<double>(gea::platform::memory::Memory::displayFlushBufferBytes());
}

double MemoryBackend::allocationSramCount() {
  return 0.0;
}

double MemoryBackend::allocationPsramCount() {
  return 0.0;
}

double MemoryBackend::allocationSramBytes() {
  return 0.0;
}

double MemoryBackend::allocationPsramBytes() {
  return 0.0;
}

double MemoryBackend::allocationSramPeakBytes() {
  return 0.0;
}

double MemoryBackend::allocationPsramPeakBytes() {
  return 0.0;
}

}  // namespace gea::framework::memory
