// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace gea::framework::services::diagnostics_internal {

bool beginLogRing();
void writeLog(const char *data, std::size_t len);
std::size_t oldestLogTotal();
std::size_t latestLogTotal();
int copyLogSince(std::size_t *cursor, char *dst, int cap);
void startServer();

}  // namespace gea::framework::services::diagnostics_internal
