#pragma once

namespace gea::chips::es8311 {

class OutputFormat {
public:
	constexpr OutputFormat(int sampleRate, int channels, int bitsPerSample)
	    : sampleRate_(sampleRate), channels_(channels), bitsPerSample_(bitsPerSample) {}

	constexpr int sampleRate() const { return sampleRate_; }
	constexpr int channels() const { return channels_; }
	constexpr int bitsPerSample() const { return bitsPerSample_; }
	constexpr bool isPcm16() const { return (channels_ == 1 || channels_ == 2) && bitsPerSample_ == 16; }

private:
	int sampleRate_;
	int channels_;
	int bitsPerSample_;
};

}  // namespace gea::chips::es8311
