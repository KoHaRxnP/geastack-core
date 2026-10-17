// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdlib>
#include <type_traits>
namespace gea { template <typename... Arms> class TaggedUnion; }
namespace gea::framework::events {
struct NativeListenerOptions { bool capture=false; bool once=false; };
inline NativeListenerOptions listenerOptions(bool capture) { return {capture,false}; }
template <typename Boolean> bool listenerOptionBoolean(const Boolean &value) {
 if constexpr (std::is_same_v<Boolean,bool>) return value;
 else { static_assert(std::is_same_v<std::remove_cvref_t<decltype(*value)>,bool>); return value.has_value() && *value; }
}
// Exact generated record fields remain native Optional<bool>; never Value/boxing.
template <typename Record> requires requires(const Record &value) { value->capture; value->once; }
NativeListenerOptions listenerOptions(const Record &value) {
 if (!value) return {};
 return {listenerOptionBoolean(value->capture),listenerOptionBoolean(value->once)};
}
template <std::size_t Index=0, typename... Arms>
NativeListenerOptions listenerOptions(const gea::TaggedUnion<Arms...> &value) {
 if (value.template is<Index>()) return listenerOptions(value.template get<Index>());
 if constexpr (Index+1<sizeof...(Arms)) return listenerOptions<Index+1>(value);
 else std::abort();
}
}
