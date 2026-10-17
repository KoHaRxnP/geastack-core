#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>

namespace gea::framework::identity {
struct Token {};
using Owner = std::shared_ptr<const Token>;
// A copied carrier retains the same occurrence, including copies made before
// the first identity observation. Fresh event carriers have distinct tokens.
class Occurrence {
public:
  Occurrence() = default;
  Occurrence(const Occurrence &other) : owner_(other.owner()) {}
  Occurrence &operator=(const Occurrence &other) {
    if (this != &other) owner_ = other.owner();
    return *this;
  }
  Owner owner() const {
    if (!owner_) owner_ = std::make_shared<Token>();
    return owner_;
  }
  const void *identity() const { return owner().get(); }
private:
  mutable Owner owner_;
};
// Tree ids are reused. Registry entries cover only the current allocation;
// the tree retains live-node tokens until removal/clear. Retained handles
// own retired tokens independently; weak collections never extend that life.
struct NodeRegistry {
  std::mutex mutex;
  std::unordered_map<int, Owner> nodes;
};
inline NodeRegistry &registry() { static NodeRegistry value; return value; }
inline Owner node(int id) {
  if (id < 0) return {};
  auto &r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto found = r.nodes.find(id);
  return found == r.nodes.end() ? Owner{} : found->second;
}
inline bool current(int id, const Owner &owner) {
  if (id < 0 || !owner) return false;
  auto &r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto found = r.nodes.find(id);
  return found != r.nodes.end() && found->second == owner;
}
inline void activate(int id) {
  if (id < 0) return;
  auto &r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.nodes[id] = std::make_shared<Token>();
}
inline void retire(int id) {
  auto &r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.nodes.erase(id);
}
inline void clear() {
  auto &r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.nodes.clear();
}
} // namespace gea::framework::identity
