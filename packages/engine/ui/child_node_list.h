// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "node.h"
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace gea::embedded::ui {

// The parent owns the token in its existing rare data. The token has no node
// backpointer; a retained list owns a birth-checked handle, never a raw slot.
struct ChildNodeListIdentity final {};

class ChildNodeListIterator;

class ChildNodeList {
public:
  ChildNodeList() = default;
  const void *identity() const { return identity_.get(); }
  std::shared_ptr<const void> weakIdentityOwner() const { return identity_; }

  std::uint32_t length() const
  {
    std::uint32_t count = 0;
    for (auto child = parent_.firstChildHandle(); child; child = child.nextSiblingHandle()) ++count;
    return count;
  }

  // The caller must already have performed WebIDL unsigned-long conversion.
  // The collection does not conflate that conversion with JS property keys.
  NodeHandle itemAt(std::uint32_t index) const
  {
    if (index > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) return NodeHandle();
    return parent_.childAt(static_cast<int>(index));
  }

  // WebIDL unsigned long converts nonfinite values to 0, truncates toward
  // zero and wraps modulo 2^32. Conversion is bounded before the integer cast.
  NodeHandle item(double index) const
  {
    double integer = std::isfinite(index) ? std::trunc(index) : 0.0;
    double wrapped = std::fmod(integer, 4294967296.0);
    if (wrapped < 0) wrapped += 4294967296.0;
    return itemAt(static_cast<std::uint32_t>(wrapped));
  }

  // Internal absence is explicit. A host adapter must publish undefined for a
  // missing indexed property, unlike item's null, without boxing a typed node.
  std::optional<NodeHandle> indexed(std::uint32_t index) const
  {
    auto child = itemAt(index);
    if (!child) return std::nullopt;
    return child;
  }

  ChildNodeListIterator values() const;

private:
  friend class NodeHandle;
  ChildNodeList(NodeHandle parent, std::shared_ptr<const ChildNodeListIdentity> identity)
    : parent_(std::move(parent)), identity_(std::move(identity)) {}
  NodeHandle parent_;
  std::shared_ptr<const ChildNodeListIdentity> identity_;
};

class ChildNodeListIterator {
public:
  explicit ChildNodeListIterator(ChildNodeList list) : list_(std::move(list)) {}

  std::optional<NodeHandle> next()
  {
    if (done_) return std::nullopt;
    auto value = list_.indexed(index_);
    if (!value) {
      done_ = true;
      return std::nullopt;
    }
    ++index_;
    return value;
  }

private:
  ChildNodeList list_;
  std::uint32_t index_ = 0;
  bool done_ = false;
};

inline ChildNodeListIterator ChildNodeList::values() const { return ChildNodeListIterator(*this); }

} // namespace gea::embedded::ui
