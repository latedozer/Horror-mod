// See game_sounds.h. The processing follows the ffmpeg chains the old get-sounds scripts used:
//   thunder:  bass=g=4:f=90, volume=12dB, alimiter=limit=0.95:attack=3:release=120, mono 32 kHz
//   ambient:  volume=3dB, alimiter=limit=0.95, mono 22.05 kHz, first 9 seconds
// ffmpeg runs the filters on the file's own format and converts to mono / the new rate last,
// and this does the same.
#include "game_sounds.h"

#include <windows.h>
#include <CascLib.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "third_party/dr_flac.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// RBJ low shelf, as ffmpeg's "bass" filter builds it (width given as Q = 0.5)
void LowShelf(float* samples, uint64_t frames, uint32_t channels, uint32_t rate, double gainDb, double hz) {
	const double A     = std::pow(10.0, gainDb / 40.0);
	const double w0    = 2.0 * kPi * hz / rate;
	const double alpha = std::sin(w0) / (2.0 * 0.5);
	const double c     = std::cos(w0);
	const double s     = 2.0 * std::sqrt(A) * alpha;
	const double a0    = (A + 1) + (A - 1) * c + s;
	const double b0    = A * ((A + 1) - (A - 1) * c + s) / a0;
	const double b1    = 2 * A * ((A - 1) - (A + 1) * c) / a0;
	const double b2    = A * ((A + 1) - (A - 1) * c - s) / a0;
	const double a1    = -2 * ((A - 1) + (A + 1) * c) / a0;
	const double a2    = ((A + 1) + (A - 1) * c - s) / a0;
	for (uint32_t ch = 0; ch < channels; ++ch) {
		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (uint64_t i = 0; i < frames; ++i) {
			float&       sample = samples[i * channels + ch];
			const double x      = sample;
			const double y      = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1; x1 = x; y2 = y1; y1 = y;
			sample = static_cast<float>(y);
		}
	}
}

// Look-ahead peak limiter, channels linked. The needed gain is spread over the look-ahead
// window (a running minimum, then a running average of the same length), which keeps every
// peak at or under the ceiling without delaying the signal; the gain recovers exponentially.
void Limit(float* samples, uint64_t frames, uint32_t channels, uint32_t rate, float limit, float attackMs, float releaseMs) {
	const uint32_t window = attackMs * rate / 1000.0f > 1.0f ? static_cast<uint32_t>(attackMs * rate / 1000.0f) : 1;
	const double   recover = 1.0 - std::exp(-1.0 / (releaseMs / 1000.0 * rate));
	float*         gain    = static_cast<float*>(malloc(frames * sizeof(float)));
	float*         held    = static_cast<float*>(malloc(frames * sizeof(float)));
	uint64_t*      queue   = static_cast<uint64_t*>(malloc(frames * sizeof(uint64_t)));
	if (gain == nullptr || held == nullptr || queue == nullptr) {
		free(gain); free(held); free(queue);
		return;
	}
	for (uint64_t i = 0; i < frames; ++i) {
		float peak = 0.0f;
		for (uint32_t ch = 0; ch < channels; ++ch) {
			peak = std::fmax(peak, std::fabs(samples[i * channels + ch]));
		}
		gain[i] = peak > limit ? limit / peak : 1.0f;
	}
	// held[i] = min(gain[i .. i + window - 1]), with a monotonic queue
	uint64_t head = 0, tail = 0;
	for (uint64_t j = 0; j < frames + window - 1; ++j) {
		if (j < frames) {
			while (tail > head && gain[queue[tail - 1]] >= gain[j]) {
				--tail;
			}
			queue[tail++] = j;
		}
		if (j + 1 >= window) {
			const uint64_t i = j + 1 - window;
			while (queue[head] < i) {
				++head;
			}
			held[i] = gain[queue[head]];
		}
	}
	// recovery: the gain may fall at once but rises only at the release rate
	double level = 1.0;
	for (uint64_t i = 0; i < frames; ++i) {
		level   = held[i] < level ? held[i] : level + (1.0 - level) * recover;
		held[i] = static_cast<float>(level);
	}
	// running average over the look-ahead window: every value averaged in is at most the gain
	// the current sample needs, so the result is too
	double sum = 0.0;
	for (uint64_t i = 0; i < frames; ++i) {
		sum += held[i];
		if (i >= window) {
			sum -= held[i - window];
		}
		const uint64_t n = i + 1 < window ? i + 1 : window;
		const float    g = static_cast<float>(sum / n);
		for (uint32_t ch = 0; ch < channels; ++ch) {
			samples[i * channels + ch] *= g;
		}
	}
	free(gain); free(held); free(queue);
}

double BesselI0(double x) {
	double sum = 1.0, term = 1.0;
	for (int k = 1; k < 30; ++k) {
		term *= (x / (2.0 * k)) * (x / (2.0 * k));
		sum += term;
	}
	return sum;
}

