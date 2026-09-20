#pragma once

namespace gea::framework::services {

class BluetoothService {
public:
	static void preinitForApp();
	static void notifyConnected();
	static void notifyDisconnected();
	static void notifyBound();
};

}  // namespace gea::framework::services
