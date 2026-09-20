// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gea::embedded::ui {

// Typed per-store notification registry for the embedded reactive renderer.
//
// Regular (CompiledStore-based) stores carry one SignalHub member; the store's
// write paths (re-lowered method dirty-flush + the dynamic proxy `set` trap)
// call `notify(field)`, and statically-dep'd renderer bindings register their
// scheduling thunk via `subscribe(field, effect)`. This replaces the dynamic
// `_priv` state-record → `direct` map → `gea_cpp_value` handler-list chain for
// those bindings: no record lookups, no boxed handler values — a flat typed
// vector with string keys (codegen literals on the subscribe side).
class SignalHub {
public:
  std::size_t subscribe(const char *field, std::function<void()> effect) {
    const std::size_t id = next_id_++;
    entries_.push_back(Entry{field, id, std::move(effect)});
    return id;
  }

  void unsubscribe(std::size_t id) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].id == id) {
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  void notify(const char *field) const {
    // Index-based: an effect may subscribe more entries while running.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (std::strcmp(entries_[i].field.c_str(), field) == 0 && entries_[i].effect) entries_[i].effect();
    }
  }

  void notify(const std::string &field) const { notify(field.c_str()); }

private:
  struct Entry {
    std::string field;
    std::size_t id;
    std::function<void()> effect;
  };
  std::vector<Entry> entries_;
  std::size_t next_id_ = 1;
};

// Typed reactive cell for the lean `ReactiveComponent` (component-as-store).
//
// A `ReactiveComponent`'s reactive fields compile to `Signal<T>` members on the
// generated instance. Reads convert to `const T&` (so `this->count + 1`,
// `gea_cpp_to_string(this->count)`, `static_cast<double>(this->count)` all work
// unchanged), and writes (`this->count = x`) assign and notify ONLY this field's
// subscribers. The mounted renderer subscribes a typed effect per node that
// reads the field, so a write re-runs exactly the effects that depend on it.
//
// Fully typed: no `gea_cpp_value`, no string-keyed observe, no Proxy. This is the
// lean counterpart to `CompiledStore`'s dynamic per-property observer map.
template <typename T>
class Signal {
public:
  Signal() = default;
  Signal(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

  // Read. Implicit so a `Signal<T>` is a drop-in for a `T` everywhere the
  // generated code reads the field (arithmetic, stringify, casts, comparisons).
  operator const T &() const { return value_; }  // NOLINT(google-explicit-constructor)
  const T &get() const { return value_; }

  // Write + notify. Unchanged-value writes are skipped so a no-op assignment
  // (or a write that recomputes the same value) does not re-render.
  Signal &operator=(const T &next) {
    if (value_ == next) return *this;
    value_ = next;
    notify();
    return *this;
  }
  Signal &operator=(T &&next) {
    if (value_ == next) return *this;
    value_ = std::move(next);
    notify();
    return *this;
  }

  // Comparisons against the underlying value. Needed explicitly for class types:
  // e.g. std::string's operator== is a TEMPLATE, and template argument deduction
  // does not consider Signal's implicit conversion, so `signal == std::string`
  // finds no candidate without these. (Arithmetic types ride the implicit
  // conversion to built-in operators and don't strictly need them.)
  friend bool operator==(const Signal &lhs, const T &rhs) { return lhs.value_ == rhs; }
  friend bool operator==(const T &lhs, const Signal &rhs) { return lhs == rhs.value_; }
  friend bool operator!=(const Signal &lhs, const T &rhs) { return !(lhs.value_ == rhs); }
  friend bool operator!=(const T &lhs, const Signal &rhs) { return !(lhs == rhs.value_); }

  // Register a typed effect. Called once per reactive node by the mounted
  // renderer; the effect re-reads the field and updates its node.
  //
  // Returns a token, exactly like `SignalHub::subscribe` above, so an effect
  // whose node is gone can be released. A renderer that rebuilds a list
  // re-subscribes the SAME element cells (they live in the store and survive
  // the rebuild), so without a release the subscriber list grows without bound
  // AND the previous generation's effects keep writing to node ids the tree has
  // since recycled -- a silent cross-wire, not a crash. Existing callers that
  // ignore the return value are unaffected.
  std::size_t subscribe(std::function<void()> effect) const {
    const std::size_t id = next_id_++;
    subscribers_.push_back(Entry{id, std::move(effect)});
    return id;
  }

  void unsubscribe(std::size_t id) const {
    for (std::size_t i = 0; i < subscribers_.size(); ++i) {
      if (subscribers_[i].id == id) {
        subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  void notify() const {
    // Index-based and size-rechecked, matching `SignalHub::notify`: an effect
    // may subscribe or release entries while it runs.
    for (std::size_t i = 0; i < subscribers_.size(); ++i) {
      if (subscribers_[i].effect) subscribers_[i].effect();
    }
  }

private:
  struct Entry {
    std::size_t id;
    std::function<void()> effect;
  };
  T value_{};
  // `mutable` so `notify()` can stay `const` and so the auto-emitted (and
  // neutralized) `__gea_to_value() const` never forces a non-const path.
  mutable std::vector<Entry> subscribers_;
  mutable std::size_t next_id_ = 1;
};

}  // namespace gea::embedded::ui
