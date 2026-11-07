#include <pthread.h>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "Core/MemMap.h"
#include "SimpleAudioDec.h"
#include "Common/LogReporting.h"
#include "ext/at3_standalone/at3_decoders.h"

inline int16_t clamp16(float f) {
	if (f >= 1.0f)
		return 32767;
	else if (f <= -1.0f)
		return -32767;
	else
		return (int)(f * 32767);
}

// Atrac3+ music decoded a packet ahead, off the CPU's thread where the game's waiting for it: the next packet is where this one ended, nearly
// always. What's decoded is only used if the packet turns out the same, byte for byte. If not, the codec goes back to before and does it as asked.
class Atrac3Audio;
// (Never destroyed: its thread is still waiting on them when the process ends, and destroying a condition variable with a waiter never returns.)
static std::mutex &aheadMutex = *new std::mutex;
static std::condition_variable &aheadWake = *new std::condition_variable;
static std::condition_variable &aheadDone = *new std::condition_variable;
static std::vector<Atrac3Audio *> &aheadQueue = *new std::vector<Atrac3Audio *>;
static bool aheadThreadStarted = false;
static void AheadThread();

// Uses our standalone AT3/AT3+ decoder derived from FFMPEG
// Test case for ATRAC3: Mega Man Maverick Hunter X, PSP menu sound
class Atrac3Audio : public AudioDecoder {
public:
	Atrac3Audio(PSPAudioType audioType, int channels, size_t blockAlign, const uint8_t *extraData, size_t extraDataSize)
		: audioType_(audioType), channels_(channels) {
		blockAlign_ = (int)blockAlign;
		if (audioType_ == PSP_CODEC_AT3) {
			at3Ctx_ = atrac3_alloc(channels, &blockAlign_, extraData, (int)extraDataSize);
			if (at3Ctx_) {
				codecOpen_ = true;
			} else {
				ERROR_LOG(Log::ME, "Failed to open atrac3 context! !channels=%d blockAlign=%d ed=%d)", channels, (int)blockAlign, (int)extraDataSize);
				codecFailed_ = true;
			}
		}
		for (int i = 0; i < 2; i++) {
			buffers_[i] = new float[4096];
		}
	}
	~Atrac3Audio() override {
		WaitAhead();
		for (int i = 0; i < 2; i++) {
			delete[] aheadBuffers_[i];
		}
		if (at3Ctx_) {
			atrac3_free(at3Ctx_);
		}
		if (at3pCtx_) {
			atrac3p_free(at3pCtx_);
		}
		for (int i = 0; i < 2; i++) {
			delete[] buffers_[i];
		}
	}

	bool IsOK() const override {
		return codecOpen_;
	}

	void FlushBuffers() override {
		if (aheadWanted_) {
			WaitAhead();
			aheadWanted_ = false;
		}
		if (at3Ctx_) {
			atrac3_flush_buffers(at3Ctx_);
		}
		if (at3pCtx_) {
			atrac3p_flush_buffers(at3pCtx_);
		}
	}

	bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, int16_t *outbuf, int *outSamples) override {
		if (outSamples)
			*outSamples = 0;
		if (inbytesConsumed)
			*inbytesConsumed = 0;
		if (!codecOpen_) {
			// We delay the codecOpen until the first decode, so the setChannels call from MediaEngine::getAudioSamples
			// can take effect. Note, we don't do this with Atrac3, just Atrac3+.
			if (codecFailed_) {
				return false;
			}
			if (audioType_ == PSP_CODEC_AT3PLUS) {
				at3pCtx_ = atrac3p_alloc(channels_, &blockAlign_);
				if (at3pCtx_) {
					codecOpen_ = true;
				} else {
					ERROR_LOG(Log::ME, "Failed to open atrac3+ context! (channels=%d blockAlign=%d)", channels_, (int)blockAlign_);
					codecFailed_ = true;
				}
			}
			if (!codecOpen_) {
				WARN_LOG_N_TIMES(codecNotOpen, 5, Log::ME, "Atrac3Audio:Decode: Codec not open, not decoding");
				return false;
			}
		}
		if (inbytes != blockAlign_ && blockAlign_ != 0) {
			WARN_LOG(Log::ME, "Atrac3Audio::Decode: inbytes not matching expected blockalign. Updating blockAlign_. Got %d bytes, expected %d. (%s)", inbytes, blockAlign_, at3pCtx_ ? "Atrac3+" : "Atrac3");
		}

		blockAlign_ = inbytes;
		// We just call the decode function directly without going through the whole packet machinery.
		int result;
		int nb_samples = 0;
		if (audioType_ == PSP_CODEC_AT3PLUS) {
			if (!TakeAhead(inbuf, inbytes, &result, &nb_samples)) {
				result = atrac3p_decode_frame(at3pCtx_, buffers_, &nb_samples, inbuf, inbytes);
			}
			if (result > 0) {
				StartAhead(inbuf + inbytes, inbytes);
			}
		} else {
			result = atrac3_decode_frame(at3Ctx_, buffers_, &nb_samples, inbuf, inbytes);
		}
		if (result < 0) {
			// NOTE: Here, to recover from single bad packets, we update inBytesConsumed/outSamples with the regular packet size.
			// Otherwise we might try to decode the same packet over and over.
			// This is seen in some unofficial game mods, mainly.
			if (inbytesConsumed) {
				*inbytesConsumed = inbytes;
			}
			if (outSamples) {
				if (*outSamples != 0) {
					nb_samples = std::min(*outSamples, nb_samples);
				}
				*outSamples = nb_samples;
			}
			return false;
		}
		if (inbytesConsumed) {
			*inbytesConsumed = result;
		}
		if (outSamples) {
			// Allow capping the output samples by setting *outSamples to non-zero.
			if (*outSamples != 0) {
				nb_samples = std::min(*outSamples, nb_samples);
			}
			*outSamples = nb_samples;
		}
		if (nb_samples > 0) {
			if (outSamples) {
				*outSamples = nb_samples;
			}
			if (outbuf) {
				_dbg_assert_(outputChannels == 1 || outputChannels == 2);
				const float *left = buffers_[0];
				if (outputChannels == 2) {
					// Stereo output, standard.
					const float *right = channels_ == 2 ? buffers_[1] : buffers_[0];
					for (int i = 0; i < nb_samples; i++) {
						outbuf[i * 2] = clamp16(left[i]);
						outbuf[i * 2 + 1] = clamp16(right[i]);
					}
				} else {
					// Mono output, just take the left channel.
					for (int i = 0; i < nb_samples; i++) {
						outbuf[i] = clamp16(left[i]);
					}
				}
			}
		}
		return true;
	}

	void SetChannels(int channels) override {
		_dbg_assert_(audioType_ == PSPAudioType::PSP_CODEC_AT3PLUS);
		if (audioType_ == PSPAudioType::PSP_CODEC_AT3PLUS) {
			if (!at3pCtx_) {
				// Codec shouldn't be open yet.
				_dbg_assert_(!codecOpen_);
				channels_ = 1;
			} else {
				// This will come every packet, just ignore it.
			}
		}
	}

	PSPAudioType GetAudioType() const override { return audioType_; }

	// On the thread that decodes ahead.
	void RunAhead() {
		atrac3p_state_save(at3pCtx_, aheadState_.data());
		aheadSamples_ = 0;
		aheadResult_ = atrac3p_decode_frame(at3pCtx_, aheadBuffers_, &aheadSamples_, aheadPacket_.data(), aheadBytes_);
	}

	bool aheadBusy_ = false;

