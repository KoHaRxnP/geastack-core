#pragma once

#include <string>

namespace gea::framework::services {

class StorageService {
public:
	static bool init();

	// Persistent key/value strings (NVS "gea" namespace on esp32). getString
	// fills buf and returns true iff the key exists; setString persists + commits.
	// Used for device settings such as the default launch app (read at boot).
	static bool getString(const char *key, char *buf, unsigned capacity);
	static bool setString(const char *key, const char *value);

	// Whole-store blob persistence for the app-facing `localStorage` (see
	// gea::host::StorageFacade). The facade keeps the live key/value set in RAM
	// and serializes it to one opaque, binary-safe blob; loadKv restores it at
	// boot (empty `out` if none stored), saveKv persists it (an empty blob clears
	// the slot). Backed by an NVS blob on esp32, RAM elsewhere.
	static bool loadKv(std::string &out);
	static void saveKv(const std::string &blob);
};

}  // namespace gea::framework::services
