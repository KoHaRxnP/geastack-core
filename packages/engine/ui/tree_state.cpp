// SPDX-License-Identifier: Apache-2.0
#include "tree_state.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <new>
#include <utility>

// TreeState is a single large struct (kMaxNodes-scaled per-node arrays: nodes,
// styles, attributes, class lists, custom props, style overrides, dirty/bounds
// tables) — on the order of ~170 KB. Internal SRAM is the scarce resource on the
// ESP32-S3 (~330 KB, and WiFi/BLE need DMA-capable internal RAM that can't live
// in PSRAM); the default `new` lands this pool in internal RAM, starving the
// radios (WiFi `mem fail` / socket open `sock < 0` during fetches). The tree is
// CPU/cache-accessed only (never DMA, never touched with the flash cache off),
// so it belongs in PSRAM like the framebuffer. Other targets keep plain `new`.
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace gea::embedded::ui {

namespace {

bool isClassWhitespace(char c)
{
	return static_cast<unsigned char>(c) <= ' ';
}

bool validToken(const std::string &token)
{
	if (token.empty()) return false;
	for (char c : token) {
		if (isClassWhitespace(c)) return false;
	}
	return true;
}

std::deque<std::vector<CssAtomId>> &classListOverflowLists()
{
	static std::deque<std::vector<CssAtomId>> lists;
	return lists;
}

std::vector<std::uint16_t> &classListOverflowFreeList()
{
	static std::vector<std::uint16_t> handles;
	return handles;
}

std::vector<CssAtomId> *classListOverflow(std::uint16_t handle)
{
	auto &lists = classListOverflowLists();
	if (handle == NodeClassList::kNoOverflow || static_cast<std::size_t>(handle) >= lists.size()) return nullptr;
	return &lists[handle];
}

const std::vector<CssAtomId> *classListOverflowConst(std::uint16_t handle)
{
	const auto &lists = classListOverflowLists();
	if (handle == NodeClassList::kNoOverflow || static_cast<std::size_t>(handle) >= lists.size()) return nullptr;
	return &lists[handle];
}

std::vector<CssAtomId> *ensureClassListOverflow(NodeClassList &list)
{
	if (auto *existing = classListOverflow(list.overflowHandle)) return existing;
	auto &lists = classListOverflowLists();
	auto &freeList = classListOverflowFreeList();
	std::uint16_t handle = NodeClassList::kNoOverflow;
	if (!freeList.empty()) {
		handle = freeList.back();
		freeList.pop_back();
		lists[handle].clear();
	} else if (lists.size() < NodeClassList::kNoOverflow) {
		handle = static_cast<std::uint16_t>(lists.size());
		lists.emplace_back();
	}
	list.overflowHandle = handle;
	return classListOverflow(handle);
}

void releaseClassListOverflow(NodeClassList &list)
{
	if (list.overflowHandle == NodeClassList::kNoOverflow) return;
	if (auto *overflow = classListOverflow(list.overflowHandle)) overflow->clear();
	classListOverflowFreeList().push_back(list.overflowHandle);
	list.overflowHandle = NodeClassList::kNoOverflow;
}

bool appendClassAtom(NodeClassList &list, CssAtomId atom)
{
	if (atom == kInvalidCssAtom || list.count == 0xFFu) return false;
	if (list.count < NodeClassList::kInlineTokenCount) {
		list.inlineTokens[list.count++] = atom;
		return true;
	}
	auto *overflow = ensureClassListOverflow(list);
	if (!overflow || list.overflowHandle == NodeClassList::kNoOverflow) return false;
	overflow->push_back(atom);
	++list.count;
	return true;
}

void eraseClassAtomAt(NodeClassList &list, std::size_t index)
{
	if (index >= list.count) return;
	if (index < NodeClassList::kInlineTokenCount) {
		const std::size_t inlineEnd = std::min<std::size_t>(list.count, NodeClassList::kInlineTokenCount);
		for (std::size_t i = index + 1; i < inlineEnd; ++i)
			list.inlineTokens[i - 1] = list.inlineTokens[i];
		if (list.count > NodeClassList::kInlineTokenCount) {
			auto *overflow = classListOverflow(list.overflowHandle);
			if (overflow && !overflow->empty()) {
				list.inlineTokens[NodeClassList::kInlineTokenCount - 1] = overflow->front();
				overflow->erase(overflow->begin());
				if (overflow->empty()) releaseClassListOverflow(list);
			}
		} else if (list.count > 0) {
			list.inlineTokens[list.count - 1] = kInvalidCssAtom;
		}
		--list.count;
		return;
	}
	auto *overflow = classListOverflow(list.overflowHandle);
	if (!overflow) return;
	const std::size_t overflowIndex = index - NodeClassList::kInlineTokenCount;
	if (overflowIndex >= overflow->size()) return;
	overflow->erase(overflow->begin() + static_cast<std::ptrdiff_t>(overflowIndex));
	--list.count;
	if (overflow->empty()) releaseClassListOverflow(list);
}

bool classListsEqual(const NodeClassList &a, const NodeClassList &b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0, n = a.size(); i < n; ++i)
		if (a.at(i) != b.at(i)) return false;
	return true;
}

NodeClassList splitClassNameAtoms(const char *className, std::size_t length)
{
	NodeClassList out;
	const char *text = className ? className : "";
	std::size_t i = 0;
	while (i < length) {
		while (i < length && isClassWhitespace(text[i])) ++i;
		const std::size_t start = i;
		while (i < length && !isClassWhitespace(text[i])) ++i;
		if (i <= start) continue;
		const CssAtomId token = internCssAtom(text + start, i - start);
		if (token == kInvalidCssAtom) continue;
		bool seen = false;
		for (std::size_t existingIndex = 0, count = out.size(); existingIndex < count; ++existingIndex) {
			if (out.at(existingIndex) == token) {
				seen = true;
				break;
			}
		}
		if (!seen) appendClassAtom(out, token);
	}
	return out;
}

}  // namespace

