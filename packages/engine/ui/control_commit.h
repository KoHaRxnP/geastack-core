// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "node.h"
#include "selector.h"
#include "tree_internal.h"
#include <string>
namespace gea::embedded::ui {
// User edit transaction, not an attribute observer. A recycled slot cannot
// inherit the allocation captured when editing began.
class ControlCommit {
 NodeHandle owner_;std::string field_="value",initial_;bool edited_=false;
 std::string value()const{const auto*p=owner_.getAttribute(field_);return p?p:"";}
public:
 void begin(int node,const char*field="value"){owner_=NodeHandle(node);field_=field;initial_=owner_.valid()?value():"";edited_=false;}
 bool owns(int node)const{return owner_.valid()&&owner_.id()==node;}
 bool admitted(int node)const{return owns(node)&&!selector::disabled(Tree::instance(),node);}
 void edited(int node){if(admitted(node))edited_=true;}
 // Retire this commit before callbacks; never retain a Tree pointer across
 // listener-driven teardown, replacement or nested focus transitions.
 bool commit(){const int id=owner_.id();if(!admitted(id)){edited_=false;return false;}const auto current=value();const bool changed=edited_&&current!=initial_;initial_=current;edited_=false;if(!changed)return false;
  gea::framework::events::PointerEvent event{};event.type=gea::framework::events::PointerEventType::Change;event.targetId=id;event.cancelable=false;Tree::instance().dispatchEvent(event);return true;}
};
}
