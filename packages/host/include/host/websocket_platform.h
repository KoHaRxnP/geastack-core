// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/websocket.h"

// Transport callbacks update native state immediately and enqueue script events
// on the socket's creating worker. They never invoke script on a network
// thread.
namespace gea::host::websocket {
bool transport_open(NativeWebSocketHandle);
void transport_message(NativeWebSocketHandle, std::string);
void transport_closing(NativeWebSocketHandle);
void transport_close(NativeWebSocketHandle, int, const std::string &);
void transport_error(NativeWebSocketHandle, const std::string &);
} // namespace gea::host::websocket
