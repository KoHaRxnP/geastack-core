// Actual facade + Context; transport is explicitly injected, with no socket IO.
#include "host/worker.h"
#include "host/websocket.h"
#include "host/websocket_platform.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unistd.h>
using namespace gea::host;
namespace {int opened=0,closed=0;std::unordered_map<NativeWebSocketHandle,int>destroyed;bool terminal=false,reentrantClose=false;struct Counts{int released=0,callbacks=0;};
struct Owner{NativeWebSocketHandle handle=0;Counts*counts;~Owner(){++counts->released;WebSocket socket(handle);(void)socket.url();(void)socket.readyState();websocket::destroy_handle(handle);if(terminal){bool refused=false;try{websocket::create_handle("ws://authored.invalid/socket");}catch(...){refused=true;}assert(refused);auto rejected=std::make_shared<int>(1);std::weak_ptr<int>weak=rejected;websocket::test_set_open_callback(handle,[rejected]{});rejected.reset();assert(weak.expired());}}};}
namespace gea::host::websocket {
void platform_open(NativeWebSocketHandle,const std::string&,const std::string&,const std::string&){++opened;}
void platform_close(NativeWebSocketHandle handle){++closed;if(reentrantClose&&closed==2)destroy_handle(handle);transport_close(handle,1000,"authored close");}
void platform_destroy(NativeWebSocketHandle handle){++destroyed[handle];}
}
int main(int argc,char**argv){assert(argc==2);alarm(6);const std::string mode=argv[1];Counts counts;
 if(mode=="terminal"){
  auto handle=websocket::create_handle("ws://authored.invalid/socket");auto owner=std::make_shared<Owner>();owner->handle=handle;owner->counts=&counts;std::weak_ptr<Owner>weak=owner;
  websocket::test_set_close_callback(handle,[owner,&counts](int,const std::string&){++counts.callbacks;});owner.reset();WebSocket(handle).close();terminal=true;workers::retireMainContext();
  assert(weak.expired()&&counts.released==1&&counts.callbacks==0);assert(destroyed[handle]==1&&opened==1);websocket::destroy_handle(handle);assert(destroyed[handle]==1);
 }else if(mode=="explicit"){
  auto handle=websocket::create_handle("ws://authored.invalid/socket");auto owner=std::make_shared<Owner>();owner->handle=handle;owner->counts=&counts;std::weak_ptr<Owner>weak=owner;websocket::test_set_open_callback(handle,[owner]{});owner.reset();websocket::destroy_handle(handle);assert(weak.expired()&&destroyed[handle]==1);
  auto next=websocket::create_handle("ws://authored.invalid/socket");assert(next!=handle&&opened==2);websocket::destroy_handle(next);workers::retireMainContext();assert(destroyed[handle]==1&&destroyed[next]==1);
 }else if(mode=="replacement"){
  for(int kind=0;kind<4;++kind){
   auto handle=websocket::create_handle("ws://authored.invalid/socket");auto owner=std::make_shared<Owner>();owner->handle=handle;owner->counts=&counts;std::weak_ptr<Owner>weak=owner;
   auto fresh=std::make_shared<int>(1);std::weak_ptr<int>freshWeak=fresh;
   if(kind==0)websocket::set_open_callback(handle,[owner]{});
   if(kind==1)websocket::set_message_callback(handle,[owner](const std::string&){});
   if(kind==2)websocket::set_close_callback(handle,[owner](int,const std::string&){});
   if(kind==3)websocket::set_error_callback(handle,[owner](const std::string&){});
   owner.reset();
   if(kind==0)websocket::set_open_callback(handle,[fresh]{});
   if(kind==1)websocket::set_message_callback(handle,[fresh](const std::string&){});
   if(kind==2)websocket::set_close_callback(handle,[fresh](int,const std::string&){});
   if(kind==3)websocket::set_error_callback(handle,[fresh](const std::string&){});
   fresh.reset();assert(weak.expired()&&freshWeak.expired()&&destroyed[handle]==1);
   auto refused=std::make_shared<int>(2);std::weak_ptr<int>refusedWeak=refused;
   websocket::set_open_callback(handle,[refused]{});websocket::set_message_callback(handle,[refused](const std::string&){});websocket::set_close_callback(handle,[refused](int,const std::string&){});websocket::set_error_callback(handle,[refused](const std::string&){});refused.reset();assert(refusedWeak.expired());
  }
  workers::retireMainContext();
 }else if(mode=="close"||mode=="close-reentrant"){
  reentrantClose=mode=="close-reentrant";
  auto handle=websocket::create_handle("ws://authored.invalid/socket");websocket::test_set_close_callback(handle,[&](int,const std::string&){++counts.callbacks;});WebSocket(handle).close();workers::Context::runMainPending();assert(counts.callbacks==1&&closed>=1);workers::retireMainContext();assert(destroyed[handle]==1);
 }else assert(false);
 std::cout<<"PASS actual main WebSocket facade retirement "<<mode<<"; injected transport/no IO\n";
}
