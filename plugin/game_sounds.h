#pragma once
// Reads sounds straight out of the installed game's own data (CASC storage), so the plugin
// never ships Blizzard's audio. Decodes the FLAC files with dr_flac and processes them the way
// the old get-sounds scripts did with ffmpeg.
#include <cstdint>

struct SoundProcess {
	uint32_t outRate;    // output sample rate (the result is always mono 16-bit)
	float    bassDb;     // low-shelf boost in dB, 0 = none
	float    bassHz;     // low-shelf frequency
	float    gainDb;     // gain before the limiter
	float    limit;      // limiter ceiling (linear, 1.0 = full scale)
	float    attackMs;   // limiter look-ahead
	float    releaseMs;  // limiter recovery
	float    maxSeconds; // cut the result to this length, 0 = whole file
};

// Opens the game's data in folder, or next to the running executable when folder is nullptr.
// Returns nullptr on failure and writes a reason into error.
void* OpenGameStorage(const wchar_t* folder, char* error, int errorSize);
void  CloseGameStorage(void* storage);

// Reads one file ("data:data\\global\\sfx\\...flac"), decodes and processes it. Returns a
// malloc'd mono buffer (the caller owns it) and its sample count, or nullptr on failure.
int16_t* LoadGameSound(void* storage, const char* name, const SoundProcess& process, uint32_t* count);
