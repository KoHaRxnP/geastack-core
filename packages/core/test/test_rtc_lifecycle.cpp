#include "host/rtc.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
  auto h = gea::host::rtc::create_handle();
  gea::host::RTCPeerConnection pc(h);

  assert(pc.connectionState() == "new");
  assert(pc.iceConnectionState() == "new");

  auto offer = pc.createOffer();
  assert(!offer.empty());

  pc.setLocalDescription(std::string("offer"), offer);
  pc.setRemoteDescription(std::string("answer"), std::string("v=0\r\n..."));
  pc.addIceCandidate(std::string("candidate:1 1 udp 2122252543 192.168.1.1 50000 typ host"),
                     std::string("0"), 0);

  pc.close();
  assert(pc.connectionState() == "closed");

  gea::host::rtc::destroy_handle(h);

  std::puts("rtc lifecycle OK");
  return 0;
}
