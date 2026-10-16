#include "host/worker.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
using namespace gea::host::workers;
namespace {
void retire(){
 retireMainContext();
}
struct Counts{int released=0,calls=0;};
struct Owner{Counts*counts;~Owner(){++counts->released;auto context=Context::main();assert(!context->post([c=counts]{++c->calls;}));assert(context->schedule([c=counts]{++c->calls;},0,false)==0);context->setPump([c=counts]{++c->calls;});context->setErrorHandler([c=counts](const std::string&){++c->calls;});bool refused=false;try{context->addCleanup([c=counts]{++c->calls;});}catch(...){refused=true;}assert(refused);retire();}};
}
int main(int argc,char**argv){assert(argc==2);alarm(6);const std::string mode=argv[1];
 if(mode=="uncreated"){retire();auto future=Context::main();assert(future->stopped()&&!future->post([]{}));std::cout<<"PASS retirement before main Context construction\n";return 0;}
 auto context=Context::main();Counts counts;
 if(mode=="pending"){
  auto owner=std::make_shared<Owner>();owner->counts=&counts;std::weak_ptr<Owner>weak=owner;
  assert(context->post([owner]{}));assert(context->schedule([owner]{},1000,false)>0);context->setPump([owner]{});context->setErrorHandler([owner](const std::string&){});context->addCleanup([owner]{});owner.reset();retire();assert(weak.expired()&&counts.released==1);context->runPending();assert(counts.calls==0);
 }else if(mode=="active"){
  auto owner=std::make_shared<Owner>();owner->counts=&counts;std::weak_ptr<Owner>weak=owner;
  assert(context->post([owner,&counts,&weak]{++counts.calls;retire();assert(!weak.expired());}));assert(context->post([&]{counts.calls+=100;}));context->setPump([&]{counts.calls+=1000;});owner.reset();context->runPending(1000);assert(counts.calls==1&&weak.expired()&&counts.released==1);
 }else if(mode=="active-pump"){
  auto owner=std::make_shared<Owner>();owner->counts=&counts;std::weak_ptr<Owner>weak=owner;
  assert(context->post([]{}));context->setPump([owner,&weak,&counts]{++counts.calls;retire();assert(!weak.expired());});owner.reset();context->runPending(1000);assert(counts.calls==1&&weak.expired()&&counts.released==1);
 }else if(mode=="cleanup"){
  int cleanups=0;auto owner=std::make_shared<Owner>();owner->counts=&counts;std::weak_ptr<Owner>weak=owner;
  context->addCleanup([&]{++cleanups;});context->addCleanup([owner,&cleanups]{++cleanups;throw std::runtime_error("authored cleanup failure");});context->addCleanup([&]{++cleanups;});owner.reset();retire();assert(cleanups==3&&weak.expired()&&counts.released==1);
 }else if(mode=="late"){
  retire();retire();auto future=Context::main();assert(future==context&&future->stopped());assert(!future->post([]{}));assert(future->schedule([]{},0,false)==0);future->setPump([&]{++counts.calls;});future->runPending();assert(counts.calls==0);
 }else if(mode=="dedicated"){
  retire();auto dedicated=Context::create("authored CPU context");assert(!dedicated->stopped());assert(dedicated->post([&]{++counts.calls;}));assert(dedicated->schedule([&]{++counts.calls;},100000,false)>0);dedicated->runPending();assert(counts.calls==1);dedicated->finish();
 }else assert(false);
 std::cout<<"PASS actual main Context retirement "<<mode<<"; no worker thread/app/audio/network\n";
}