NodeClassList::NodeClassList(const NodeClassList &other)
{
	for (std::size_t i = 0, n = other.size(); i < n; ++i)
		appendClassAtom(*this, other.at(i));
}

NodeClassList &NodeClassList::operator=(const NodeClassList &other)
{
	if (this == &other) return *this;
	clear();
	for (std::size_t i = 0, n = other.size(); i < n; ++i)
		appendClassAtom(*this, other.at(i));
	return *this;
}

NodeClassList::NodeClassList(NodeClassList &&other) noexcept
{
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		inlineTokens[i] = other.inlineTokens[i];
	overflowHandle = other.overflowHandle;
	count = other.count;
	other.overflowHandle = kNoOverflow;
	other.count = 0;
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		other.inlineTokens[i] = kInvalidCssAtom;
}

NodeClassList &NodeClassList::operator=(NodeClassList &&other) noexcept
{
	if (this == &other) return *this;
	clear();
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		inlineTokens[i] = other.inlineTokens[i];
	overflowHandle = other.overflowHandle;
	count = other.count;
	other.overflowHandle = kNoOverflow;
	other.count = 0;
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		other.inlineTokens[i] = kInvalidCssAtom;
	return *this;
}

NodeClassList::~NodeClassList()
{
	releaseClassListOverflow(*this);
}

void NodeClassList::clear()
{
	releaseClassListOverflow(*this);
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		inlineTokens[i] = kInvalidCssAtom;
	count = 0;
}

bool NodeClassList::set(const std::string &className)
{
	return set(className.c_str());
}

bool NodeClassList::set(const char *className)
{
	NodeClassList next = splitClassNameAtoms(className, className ? std::strlen(className) : 0);
	if (classListsEqual(*this, next)) return false;
	*this = std::move(next);
	return true;
}

bool NodeClassList::add(const std::string &token)
{
	if (!validToken(token)) return false;
	const CssAtomId atom = internCssAtom(token);
	if (containsAtom(atom)) return false;
	return appendClassAtom(*this, atom);
}

bool NodeClassList::remove(const std::string &token)
{
	if (!validToken(token)) return false;
	const CssAtomId atom = findCssAtom(token);
	if (atom == kInvalidCssAtom) return false;
	for (std::size_t i = 0, n = size(); i < n; ++i) {
		if (at(i) != atom) continue;
		eraseClassAtomAt(*this, i);
		return true;
	}
	return false;
}

bool NodeClassList::toggle(const std::string &token)
{
	if (contains(token)) {
		remove(token);
		return false;
	}
	if (!add(token)) return false;
	return true;
}

bool NodeClassList::toggle(const std::string &token, bool force)
{
	return force ? add(token) || contains(token) : !(remove(token) || !contains(token));
}

bool NodeClassList::contains(const std::string &token) const
{
	if (!validToken(token)) return false;
	return containsAtom(findCssAtom(token));
}

bool NodeClassList::containsAtom(CssAtomId token) const
{
	if (token == kInvalidCssAtom) return false;
	for (std::size_t i = 0, n = size(); i < n; ++i)
		if (at(i) == token) return true;
	return false;
}

CssAtomId NodeClassList::at(std::size_t index) const
{
	if (index >= count) return kInvalidCssAtom;
	if (index < kInlineTokenCount) return inlineTokens[index];
	const auto *overflow = classListOverflowConst(overflowHandle);
	if (!overflow) return kInvalidCssAtom;
	const std::size_t overflowIndex = index - kInlineTokenCount;
	if (overflowIndex >= overflow->size()) return kInvalidCssAtom;
	return (*overflow)[overflowIndex];
}