private:
	void WaitAhead() {
		std::unique_lock<std::mutex> lock(aheadMutex);
		aheadDone.wait(lock, [this] { return !aheadBusy_; });
	}

	bool TakeAhead(const uint8_t *inbuf, int inbytes, int *result, int *samples) {
		if (!aheadWanted_)
			return false;
		WaitAhead();
		aheadWanted_ = false;
		if (aheadResult_ > 0 && inbuf == aheadIn_ && inbytes == aheadBytes_ && memcmp(inbuf, aheadPacket_.data(), inbytes) == 0) {
			std::swap(buffers_[0], aheadBuffers_[0]);
			std::swap(buffers_[1], aheadBuffers_[1]);
			*result = aheadResult_;
			*samples = aheadSamples_;
			return true;
		}
		atrac3p_state_restore(at3pCtx_, aheadState_.data());
		return false;
	}

	void StartAhead(const uint8_t *next, int bytes) {
		// Only out of guest memory, where it's safe to look further than asked.
		const uintptr_t offset = next - Memory::base;
		if (next < Memory::base || offset >= 0x100000000ULL || !Memory::IsValidRange((u32)offset, bytes)) {
			return;
		}
		if (aheadBuffers_[0] == nullptr) {
			aheadBuffers_[0] = new float[4096];
			aheadBuffers_[1] = new float[4096];
			aheadState_.resize(atrac3p_state_size(at3pCtx_));
		}
		aheadPacket_.assign(next, next + bytes);
		aheadIn_ = next;
		aheadBytes_ = bytes;
		aheadWanted_ = true;
		std::lock_guard<std::mutex> lock(aheadMutex);
		aheadBusy_ = true;
		aheadQueue.push_back(this);
		if (!aheadThreadStarted) {
			aheadThreadStarted = true;
			std::thread(AheadThread).detach();
		}
		aheadWake.notify_one();
	}

	float *aheadBuffers_[2]{};
	std::vector<uint8_t> aheadPacket_;
	std::vector<uint8_t> aheadState_;
	const uint8_t *aheadIn_ = nullptr;
	int aheadBytes_ = 0;
	int aheadResult_ = 0;
	int aheadSamples_ = 0;
	bool aheadWanted_ = false;

	ATRAC3PContext *at3pCtx_ = nullptr;
	ATRAC3Context *at3Ctx_ = nullptr;

	int channels_ = 0;
	int blockAlign_ = 0;

	float *buffers_[2]{};

	bool codecOpen_ = false;
	bool codecFailed_ = false;

	PSPAudioType audioType_;
};

static void AheadThread() {
	pthread_setname_np(pthread_self(), "dsp-atrac");
	std::unique_lock<std::mutex> lock(aheadMutex);
	while (true) {
		aheadWake.wait(lock, [] { return !aheadQueue.empty(); });
		Atrac3Audio *decoder = aheadQueue.front();
		aheadQueue.erase(aheadQueue.begin());
		lock.unlock();
		decoder->RunAhead();
		lock.lock();
		decoder->aheadBusy_ = false;
		aheadDone.notify_all();
	}
}

AudioDecoder *CreateAtrac3Audio(int channels, size_t blockAlign, const uint8_t *extraData, size_t extraDataSize) {
	return new Atrac3Audio(PSP_CODEC_AT3, channels, blockAlign, extraData, extraDataSize);
}
AudioDecoder *CreateAtrac3PlusAudio(int channels, size_t blockAlign) {
	return new Atrac3Audio(PSP_CODEC_AT3PLUS, channels, blockAlign, nullptr, 0);
}
