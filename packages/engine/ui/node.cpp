// SPDX-License-Identifier: Apache-2.0
#include "node.h"

#include "host/audio.h"
#include "tree_internal.h"

#include <cstring>
#include <utility>

namespace gea::embedded::ui {

bool ClassList::add(const std::string &token) const
{
	if (nodeId_ < 0) return false;
	return Tree::instance().addClass(nodeId_, token);
}

bool ClassList::remove(const std::string &token) const
{
	if (nodeId_ < 0) return false;
	return Tree::instance().removeClass(nodeId_, token);
}

bool ClassList::toggle(const std::string &token) const
{
	if (nodeId_ < 0) return false;
	return Tree::instance().toggleClass(nodeId_, token);
}

bool ClassList::toggle(const std::string &token, bool force) const
{
	if (nodeId_ < 0) return false;
	return Tree::instance().toggleClass(nodeId_, token, force);
}

void ClassList::set(const std::string &className) const
{
	if (nodeId_ < 0) return;
	Tree::instance().setClassName(nodeId_, className);
}

void ClassList::set(const char *className) const
{
	if (nodeId_ < 0) return;
	Tree::instance().setClassName(nodeId_, className);
}

void ClassList::clear() const
{
	if (nodeId_ < 0) return;
	Tree::instance().clearClasses(nodeId_);
}

bool ClassList::contains(const std::string &token) const
{
	if (nodeId_ < 0) return false;
	return Tree::instance().hasClass(nodeId_, token);
}

std::string ClassList::value() const
{
	if (nodeId_ < 0) return std::string();
	return Tree::instance().className(nodeId_);
}

NodeHandle NodeHandle::cloneNode(bool deep) const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().cloneNode(id(), deep));
}

NodeHandle NodeHandle::firstChildHandle() const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().firstChildOf(id()));
}

NodeHandle NodeHandle::nextSiblingHandle() const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().node(id()).next_sibling);
}

void NodeHandle::replaceChildren(const std::vector<NodeHandle> &children) const
{
	if (!valid()) return;
	auto &tree = Tree::instance();
	// Detach existing children (walk from first_child, capturing next before remove).
	int child = tree.node(id()).first_child;
	while (child >= 0) {
		const int nextChild = tree.node(child).next_sibling;
		NodeHandle(child).remove();
		child = nextChild;
	}
	for (const NodeHandle &c : children)
		if (c.valid()) appendChild(c);
}

NodeHandle NodeHandle::childAt(int index) const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().childAt(id(), index));
}

void NodeHandle::appendChild(NodeHandle child) const
{
	if (!valid() || !child.valid()) return;
	Tree::instance().setParent(child.id(), id());
}

void NodeHandle::insertBefore(NodeHandle child, NodeHandle reference) const
{
	if (!valid() || !child.valid()) return;
	Tree::instance().insertBefore(child.id(), id(), reference.valid() ? reference.id() : -1);
}

void NodeHandle::setParent(NodeHandle parent) const
{
	parent.appendChild(*this);
}

void NodeHandle::remove() const
{
	if (!valid()) return;
	Tree::instance().removeNode(id());
}

void NodeHandle::setText(const char *text) const
{
	if (!valid()) return;
	Tree::instance().setText(id(), text);
}

void NodeHandle::setAttribute(const char *name, const char *value) const
{
	if (!valid()) return;
	Tree::instance().setAttribute(id(), name, value);
}

void NodeHandle::removeAttribute(const char *name) const
{
	if (!valid()) return;
	Tree::instance().removeAttribute(id(), name);
}

bool NodeHandle::toggleAttribute(const char *name, bool force) const
{
	if (!valid()) return false;
	return Tree::instance().toggleAttribute(id(), name, force);
}

const char *NodeHandle::getAttribute(const char *name) const
{
	if (!valid()) return "";
	return Tree::instance().getAttribute(id(), name);
}

bool NodeHandle::hasAttribute(const char *name) const
{
	if (!valid()) return false;
	return Tree::instance().hasAttribute(id(), name);
}

void NodeHandle::scrollIntoView() const
{
	if (!valid()) return;
	Tree::instance().scrollIntoView(id());
}

void NodeHandle::focus() const
{
	if (!valid()) return;
	Tree::instance().setActiveInput(id());
}

void NodeHandle::blur() const
{
	if (!valid()) return;
	// Only clear focus if this node is the one holding it: `blur()` on some
	// other element must not steal the caret away from the focused input.
	if (Tree::instance().activeInputId() != id()) return;
	Tree::instance().setActiveInput(-1);
}

bool NodeHandle::play() const
{
	if (!valid() || std::strcmp(tagName(), "audio") != 0) return false;
	return gea::host::HTMLAudioElement(*this).play();
}

void NodeHandle::pause() const
{
	if (!valid() || std::strcmp(tagName(), "audio") != 0) return;
	gea::host::HTMLAudioElement(*this).pause();
}

void NodeHandle::setTagName(const char *tagName) const
{
	if (!valid()) return;
	Tree::instance().setTagName(id(), tagName);
}

const char *NodeHandle::tagName() const
{
	if (!valid()) return "";
	return Tree::instance().tagName(id());
}

// DOM Node.nodeType for a handle: ELEMENT_NODE (1) for any element-like tree
// node, TEXT_NODE (3) for a text node, DOCUMENT_FRAGMENT_NODE (11) for an
// invalid handle (the embedded tree has no fragment node kind, so a real handle
// is never a fragment).
NodeHandle::NodeTypeProperty::operator int() const
{
	if (id < 0) return 11;
	return Tree::instance().node(id).type == NodeType::Text ? 3 : 1;
}

void NodeHandle::setPressId(int pressId) const
{
	if (!valid()) return;
	Tree::instance().setPressId(id(), pressId);
}

void NodeHandle::setPressValue(int pressValue) const
{
	if (!valid()) return;
	Tree::instance().setPressValue(id(), pressValue);
}

gea::framework::events::EventListenerId NodeHandle::addEventListener(const char *type, gea::framework::events::EventListener listener) const
{
	if (!valid()) return gea::framework::events::kInvalidEventListenerId;
	return Tree::instance().setEventListener(id(), type, std::move(listener));
}

bool NodeHandle::removeEventListener(const char *type, gea::framework::events::EventListenerId listenerId) const
{
	if (!valid()) return false;
	return Tree::instance().removeEventListener(id(), type, listenerId);
}

}  // namespace gea::embedded::ui
