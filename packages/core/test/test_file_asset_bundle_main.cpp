#include "platform/asset_bundle.h"
#include <cassert>
#include <fstream>
#include <string>

namespace gea::platform::storage {
bool ensureMounted() { return true; }
} // namespace gea::platform::storage

int main(int argc, char **argv) {
  assert(argc == 2);
  unsigned char first[3]{}, second[6]{};
  const gea::platform::storage::AssetSegment parts[] = {{first, 3},
                                                        {second, 6}};
  auto load = [&] {
    return gea::platform::storage::loadAssetBundle(argv[1], parts, 2, 9,
                                                   0xcbf43926U);
  };
  auto write = [&](const std::string &bytes) {
    std::ofstream(argv[1], std::ios::binary) << bytes;
  };
  write("123456789");
  assert(load());
  assert(std::memcmp(first, "123", 3) == 0);
  assert(std::memcmp(second, "456789", 6) == 0);
  for (const auto *invalid : {"12345678", "1234567890", "923456789"}) {
    write(invalid);
    assert(!load());
    for (auto byte : first)
      assert(byte == 0);
    for (auto byte : second)
      assert(byte == 0);
  }
  std::remove(argv[1]);
  assert(!load());
}