// Mono mixdown and Kaiser-windowed sinc resampling (like ffmpeg's default: cutoff 0.97, beta 9)
float* MonoResample(const float* samples, uint64_t frames, uint32_t channels, uint32_t inRate, uint32_t outRate, uint64_t* outFrames) {
	float* mono = static_cast<float*>(malloc(frames * sizeof(float)));
	if (mono == nullptr) {
		return nullptr;
	}
	for (uint64_t i = 0; i < frames; ++i) {
		float sum = 0.0f;
		for (uint32_t ch = 0; ch < channels; ++ch) {
			sum += samples[i * channels + ch];
		}
		mono[i] = sum / static_cast<float>(channels);
	}
	if (inRate == outRate) {
		*outFrames = frames;
		return mono;
	}
	const double   ratio  = static_cast<double>(inRate) / outRate;
	const double   cutoff = 0.97 * (outRate < inRate ? static_cast<double>(outRate) / inRate : 1.0); // of the input Nyquist
	const int      half   = static_cast<int>(std::ceil(16.0 / cutoff));
	constexpr int  kPhases = 512;
	const int      taps   = 2 * half;
	float*         table  = static_cast<float*>(malloc(static_cast<size_t>(kPhases + 1) * taps * sizeof(float)));
	const uint64_t count  = static_cast<uint64_t>(static_cast<double>(frames) / ratio);
	float*         out    = static_cast<float*>(malloc((count > 0 ? count : 1) * sizeof(float)));
	if (table == nullptr || out == nullptr) {
		free(mono); free(table); free(out);
		return nullptr;
	}
	const double beta = 9.0, norm = BesselI0(beta);
	for (int p = 0; p <= kPhases; ++p) {
		const double frac = static_cast<double>(p) / kPhases;
		for (int k = 0; k < taps; ++k) {
			const double x   = (k - half + 1) - frac; // distance from the output point, in input samples
			const double arg = x * cutoff;
			const double sinc = arg == 0.0 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
			const double r    = x / half;
			const double win  = r * r < 1.0 ? BesselI0(beta * std::sqrt(1.0 - r * r)) / norm : 0.0;
			table[p * taps + k] = static_cast<float>(cutoff * sinc * win);
		}
	}
	for (uint64_t i = 0; i < count; ++i) {
		const double  t    = i * ratio;
		const int64_t base = static_cast<int64_t>(t);
		const int     p    = static_cast<int>((t - base) * kPhases + 0.5);
		const float*  h    = table + p * taps;
		float         acc  = 0.0f;
		for (int k = 0; k < taps; ++k) {
			const int64_t at = base + k - half + 1;
			if (at >= 0 && at < static_cast<int64_t>(frames)) {
				acc += mono[at] * h[k];
			}
		}
		out[i] = acc;
	}
	free(mono); free(table);
	*outFrames = count;
	return out;
}

} // namespace

void* OpenGameStorage(const wchar_t* folder, char* error, int errorSize) {
	wchar_t path[1024] = {};
	if (folder != nullptr) {
		wcsncpy(path, folder, 1023);
	} else {
		const DWORD length = GetModuleFileNameW(nullptr, path, 1000);
		wchar_t*    slash  = length > 0 && length < 1000 ? wcsrchr(path, L'\\') : nullptr;
		if (slash == nullptr) {
			snprintf(error, errorSize, "could not find the game folder");
			return nullptr;
		}
		*slash = L'\0';
	}
	HANDLE storage = nullptr;
	if (!CascOpenStorage(path, 0, &storage)) {
		snprintf(error, errorSize, "could not open the game data in %ls (error %lu)", path, GetCascError());
		return nullptr;
	}
	return storage;
}

void CloseGameStorage(void* storage) {
	if (storage != nullptr) {
		CascCloseStorage(storage);
	}
}

int16_t* LoadGameSound(void* storage, const char* name, const SoundProcess& process, uint32_t* count) {
	*count      = 0;
	HANDLE file = nullptr;
	if (storage == nullptr || !CascOpenFile(storage, name, 0, CASC_OPEN_BY_NAME, &file)) {
		return nullptr;
	}
	ULONGLONG size = 0;
	if (!CascGetFileSize64(file, &size) || size == 0 || size > 64ULL * 1024 * 1024) {
		CascCloseFile(file);
		return nullptr;
	}
	uint8_t* data = static_cast<uint8_t*>(malloc(size));
	DWORD    read = 0;
	const bool ok = data != nullptr && CascReadFile(file, data, static_cast<DWORD>(size), &read) && read == size;
	CascCloseFile(file);
	if (!ok) {
		free(data);
		return nullptr;
	}
	unsigned int channels = 0, rate = 0;
	drflac_uint64 frames  = 0;
	float*        samples = drflac_open_memory_and_read_pcm_frames_f32(data, size, &channels, &rate, &frames, nullptr);
	free(data);
	if (samples == nullptr || frames == 0 || channels == 0 || rate == 0) {
		drflac_free(samples, nullptr);
		return nullptr;
	}

	if (process.bassDb != 0.0f) {
		LowShelf(samples, frames, channels, rate, process.bassDb, process.bassHz);
	}
	const float gain = std::pow(10.0f, process.gainDb / 20.0f);
	for (uint64_t i = 0; i < frames * channels; ++i) {
		samples[i] *= gain;
	}
	Limit(samples, frames, channels, rate, process.limit, process.attackMs, process.releaseMs);

	uint64_t outFrames = 0;
	float*   mono      = MonoResample(samples, frames, channels, rate, process.outRate, &outFrames);
	drflac_free(samples, nullptr);
	if (mono == nullptr) {
		return nullptr;
	}
	if (process.maxSeconds > 0.0f) {
		const uint64_t most = static_cast<uint64_t>(process.maxSeconds * process.outRate);
		outFrames           = outFrames < most ? outFrames : most;
	}
	int16_t* out = static_cast<int16_t*>(malloc((outFrames > 0 ? outFrames : 1) * sizeof(int16_t)));
	if (out != nullptr) {
		for (uint64_t i = 0; i < outFrames; ++i) {
			const float v = std::fmax(-1.0f, std::fmin(1.0f, mono[i])) * 32767.0f;
			out[i]        = static_cast<int16_t>(std::lrint(v));
		}
		*count = static_cast<uint32_t>(outFrames);
	}
	free(mono);
	return out;
}