std::string NodeClassList::value() const
{
	std::string out;
	for (std::size_t i = 0, n = size(); i < n; ++i) {
		const CssAtomId token = at(i);
		if (!out.empty()) out.push_back(' ');
		out += cssAtomText(token);
	}
	return out;
}

namespace {

NodeStyleOverride &styleOverrideAt(NodeStyleOverrideStore &store, std::size_t index)
{
	return store.spilled ? store.spillValues[index] : store.inlineValues[index];
}

void appendStyleOverride(NodeStyleOverrideStore &store, NodeStyleOverride entry)
{
	if (!store.spilled && store.inlineCount < NodeStyleOverrideStore::kInlineCount) {
		store.inlineValues[store.inlineCount++] = entry;
		return;
	}
	if (!store.spilled) {
		store.spillCapacity = NodeStyleOverrideStore::kInlineCount * 2;
		store.spillValues = new NodeStyleOverride[store.spillCapacity];
		for (std::size_t i = 0; i < store.inlineCount; ++i)
			store.spillValues[i] = store.inlineValues[i];
		store.spillCount = store.inlineCount;
		store.spilled = true;
	}
	if (store.spillCount >= store.spillCapacity) {
		const std::size_t nextCapacity = store.spillCapacity ? store.spillCapacity * 2 : NodeStyleOverrideStore::kInlineCount * 2;
		auto *next = new NodeStyleOverride[nextCapacity];
		for (std::size_t i = 0; i < store.spillCount; ++i)
			next[i] = store.spillValues[i];
		delete[] store.spillValues;
		store.spillValues = next;
		store.spillCapacity = nextCapacity;
	}
	store.spillValues[store.spillCount++] = entry;
}

}  // namespace

NodeStyleOverrideStore::NodeStyleOverrideStore(const NodeStyleOverrideStore &other)
{
	for (std::size_t i = 0, n = other.size(); i < n; ++i)
		appendStyleOverride(*this, other.at(i));
}

NodeStyleOverrideStore &NodeStyleOverrideStore::operator=(const NodeStyleOverrideStore &other)
{
	if (this == &other) return *this;
	clear();
	for (std::size_t i = 0, n = other.size(); i < n; ++i)
		appendStyleOverride(*this, other.at(i));
	return *this;
}

NodeStyleOverrideStore::NodeStyleOverrideStore(NodeStyleOverrideStore &&other) noexcept
{
	for (std::uint8_t i = 0; i < kInlineCount; ++i)
		inlineValues[i] = other.inlineValues[i];
	spillValues = other.spillValues;
	spillCount = other.spillCount;
	spillCapacity = other.spillCapacity;
	inlineCount = other.inlineCount;
	spilled = other.spilled;
	other.spillValues = nullptr;
	other.spillCount = 0;
	other.spillCapacity = 0;
	other.inlineCount = 0;
	other.spilled = false;
}

NodeStyleOverrideStore &NodeStyleOverrideStore::operator=(NodeStyleOverrideStore &&other) noexcept
{
	if (this == &other) return *this;
	delete[] spillValues;
	for (std::uint8_t i = 0; i < kInlineCount; ++i)
		inlineValues[i] = other.inlineValues[i];
	spillValues = other.spillValues;
	spillCount = other.spillCount;
	spillCapacity = other.spillCapacity;
	inlineCount = other.inlineCount;
	spilled = other.spilled;
	other.spillValues = nullptr;
	other.spillCount = 0;
	other.spillCapacity = 0;
	other.inlineCount = 0;
	other.spilled = false;
	return *this;
}

NodeStyleOverrideStore::~NodeStyleOverrideStore()
{
	delete[] spillValues;
}

void NodeStyleOverrideStore::clear()
{
	if (spilled)
		spillCount = 0;
	else
		inlineCount = 0;
}

void NodeStyleOverrideStore::set(Property property, int value)
{
	for (std::size_t i = 0, n = size(); i < n; ++i) {
		auto &entry = styleOverrideAt(*this, i);
		if (entry.property != property) continue;
		entry.value = value;
		return;
	}
	appendStyleOverride(*this, {property, value});
}

bool NodeStyleOverrideStore::remove(Property property)
{
	for (std::size_t i = 0, n = size(); i < n; ++i) {
		if (at(i).property != property) continue;
		if (spilled) {
			for (std::size_t j = i + 1; j < spillCount; ++j)
				spillValues[j - 1] = spillValues[j];
			--spillCount;
		} else {
			for (std::size_t j = i + 1; j < inlineCount; ++j)
				inlineValues[j - 1] = inlineValues[j];
			--inlineCount;
		}
		return true;
	}
	return false;
}

const NodeStyleOverride &NodeStyleOverrideStore::at(std::size_t index) const
{
	return spilled ? spillValues[index] : inlineValues[index];
}

