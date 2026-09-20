#pragma once

namespace gea::platform::i2c {

class Bus {
public:
	using NativeHandle = void *;

	static Bus primary();

	bool available() const { return handle_ != nullptr; }
	NativeHandle nativeHandle() const { return handle_; }

private:
	explicit Bus(NativeHandle handle) : handle_(handle) {}

	NativeHandle handle_ = nullptr;
};

}  // namespace gea::platform::i2c
