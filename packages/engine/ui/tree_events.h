// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "css_atom.h"
#include "events.h"
#include "node_model.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gea::embedded::ui {

// Native target registry operations run on the platform UI owner thread.
// Document listeners use the real off-tree document authority. Capture and callback
// identity are separate from node listener IDs; no body-node proxy is installed.
bool addDocumentEventListener(const char *type, const void *callbackIdentity,
                             gea::framework::events::EventListener listener,
                             bool capture = false, bool once = false);
bool removeDocumentEventListener(const char *type, const void *callbackIdentity,
                                bool capture = false);
bool dispatchDocumentEvent(gea::framework::events::PointerEvent &event, bool capture);
bool documentEventListenersRetiring();

// Process termination only, on the UI owner thread: no event is dispatched.
// Permanently closes Tree node and Document/Window listener admission and
// dispatch, and releases queued captures outside locks. An active callback
// keeps its own capture until returning. Routine Tree.clear resets into a
// successor realm only before process-terminal retirement.
void retireNativeEventRealm() noexcept;

// Window listeners belong to the native app realm, not a fabricated Tree node.
// Callback identity is supplied by the installed typed-callable adapter and is
// retained by the listener itself. Capture is part of registration identity.
bool addWindowEventListener(const char *type, const void *callbackIdentity,
                           gea::framework::events::EventListener listener,
                           bool capture = false, bool once = false);
bool removeWindowEventListener(const char *type, const void *callbackIdentity,
                              bool capture = false);
bool dispatchWindowEvent(gea::framework::events::PointerEvent &event, bool capture);
void resetWindowEventListeners();
bool windowEventListenersRetiring();
bool dispatchWindowLifecycleEvent(gea::framework::events::PointerEventType type);

inline constexpr int kMaxNodeAttributes = 8;
inline constexpr int kNodeAttributeNameMax = 32;
inline constexpr int kNodeAttributeValueMax = 64;

struct NodeAttributeEntry;
struct NodeAttributeDeleter {
	void operator()(NodeAttributeEntry *entry) const;
};
using NodeAttributePtr = std::unique_ptr<NodeAttributeEntry, NodeAttributeDeleter>;
struct NodeAttributeEntry {
	NodeAttributePtr next;
	std::uint8_t nameBytes = 0, valueCapacity = 0;
	// The allocation continues with nameBytes + valueCapacity bytes. Updating
	// another attribute never moves this entry or invalidates its string.
	const char *name() const { return reinterpret_cast<const char *>(this + 1); }
	char *value() { return reinterpret_cast<char *>(this + 1) + nameBytes; }
	const char *value() const { return reinterpret_cast<const char *>(this + 1) + nameBytes; }
	static NodeAttributePtr create(const char *name, const char *value);
};

struct NodeAttributeStore {
#if GEA_UI_NODE_ATTRIBUTES
	int16_t pressId = -1;
	int16_t pressValue = -1;
	CssAtomId idAtom = kInvalidCssAtom;
	uint8_t count = 0;
	// Allocate only attributes that exist. Entries stay at stable addresses
	// as other attributes are added/removed; an empty store allocates nothing.
	NodeAttributePtr values;

	NodeAttributeStore() = default;
	NodeAttributeStore(const NodeAttributeStore &other);
	NodeAttributeStore &operator=(const NodeAttributeStore &other);
	NodeAttributeStore(NodeAttributeStore &&other) noexcept;
	NodeAttributeStore &operator=(NodeAttributeStore &&other) noexcept;

	void clear();
	void set(const char *name, const char *value);
	bool remove(const char *name);
	const char *get(const char *name) const;
	bool has(const char *name) const;
#else
	static constexpr int16_t pressId = -1, pressValue = -1;
	static constexpr CssAtomId idAtom = kInvalidCssAtom;
	void clear() {}
	void set(const char *name, const char *value);
	bool remove(const char *) { return false; }
	const char *get(const char *) const { return ""; }
	bool has(const char *) const { return false; }
#endif
};

using gea::framework::events::EventListenerId;
using gea::framework::events::kInvalidEventListenerId;

struct NodeEventListenerEntry {
	EventListenerId id = kInvalidEventListenerId;
	std::uint8_t type = 0;
	std::string subscribedType;
	std::shared_ptr<gea::framework::events::EventListener> listener;
};

struct NodeEventListeners {
#if GEA_UI_NODE_LISTENERS
	std::vector<NodeEventListenerEntry> entries;
	std::uint16_t types = 0;
#else
	inline static constexpr std::array<NodeEventListenerEntry, 0> entries{};
	static constexpr std::uint16_t types = 0;
#endif

	void clear();
	bool hasType(int type) const { return type >= 0 && (types & (1u << type)); }
	bool hasAny() const { return types != 0; }
};

// NOTE: the NodeRareData block (which bundles attributes + listeners + other
// cold per-node state) is defined in tree_state.h, after the remaining
// cold-state structs (NodeCustomPropertyStore, VirtualListNodeState) it embeds.

}  // namespace gea::embedded::ui
