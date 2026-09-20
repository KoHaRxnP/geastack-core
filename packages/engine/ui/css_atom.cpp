// SPDX-License-Identifier: Apache-2.0
#include "css_atom.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

namespace gea::embedded::ui {
namespace {

std::deque<std::string> &atomTable()
{
	static std::deque<std::string> table = {std::string()};
	return table;
}

std::vector<CssAtomId> &atomBuckets()
{
	static std::vector<CssAtomId> buckets(64, kInvalidCssAtom);
	return buckets;
}

std::vector<CssAtomId> &atomNext()
{
	static std::vector<CssAtomId> next = {kInvalidCssAtom};
	return next;
}

std::vector<std::uint32_t> &atomHashes()
{
	static std::vector<std::uint32_t> hashes = {0};
	return hashes;
}

std::uint32_t atomHash(const char *text, std::size_t length)
{
	const unsigned char *s = reinterpret_cast<const unsigned char *>(text ? text : "");
	std::uint32_t hash = 2166136261u;
	for (std::size_t i = 0; i < length; ++i) {
		hash ^= s[i];
		hash *= 16777619u;
	}
	return hash ? hash : 1u;
}

bool atomEquals(const std::string &atom, const char *text, std::size_t length)
{
	return atom.size() == length && std::memcmp(atom.c_str(), text ? text : "", length) == 0;
}

std::size_t nextBucketCount(std::size_t atomCount)
{
	std::size_t count = 64;
	const std::size_t target = std::max<std::size_t>(atomCount * 2, count);
	while (count < target) count <<= 1u;
	return count;
}

void rebuildAtomIndex(std::size_t atomCount)
{
	auto &table = atomTable();
	auto &buckets = atomBuckets();
	auto &next = atomNext();
	auto &hashes = atomHashes();
	buckets.assign(nextBucketCount(atomCount), kInvalidCssAtom);
	next.assign(table.size(), kInvalidCssAtom);
	hashes.resize(table.size(), 0);
	for (std::size_t i = 1; i < table.size(); ++i) {
		if (hashes[i] == 0) hashes[i] = atomHash(table[i].c_str(), table[i].size());
		const std::size_t bucket = hashes[i] & (buckets.size() - 1);
		next[i] = buckets[bucket];
		buckets[bucket] = static_cast<CssAtomId>(i);
	}
}

bool atomIndexNeedsGrow(std::size_t atomCount)
{
	const auto &buckets = atomBuckets();
	return buckets.empty() || atomCount * 4 >= buckets.size() * 3;
}

CssAtomId findCssAtomWithHash(const char *text, std::size_t length, std::uint32_t hash)
{
	const char *s = text ? text : "";
	auto &table = atomTable();
	auto &buckets = atomBuckets();
	auto &next = atomNext();
	auto &hashes = atomHashes();
	if (buckets.empty()) rebuildAtomIndex(table.size());
	const std::size_t bucket = hash & (buckets.size() - 1);
	for (CssAtomId id = buckets[bucket]; id != kInvalidCssAtom; id = next[id]) {
		if (static_cast<std::size_t>(id) >= table.size()) break;
		if (hashes[id] == hash && atomEquals(table[id], s, length)) return id;
	}
	return kInvalidCssAtom;
}

}  // namespace

CssAtomId findCssAtom(const char *text, std::size_t length)
{
	const char *s = text ? text : "";
	if (length == 0) return kInvalidCssAtom;
	return findCssAtomWithHash(s, length, atomHash(s, length));
}

CssAtomId findCssAtom(const std::string &text)
{
	return findCssAtom(text.c_str(), text.size());
}

CssAtomId internCssAtom(const char *text, std::size_t length)
{
	const char *s = text ? text : "";
	if (length == 0) return kInvalidCssAtom;
	const std::uint32_t hash = atomHash(s, length);
	if (const CssAtomId existing = findCssAtomWithHash(s, length, hash)) return existing;
	auto &table = atomTable();
	if (table.size() >= 0xFFFFu) return kInvalidCssAtom;
	table.emplace_back(s, length);
	auto &next = atomNext();
	auto &hashes = atomHashes();
	next.push_back(kInvalidCssAtom);
	hashes.push_back(hash);
	const CssAtomId id = static_cast<CssAtomId>(table.size() - 1);
	if (atomIndexNeedsGrow(table.size())) {
		rebuildAtomIndex(table.size());
		return id;
	}
	auto &buckets = atomBuckets();
	const std::size_t bucket = hash & (buckets.size() - 1);
	next[id] = buckets[bucket];
	buckets[bucket] = id;
	return id;
}

CssAtomId internCssAtom(const char *text)
{
	return internCssAtom(text, text ? std::strlen(text) : 0);
}

CssAtomId internCssAtom(const std::string &text)
{
	return internCssAtom(text.c_str(), text.size());
}

const char *cssAtomText(CssAtomId atom)
{
	auto &table = atomTable();
	if (atom == kInvalidCssAtom || static_cast<std::size_t>(atom) >= table.size()) return "";
	return table[static_cast<std::size_t>(atom)].c_str();
}

}  // namespace gea::embedded::ui