void NodeCustomPropertyStore::clear()
{
	values.clear();
}

void NodeCustomPropertyStore::set(const std::string &name, const std::string &value)
{
	if (name.empty()) return;
	set(internCssAtom(name), value);
}

CssAtomId customPropertyValueAtom(const std::string &value)
{
	return value.empty() ? kInvalidCssAtom : internCssAtom(value);
}

void NodeCustomPropertyStore::set(CssAtomId nameId, const std::string &value)
{
	if (nameId == kInvalidCssAtom) return;
	for (auto &entry : values) {
		if (entry.nameId != nameId) continue;
		entry.value = value;
		entry.valueAtom = customPropertyValueAtom(value);
		entry.flags = 0;
		return;
	}
	NodeCustomProperty entry;
	entry.nameId = nameId;
	entry.value = value;
	entry.valueAtom = customPropertyValueAtom(value);
	values.push_back(std::move(entry));
}

void NodeCustomPropertyStore::setColor(CssAtomId nameId,
                                       const std::string &value,
                                       std::int32_t styleColor,
                                       std::int32_t nativeColor,
                                       std::uint8_t alpha)
{
	if (nameId == kInvalidCssAtom) return;
	for (auto &entry : values) {
		if (entry.nameId != nameId) continue;
		entry.value = value;
		entry.valueAtom = customPropertyValueAtom(value);
		entry.colorStyle = styleColor;
		entry.colorNative = nativeColor;
		entry.colorAlpha = alpha;
		entry.flags = static_cast<std::uint8_t>((entry.flags & ~2u) | 1u);
		return;
	}
	NodeCustomProperty entry;
	entry.nameId = nameId;
	entry.value = value;
	entry.valueAtom = customPropertyValueAtom(value);
	entry.colorStyle = styleColor;
	entry.colorNative = nativeColor;
	entry.colorAlpha = alpha;
	entry.flags = 1;
	values.push_back(std::move(entry));
}

void NodeCustomPropertyStore::setLength(CssAtomId nameId,
                                        const std::string &value,
                                        float lengthValue,
                                        std::uint8_t lengthUnit)
{
	if (nameId == kInvalidCssAtom) return;
	for (auto &entry : values) {
		if (entry.nameId != nameId) continue;
		entry.value = value;
		entry.valueAtom = customPropertyValueAtom(value);
		entry.lengthValue = lengthValue;
		entry.lengthUnit = lengthUnit;
		entry.flags = static_cast<std::uint8_t>((entry.flags & ~1u) | 2u);
		return;
	}
	NodeCustomProperty entry;
	entry.nameId = nameId;
	entry.value = value;
	entry.valueAtom = customPropertyValueAtom(value);
	entry.lengthValue = lengthValue;
	entry.lengthUnit = lengthUnit;
	entry.flags = 2;
	values.push_back(std::move(entry));
}

const std::string *NodeCustomPropertyStore::get(const std::string &name) const
{
	if (name.empty()) return nullptr;
	return get(findCssAtom(name));
}

const std::string *NodeCustomPropertyStore::get(CssAtomId nameId) const
{
	if (nameId == kInvalidCssAtom) return nullptr;
	if (const NodeCustomProperty *entry = getEntry(nameId)) return &entry->value;
	return nullptr;
}

const NodeCustomProperty *NodeCustomPropertyStore::getEntry(CssAtomId nameId) const
{
	if (nameId == kInvalidCssAtom) return nullptr;
	for (const auto &entry : values)
		if (entry.nameId == nameId) return &entry;
	return nullptr;
}

// File-scope lazy pointer rather than a function-local static. On this Xtensa
// toolchain the static-local guard is NOT inlined, so a Meyers singleton pays a
// __cxa_guard_acquire CALL on every access — and treeState() sits on the hottest
// style-recompute path (called per rule, per node). UI state is single-threaded
// (the mount task and the frame task never run concurrently), so the guard is
// unnecessary; a zero-initialized pointer + null check is one load and a branch.
static TreeState *g_treeState = nullptr;

TreeState &treeState()
{
	if (!g_treeState) {
#if defined(ESP_PLATFORM)
		// The tree pool belongs in PSRAM (it's CPU/cache-only, never DMA), so it
		// doesn't eat the scarce internal SRAM the radios need. esp_psram is brought
		// up before app code runs (CONFIG_SPIRAM_BOOT_INIT), so the first access
		// here lands in PSRAM directly.
		void *mem = heap_caps_malloc(sizeof(TreeState), MALLOC_CAP_SPIRAM);
		g_treeState = mem ? new (mem) TreeState() : new TreeState();
#else
		g_treeState = new TreeState();
#endif
	}
	return *g_treeState;
}

}  // namespace gea::embedded::ui
