// Horrors in the Night - D2RLoader plugin
//   * phantoms: false monsters that cannot be killed, drain life without killing,
//     and vanish after a random time (server side, one game-frame hook)
//   * storm: lightning flash + thunder in rainy areas (client side, overlay + winmm)
//   * sandstorm: the Act 2 desert's storm look fades in and out, with dust and wind
//
// Engine addresses are for the D2R image with PE timestamp 0x6AB3782C only.
// Every address is byte-checked before use; on a mismatch the phantoms switch off.

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/data_tables.h>
#include <D2RLPlugin/lifecycle_events.h>
#include <D2RLPlugin/overlay.h>
#include <D2RLPlugin/threads.h>
#include "game_sounds.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

// ---------------------------------------------------------------- basics

constexpr uint32_t kImageStamp   = 0x6AB3782C;
constexpr int16_t  kPhantomMark  = 7777; // monstats aip8 on every difficulty marks a phantom row
constexpr uint32_t kMonstatsSize = 0x1FC;
constexpr uint32_t kAip8Offset   = 0x90 + 6 * 7;

const D2RL::PluginContext*    g_ctx     = nullptr;
const D2RL::OverlayService*   g_overlay = nullptr;
const D2RL::DataTableService* g_tables  = nullptr;
HMODULE                       g_module  = nullptr;
uintptr_t                     g_base    = 0;

// D2RLoader stores every log line on the disk before its log call returns (on a slow drive that
// took a quarter of a second or more), and much of this plugin runs on the game's own threads.
// So lines go to the loader from a thread of their own: the game's threads only copy the text
// into a queue. Before that thread runs (and after it has stopped) lines are written directly.
constexpr int     kLogQueue = 64;
char              g_logLines[kLogQueue][512];
int               g_logFirst = 0; // under g_logLock
int               g_logCount = 0; // under g_logLock
SRWLOCK           g_logLock  = SRWLOCK_INIT;
HANDLE            g_logEvent  = nullptr;
HANDLE            g_logThread = nullptr;
std::atomic<bool> g_logAsync { false };
std::atomic<bool> g_logStop { false };
std::atomic<int>  g_logDropped { 0 };

void Log(const char* format, ...) {
	if (g_ctx == nullptr) {
		return;
	}
	char    text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (!g_logAsync.load(std::memory_order_acquire)) {
		g_ctx->LogInfo(text);
		return;
	}
	bool queued = false;
	AcquireSRWLockExclusive(&g_logLock);
	if (g_logCount < kLogQueue) {
		memcpy(g_logLines[(g_logFirst + g_logCount) % kLogQueue], text, sizeof(text));
		++g_logCount;
		queued = true;
	}
	ReleaseSRWLockExclusive(&g_logLock);
	if (queued) {
		SetEvent(g_logEvent);
	} else {
		++g_logDropped;
	}
}

// Hands every waiting line to the loader, oldest first. False when there was none.
bool WriteWaitingLines() {
	bool wrote = false;
	for (;;) {
		char text[512];
		bool have = false;
		AcquireSRWLockExclusive(&g_logLock);
		if (g_logCount > 0) {
			memcpy(text, g_logLines[g_logFirst], sizeof(text));
			g_logFirst = (g_logFirst + 1) % kLogQueue;
			--g_logCount;
			have = true;
		}
		ReleaseSRWLockExclusive(&g_logLock);
		if (!have) {
			return wrote;
		}
		g_ctx->LogInfo(text);
		wrote = true;
	}
}

DWORD WINAPI LogThread(void*) {
	for (;;) {
		WaitForSingleObject(g_logEvent, INFINITE);
		WriteWaitingLines();
		if (g_logStop.load()) {
			WriteWaitingLines();
			return 0;
		}
	}
}

void StartLogThread() {
	g_logStop   = false;
	g_logEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_logThread = g_logEvent != nullptr ? CreateThread(nullptr, 0, LogThread, nullptr, 0, nullptr) : nullptr;
	g_logAsync.store(g_logThread != nullptr, std::memory_order_release);
}

// The queue is emptied before the thread ends; lines logged from then on are written directly.
void StopLogThread() {
	if (g_logThread == nullptr) {
		return;
	}
	g_logStop = true;
	SetEvent(g_logEvent);
	if (WaitForSingleObject(g_logThread, 3000) != WAIT_OBJECT_0) {
		return; // still busy with the disk: it keeps the queue, nothing is freed under it
	}
	g_logAsync.store(false, std::memory_order_release);
	WriteWaitingLines(); // anything queued after its last look
	CloseHandle(g_logThread);
	CloseHandle(g_logEvent);
	g_logThread = nullptr;
	g_logEvent  = nullptr;
	if (g_logDropped.load() > 0) {
		Log("log: %d lines were left out (more than %d were waiting at once)", g_logDropped.load(), kLogQueue);
	}
}

// The server tick, the client (overlay) callback and the sound mixer run on different threads,
// so each has its own generator.
struct Rng {
	uint64_t state;

	uint32_t Next() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return static_cast<uint32_t>(state >> 16);
	}

	int Range(int low, int high) {
		if (high <= low) {
			return low;
		}
		return low + static_cast<int>(Next() % static_cast<uint32_t>(high - low + 1));
	}
};

Rng g_rng { 0x9E3779B97F4A7C15ULL };       // server tick
Rng g_clientRng { 0xD1B54A32D192ED03ULL }; // overlay callback
Rng g_mixRng { 0x8CB92BA72F3D8DD7ULL };    // mixer thread

void SeedRandom(uint64_t seed) {
	for (Rng* rng : { &g_rng, &g_clientRng, &g_mixRng }) {
		rng->state ^= seed * 0x2545F4914F6CDD1DULL;
		if (rng->state == 0) {
			rng->state = 1;
		}
		seed += 0x9E3779B97F4A7C15ULL;
	}
}

uint32_t Rand() { return g_rng.Next(); }
int      RandRange(int low, int high) { return g_rng.Range(low, high); }
uint32_t ClientRand() { return g_clientRng.Next(); }
int      ClientRange(int low, int high) { return g_clientRng.Range(low, high); }

// ---------------------------------------------------------------- config

// The most effects a storm's blowing field can be made of. The ash storm has a setting for each;
// the sandstorm has three.
constexpr int kMaxLayers = 6;

// One kind of storm the plugin makes itself (see "storms" below): its settings.
struct StormConfig {
	int enabled;
	int calmMin;      // seconds between storms...
	int calmMax;
	int lengthMin;    // ...and how long one blows
	int lengthMax;
	int fade;         // seconds a storm takes to roll in or die down
	int strength;     // how much of the mod's storm look a storm reaches, percent
	int always;       // the storm never stops (0: storms come and go on the clock above)
	int dust;         // the blowing field on (0 = off)
	int layer[kMaxLayers]; // the effects that make up the field: numbers into the kind's effect
	                  // list, 0 = none. They come in one after another as a storm builds.
	int fieldRadius;  // patches of each effect out from the one you stand in (5 = 11 x 11)
	int fieldSpacing; // sub-tiles between patches
	int fieldRate;    // patches created per second while the field is being laid out
	int fieldLead;    // how far the field leans ahead of a moving player, percent of its reach
	int windVolume;
	int indoorWind;   // muffled wind in its town and underground while a storm blows
	int windLowPass;  // Hz: only the wind's tones below this are heard, 0 = all of it
};

struct Config {
	int phantomsEnabled = 0;
	int minInterval     = 40;
	int maxInterval     = 140;
	int minLife         = 4;
	int maxLife         = 10;
	int maxAlive        = 3;
	int maxGroup        = 2;
	int drainPercent    = 10;
	int minDistance     = 10;
	int maxDistance     = 18;

	int hauntChance      = 5;  // percent of appearances that are a townsfolk haunting
	int riseMax          = 4;  // corpses that get back up in a haunting
	int falseHealth      = 1;  // the life orb reads 1 during a haunting

	int weatherEnabled   = 1;  // drive the game's own rain and snow
	int weatherIntensity = 100;

	// the Act 2 sandstorm: on, calm 60-240 s, storms of 45-120 s, 8 s to roll in, full strength,
	// never stopping, blowing sand on; thin sand, then huge clouds now and then, then steady haze
	// (numbers into kSandEffects); an 11 x 11 field, 17 sub-tiles apart, 30 patches a second,
	// leaning half its reach ahead; wind at 70%, muffled in town and underground
	StormConfig sand = { 1, 60, 240, 45, 120, 8, 100, 1, 100, { 6, 5, 3 }, 5, 17, 30, 50, 70, 1, 0 };
	// the Act 4 ash storm: the same clock; wide ash and embers, fog, wide haze, lava glow and
	// smoke columns (numbers into kAshEffects), one layer left empty; an 11 x 11 field, 20
	// sub-tiles apart
	StormConfig ash = { 1, 60, 240, 45, 120, 8, 100, 1, 100, { 6, 9, 11, 17, 14, 0 }, 5, 20, 30, 50, 190, 1, 100 };
	// the ash storm's meteors: burning rocks from the sky while a storm shows
	int meteors           = 1;
	int fireVolume        = 70; // the fire under the storm: a low roar and crackling embers
	int burstVolume       = 70; // now and then: steam, falling rock
	int burstMinInterval  = 10; // seconds between them
	int burstMaxInterval  = 40;
	int meteorMinInterval = 40; // seconds between volleys...
	int meteorMaxInterval = 80;
	int meteorsMin        = 50; // ...of this many meteors
	int meteorsMax        = 50;
	int meteorGapMin      = 40; // milliseconds from one meteor of a volley to the next (0: together)
	int meteorGapMax      = 240;
	int meteorMinDistance = 3;  // how near and how far from you they land, in tiles
	int meteorMaxDistance = 22;
	int meteorHits        = 1;  // one of a volley can be aimed at you: its marker shows where
	int meteorHitChance   = 6;  // percent of volleys with an aimed one
	int meteorHitDamage   = 5;  // percent of MAXIMUM life it burns if you are still there (never kills): lowest...
	int meteorHitDamageMax = 10; // ...and highest

	int ambientEnabled   = 1;
	int ambientMin       = 70;
	int ambientMax       = 200;
	int ambientVolume    = 60;

	int stormEnabled     = 1;
	int stormMinInterval = 15;
	int stormMaxInterval = 70;
	int flashStrength    = 1;  // 1 = drop a lightning bolt near you on outdoor strikes
	int screenFlash      = 0;
	int boltRow          = -1; // -1 = the mod's own bolt row; otherwise this missiles row
	int hitsMin          = 3;  // a strike flickers this many times...
	int hitsMax          = 3;
	int boltsPerHit      = 1;  // ...with this many bolts each time
	int boltSpread       = 16; // farthest a bolt lands from you, in tiles
	int boltMinDistance  = 7;  // nearest a normal bolt lands
	int directHits       = 1;  // strikes can land on the player at all
	int directHitChance  = 6;  // percent of strikes that land on the player
	int directHitDamage  = 5;  // percent of MAXIMUM life a direct hit removes (never kills): lowest...
	int directHitDamageMax = 10; // ...and highest
	int skyFlash         = 1;  // light the whole scene through the renderer's time-of-day blend
	int skyStrength      = 100;
	int volume           = 100;
	int snowAreas        = 1;
	int indoorThunder    = 1;
} g_config;

int Clamp(int value, int low, int high) {
	return value < low ? low : (value > high ? high : value);
}

// What a sandstorm can blow past you: the game's own particle effects, by number ("horror fx").
// 0 means none. They are effects the game places around Lut Gholein. Each is a patch some
// tens of sub-tiles across; a storm lays them out on a grid around you.
struct StormEffect {
	const char* name;
	const char* path;
};
const StormEffect kSandEffects[] = {
	{ "none", nullptr },
	{ "haze, gusting", "data/hd/vfx/particles/env/Act2/town/FX_sandHaze_low_med_gusts.particles" },
	{ "haze, scattered gusts", "data/hd/vfx/particles/env/Act2/town/FX_sandHaze_low_med_scatteredgusts.particles" },
	{ "haze, steady", "data/hd/vfx/particles/env/Act2/town/FX_sandHaze_low_med.particles" },
	{ "huge clouds, steady", "data/hd/vfx/particles/env/Act2/town/fx_sandhuge_permenant.particles" },
	{ "huge clouds, now and then", "data/hd/vfx/particles/env/Act2/town/FX_SandHuge_Intermittent.particles" },
	{ "thin sand, steady", "data/hd/vfx/particles/env/Act2/town/FX_SandThin_Permenant.particles" },
	{ "sand wall, outer", "data/hd/vfx/particles/env/Act2/town/FX_Sand_Wall_outer.particles" },
	{ "sand wall", "data/hd/vfx/particles/env/Act2/town/FX_Sand_Wall_20x40.particles" },
	{ "ribbons, tall", "data/hd/vfx/particles/env/Act2/town/FX_SandRibbons_MoreHeight_Variance.particles" },
};
constexpr int kSandEffectCount = static_cast<int>(sizeof(kSandEffects) / sizeof(kSandEffects[0]));
bool          g_sandFailed[kSandEffectCount]; // the game would not create it: not tried again

// The same for the Act 4 ash storm ("horror ashfx"): effects the game places in Act 4's own maps,
// and its ash and embers, which no map places.
const StormEffect kAshEffects[] = {
	{ "none", nullptr },
	{ "wind, small", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_Wind_Small_10x10.particles" },
	{ "wind, huge", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_Wind_Huge_40x40.particles" },
	{ "wind ribbons", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_WindRibbons_20x40.particles" },
	{ "ash and embers, small", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_10x10.particles" },
	{ "ash and embers", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_20x20.particles" },
	{ "ash and embers, wide", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_30x30.particles" },
	{ "ash and embers, high", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_high_30x30.particles" },
	{ "ash and embers, long", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_10x30.particles" },
	{ "fog", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_fog_30x30.particles" },
	{ "haze", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_HoleHaze_20x20.particles" },
	{ "haze, wide", "data/hd/vfx/particles/env/Act4/Mesa/fx_act4mesa_holehaze_40x40.particles" },
	{ "steam", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_HoleSteam_20x20.particles" },
	{ "steam, wide", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_HoleSteam_40x40.particles" },
	{ "smoke column", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_ChimneyPit_20x20.particles" },
	{ "smoke column, wide", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_ChimneyPit_40x40.particles" },
	{ "falling rocks", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_FallingRocks.particles" },
	{ "lava glow", "data/hd/vfx/particles/env/Act4/Expansion/FX_Act4Expansion_lavaglow_30x30.particles" },
	{ "embers (Travincal's)", "data/hd/vfx/particles/env/Act3/Travincal/FX_Act3Travincal_embers_20x20.particles" },
};
constexpr int kAshEffectCount = static_cast<int>(sizeof(kAshEffects) / sizeof(kAshEffects[0]));
bool          g_ashFailed[kAshEffectCount];
constexpr int kMaxFieldRadius  = 5;

// Every setting, once: where it lives in horror-night.toml, its limits, and (when it has a
// label) its line in the in-game settings menu. A row without a section is a menu header.
struct Setting {
	const char* section;
	const char* key;
	int*        target;
	bool        isBool;
	int         min;
	int         max;
	int         step;
	const char* label; // nullptr = config file only
	const char* note;  // no double quotes: the text goes into a quoted TOML string
};

const Setting kSettings[] = {
	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Phantoms", nullptr },
	{ "phantoms", "enabled", &g_config.phantomsEnabled, true, 0, 1, 0, "Phantoms appear", "False monsters: they cannot be killed, their hits drain life but never kill, and they vanish." },
	{ "phantoms", "min_interval", &g_config.minInterval, false, 5, 3600, 5, "Shortest wait (seconds)", "Time between appearances is random between the two." },
	{ "phantoms", "max_interval", &g_config.maxInterval, false, 5, 7200, 5, "Longest wait (seconds)", nullptr },
	{ "phantoms", "min_life", &g_config.minLife, false, 2, 600, 1, "Shortest stay (seconds)", "How long a phantom stays before it vanishes." },
	{ "phantoms", "max_life", &g_config.maxLife, false, 2, 900, 1, "Longest stay (seconds)", nullptr },
	{ "phantoms", "max_alive", &g_config.maxAlive, false, 1, 12, 1, "Most at once", nullptr },
	{ "phantoms", "max_group", &g_config.maxGroup, false, 1, 12, 1, "Most appearing together", nullptr },
	{ "phantoms", "drain_percent", &g_config.drainPercent, false, 1, 50, 1, "Life drained per hit (%)", "Percent of your current life. It can never kill." },
	{ "phantoms", "min_distance", &g_config.minDistance, false, 4, 30, 1, "Nearest they appear (tiles)", nullptr },
	{ "phantoms", "max_distance", &g_config.maxDistance, false, 4, 34, 1, "Farthest they appear (tiles)", nullptr },

	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Hauntings", nullptr },
	{ "haunting", "chance", &g_config.hauntChance, false, 0, 100, 1, "Haunting chance (%)", "Share of phantom appearances where one of the dead wears a townsperson's name." },
	{ "haunting", "corpses", &g_config.riseMax, false, 0, 8, 1, "Corpses that get back up", nullptr },
	{ "haunting", "false_health", &g_config.falseHealth, true, 0, 1, 0, "Life orb reads 1", "Only what the orb shows; your real life is untouched." },

	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Weather", nullptr },
	{ "weather", "enabled", &g_config.weatherEnabled, true, 0, 1, 0, "Always rain or snow outdoors", "Rain in Acts 1 and 3, snow in Act 5." },
	{ "weather", "intensity", &g_config.weatherIntensity, false, 5, 100, 5, "How heavy (%)", nullptr },

	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Sandstorms", nullptr },
	{ "sandstorm", "enabled", &g_config.sand.enabled, true, 0, 1, 0, "Sandstorms in the Act 2 desert", "Needs the mod's Sandstorms in Act 2 option, with Always night on." },
	{ "sandstorm", "min_calm", &g_config.sand.calmMin, false, 5, 3600, 5, "Shortest calm (seconds)", "Time between storms is random between the two." },
	{ "sandstorm", "max_calm", &g_config.sand.calmMax, false, 5, 7200, 5, "Longest calm (seconds)", nullptr },
	{ "sandstorm", "min_length", &g_config.sand.lengthMin, false, 5, 3600, 5, "Shortest storm (seconds)", nullptr },
	{ "sandstorm", "max_length", &g_config.sand.lengthMax, false, 5, 7200, 5, "Longest storm (seconds)", nullptr },
	{ "sandstorm", "fade", &g_config.sand.fade, false, 1, 60, 1, "Roll-in time (seconds)", "How long a storm takes to build up, and to die down." },
	{ "sandstorm", "strength", &g_config.sand.strength, false, 10, 100, 5, "Storm strength (%)", "How far the storm look goes at its height." },
	{ "sandstorm", "always", &g_config.sand.always, true, 0, 1, 0, "The storm never stops", nullptr },
	{ "sandstorm", "dust", &g_config.sand.dust, false, 0, 100, 100, "Blowing sand", "0 turns the blowing sand off, 100 on." },
	{ "sandstorm", "layer1", &g_config.sand.layer[0], false, 0, kSandEffectCount - 1, 1, "Blowing sand: first effect", "Effect numbers: type horror fx in the console for the list. 0 is none." },
	{ "sandstorm", "layer2", &g_config.sand.layer[1], false, 0, kSandEffectCount - 1, 1, "Blowing sand: second effect", "Comes in as the storm builds." },
	{ "sandstorm", "layer3", &g_config.sand.layer[2], false, 0, kSandEffectCount - 1, 1, "Blowing sand: third effect", "Comes in last, at the height of the storm." },
	{ "sandstorm", "field_radius", &g_config.sand.fieldRadius, false, 0, kMaxFieldRadius, 1, "Blowing sand: how far out (patches)", "Patches of each effect in every direction from where you stand. 1 is a 3 by 3 field, 2 is 5 by 5, up to 5 (11 by 11). More costs frame rate." },
	{ "sandstorm", "field_spacing", &g_config.sand.fieldSpacing, false, 5, 60, 1, "Blowing sand: patch spacing (sub-tiles)", "Distance between patches. Smaller is denser, larger reaches further." },
	{ "sandstorm", "field_rate", &g_config.sand.fieldRate, false, 1, 100, 1, "Blowing sand: patches created per second", "How fast the field fills in as a storm builds and as you walk. Low lets it creep in, high is all at once. A dense field needs a high rate to keep up with you running." },
	{ "sandstorm", "field_lead", &g_config.sand.fieldLead, false, 0, 100, 5, "Blowing sand: lean ahead when moving (%)", "While you move the field shifts ahead of you, so sand has time to build up before you reach it. 0 keeps it centred on you, 100 puts you at its back edge." },
	{ "sandstorm", "wind_volume", &g_config.sand.windVolume, false, 0, 300, 5, "Wind volume (%)", nullptr },
	{ "sandstorm", "wind_low_pass", &g_config.sand.windLowPass, false, 0, 2000, 10, "Wind: only tones below (Hz)", "0 is the whole wind. A low number leaves only its bass: a deep roar without the whistling." },
	{ "sandstorm", "indoor_wind", &g_config.sand.indoorWind, true, 0, 1, 0, "Muffled wind in town and underground", nullptr },

	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Ash storms", nullptr },
	{ "ashstorm", "enabled", &g_config.ash.enabled, true, 0, 1, 0, "Ash storms in Act 4", "Needs the mod's Ash storms in Act 4 option, with Always night on." },
	{ "ashstorm", "min_calm", &g_config.ash.calmMin, false, 5, 3600, 5, "Shortest calm (seconds)", "Time between storms is random between the two." },
	{ "ashstorm", "max_calm", &g_config.ash.calmMax, false, 5, 7200, 5, "Longest calm (seconds)", nullptr },
	{ "ashstorm", "min_length", &g_config.ash.lengthMin, false, 5, 3600, 5, "Shortest storm (seconds)", nullptr },
	{ "ashstorm", "max_length", &g_config.ash.lengthMax, false, 5, 7200, 5, "Longest storm (seconds)", nullptr },
	{ "ashstorm", "fade", &g_config.ash.fade, false, 1, 60, 1, "Roll-in time (seconds)", "How long a storm takes to build up, and to die down." },
	{ "ashstorm", "strength", &g_config.ash.strength, false, 10, 100, 5, "Storm strength (%)", "How far the storm look goes at its height." },
	{ "ashstorm", "always", &g_config.ash.always, true, 0, 1, 0, "The storm never stops", nullptr },
	{ "ashstorm", "dust", &g_config.ash.dust, false, 0, 100, 100, "Blowing ash", "0 turns the blowing ash off, 100 on." },
	{ "ashstorm", "layer1", &g_config.ash.layer[0], false, 0, kAshEffectCount - 1, 1, "Blowing ash: first effect", "Effect numbers: type horror ashfx in the console for the list. 0 is none." },
	{ "ashstorm", "layer2", &g_config.ash.layer[1], false, 0, kAshEffectCount - 1, 1, "Blowing ash: second effect", "Comes in as the storm builds." },
	{ "ashstorm", "layer3", &g_config.ash.layer[2], false, 0, kAshEffectCount - 1, 1, "Blowing ash: third effect", "The effects come in one after another, the last at the height of the storm." },
	{ "ashstorm", "layer4", &g_config.ash.layer[3], false, 0, kAshEffectCount - 1, 1, "Blowing ash: fourth effect", "0 is none. Every effect is another field of patches: more costs frame rate." },
	{ "ashstorm", "layer5", &g_config.ash.layer[4], false, 0, kAshEffectCount - 1, 1, "Blowing ash: fifth effect", nullptr },
	{ "ashstorm", "layer6", &g_config.ash.layer[5], false, 0, kAshEffectCount - 1, 1, "Blowing ash: sixth effect", nullptr },
	{ "ashstorm", "field_radius", &g_config.ash.fieldRadius, false, 0, kMaxFieldRadius, 1, "Blowing ash: how far out (patches)", "Patches of each effect in every direction from where you stand. 1 is a 3 by 3 field, up to 5 (11 by 11). More costs frame rate." },
	{ "ashstorm", "field_spacing", &g_config.ash.fieldSpacing, false, 5, 60, 1, "Blowing ash: patch spacing (sub-tiles)", "Distance between patches. Smaller is denser, larger reaches further." },
	{ "ashstorm", "field_rate", &g_config.ash.fieldRate, false, 1, 100, 1, "Blowing ash: patches created per second", "How fast the field fills in as a storm builds and as you walk." },
	{ "ashstorm", "field_lead", &g_config.ash.fieldLead, false, 0, 100, 5, "Blowing ash: lean ahead when moving (%)", "While you move the field shifts ahead of you. 0 keeps it centred on you, 100 puts you at its back edge." },
	{ "ashstorm", "wind_volume", &g_config.ash.windVolume, false, 0, 300, 5, "Wind volume (%)", nullptr },
	{ "ashstorm", "wind_low_pass", &g_config.ash.windLowPass, false, 0, 2000, 10, "Wind: only tones below (Hz)", "0 is the whole wind. A low number leaves only its bass: a deep roar without the whistling. 100 as shipped." },
	{ "ashstorm", "indoor_wind", &g_config.ash.indoorWind, true, 0, 1, 0, "Muffled wind in the Pandemonium Fortress", "The fire is muffled with it." },
	{ "ashstorm", "fire_volume", &g_config.fireVolume, false, 0, 300, 5, "Fire volume (%)", "The roar and crackle of fire under the storm. 0 is none." },
	{ "ashstorm", "burst_volume", &g_config.burstVolume, false, 0, 300, 5, "Steam and rockfall volume (%)", "Now and then, out in the storm: a vent of steam, rock coming down. 0 is none." },
	{ "ashstorm", "burst_min_interval", &g_config.burstMinInterval, false, 2, 3600, 1, "Steam and rockfall: shortest wait (seconds)", "Time between them is random between the two." },
	{ "ashstorm", "burst_max_interval", &g_config.burstMaxInterval, false, 2, 7200, 1, "Steam and rockfall: longest wait (seconds)", nullptr },
	{ "ashstorm", "meteors", &g_config.meteors, true, 0, 1, 0, "Meteors fall during a storm", "Burning rocks from the sky. They are for show: only one aimed at you can hurt (below)." },
	{ "ashstorm", "meteor_min_interval", &g_config.meteorMinInterval, false, 2, 3600, 1, "Meteors: shortest wait (seconds)", "Time between volleys is random between the two." },
	{ "ashstorm", "meteor_max_interval", &g_config.meteorMaxInterval, false, 2, 7200, 1, "Meteors: longest wait (seconds)", nullptr },
	{ "ashstorm", "meteors_min", &g_config.meteorsMin, false, 1, 60, 1, "Meteors: fewest in a volley", nullptr },
	{ "ashstorm", "meteors_max", &g_config.meteorsMax, false, 1, 60, 1, "Meteors: most in a volley", nullptr },
	{ "ashstorm", "meteor_gap_min", &g_config.meteorGapMin, false, 0, 2000, 40, "Meteors: shortest gap in a volley (ms)", "Time from one meteor of a volley to the next, random between the two. The game counts in steps of 40 ms; 0 lets them fall together." },
	{ "ashstorm", "meteor_gap_max", &g_config.meteorGapMax, false, 0, 2000, 40, "Meteors: longest gap in a volley (ms)", nullptr },
	{ "ashstorm", "meteor_min_distance", &g_config.meteorMinDistance, false, 2, 30, 1, "Meteors: nearest one lands (tiles)", nullptr },
	{ "ashstorm", "meteor_max_distance", &g_config.meteorMaxDistance, false, 4, 34, 1, "Meteors: farthest one lands (tiles)", nullptr },
	{ "ashstorm", "meteor_hits", &g_config.meteorHits, true, 0, 1, 0, "A meteor can be aimed at you", "Its red marker shows where it will land: step out before it does. Off: meteors never hurt." },
	{ "ashstorm", "meteor_hit_chance", &g_config.meteorHitChance, false, 0, 100, 1, "Volleys with an aimed meteor (%)", nullptr },
	{ "ashstorm", "meteor_hit_damage_min", &g_config.meteorHitDamage, false, 0, 100, 1, "Aimed meteor: least damage (% of max life)", "A hit always leaves at least 1 life." },
	{ "ashstorm", "meteor_hit_damage_max", &g_config.meteorHitDamageMax, false, 0, 100, 1, "Aimed meteor: most damage (% of max life)", nullptr },

	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Whispers", nullptr },
	{ "sounds", "enabled", &g_config.ambientEnabled, true, 0, 1, 0, "Whispers and distant sounds", "Breathing, moans, screams and ravens outside town." },
	{ "sounds", "min_interval", &g_config.ambientMin, false, 3, 3600, 5, "Shortest wait (seconds)", nullptr },
	{ "sounds", "max_interval", &g_config.ambientMax, false, 3, 7200, 5, "Longest wait (seconds)", nullptr },
	{ "sounds", "volume", &g_config.ambientVolume, false, 0, 300, 5, "Volume (%)", "100 is as recorded." },

	{ nullptr, nullptr, nullptr, false, 0, 0, 0, "Storm", nullptr },
	{ "storm", "enabled", &g_config.stormEnabled, true, 0, 1, 0, "Lightning and thunder", "In the rainy outdoor areas of Acts 1 and 3. Needs the weather on." },
	{ "storm", "min_interval", &g_config.stormMinInterval, false, 3, 3600, 5, "Shortest wait (seconds)", "Time between strikes is random between the two." },
	{ "storm", "max_interval", &g_config.stormMaxInterval, false, 3, 7200, 5, "Longest wait (seconds)", nullptr },
	{ "storm", "volume", &g_config.volume, false, 0, 300, 5, "Thunder volume (%)", nullptr },
	{ "storm", "sky_flash", &g_config.skyFlash, true, 0, 1, 0, "Lightning lights the whole scene", "Needs the mod's Lightning lights the whole sky option." },
	{ "storm", "sky_flash_strength", &g_config.skyStrength, false, 0, 100, 5, "Flash brightness (%)", nullptr },
	{ "storm", "lightning_bolts", &g_config.flashStrength, true, 0, 1, 0, "Lightning bolts", "A harmless bolt lands near you on each strike." },
	{ "storm", "hits_min", &g_config.hitsMin, false, 1, 12, 1, "Fewest flickers per strike", nullptr },
	{ "storm", "hits_max", &g_config.hitsMax, false, 1, 12, 1, "Most flickers per strike", nullptr },
	{ "storm", "bolts_per_hit", &g_config.boltsPerHit, false, 1, 8, 1, "Bolts per flicker", nullptr },
	{ "storm", "bolt_min_distance", &g_config.boltMinDistance, false, 2, 30, 1, "Nearest a bolt lands (tiles)", nullptr },
	{ "storm", "bolt_spread", &g_config.boltSpread, false, 4, 30, 1, "Farthest a bolt lands (tiles)", nullptr },
	{ "storm", "direct_hits", &g_config.directHits, true, 0, 1, 0, "Lightning can strike you", "Off: bolts only land around you, never on you." },
	{ "storm", "direct_hit_chance", &g_config.directHitChance, false, 0, 100, 1, "Strikes that hit you (%)", nullptr },
	{ "storm", "direct_hit_damage_min", &g_config.directHitDamage, false, 0, 100, 1, "Direct hit: least damage (% of max life)", "A direct hit always leaves at least 1 life." },
	{ "storm", "direct_hit_damage_max", &g_config.directHitDamageMax, false, 0, 100, 1, "Direct hit: most damage (% of max life)", nullptr },
	{ "storm", "snow_areas", &g_config.snowAreas, true, 0, 1, 0, "Storms in the snow of Act 5 too", nullptr },
	{ "storm", "indoor_thunder", &g_config.indoorThunder, true, 0, 1, 0, "Muffled thunder underground", nullptr },
	{ "storm", "screen_flash", &g_config.screenFlash, true, 0, 1, 0, "Also flash the whole screen white", "Needs a working D2RLoader overlay. It covers the interface too." },
	// a testing knob, kept out of the menu
	{ "storm", "bolt_row", &g_config.boltRow, false, -1, 65535, 1, nullptr, nullptr },
};

const Setting* FindSetting(const char* section, size_t sectionLength, const char* key) {
	for (const Setting& setting : kSettings) {
		if (setting.section != nullptr && strlen(setting.section) == sectionLength && strncmp(setting.section, section, sectionLength) == 0
			&& strcmp(setting.key, key) == 0) {
			return &setting;
		}
	}
	return nullptr;
}

void SetSetting(const Setting& setting, int number) {
	*setting.target = setting.isBool ? (number != 0 ? 1 : 0) : Clamp(number, setting.min, setting.max);
}

void ApplyConfigValue(const char* section, const char* key, const char* value) {
	int number = 0;
	if (strncmp(value, "true", 4) == 0) {
		number = 1;
	} else if (strncmp(value, "false", 5) == 0) {
		number = 0;
	} else {
		number = atoi(value);
	}
	// keys this version does not know (from an older or newer file) are skipped
	if (const Setting* setting = FindSetting(section, strlen(section), key)) {
		SetSetting(*setting, number);
	}
}

// Every value inside its limits, and the second of each pair never below the first.
void NormalizeConfig() {
	for (const Setting& setting : kSettings) {
		if (setting.target != nullptr) {
			SetSetting(setting, *setting.target);
		}
	}
	const auto atLeast = [](int& value, int floor) {
		if (value < floor) {
			value = floor;
		}
	};
	const auto atMost = [](int& value, int ceiling) {
		if (value > ceiling) {
			value = ceiling;
		}
	};
	atLeast(g_config.maxInterval, g_config.minInterval);
	atLeast(g_config.maxLife, g_config.minLife);
	atLeast(g_config.maxDistance, g_config.minDistance);
	for (StormConfig* storm : { &g_config.sand, &g_config.ash }) {
		atLeast(storm->calmMax, storm->calmMin);
		atLeast(storm->lengthMax, storm->lengthMin);
	}
	atLeast(g_config.ambientMax, g_config.ambientMin);
	atLeast(g_config.stormMaxInterval, g_config.stormMinInterval);
	atLeast(g_config.hitsMax, g_config.hitsMin);
	atLeast(g_config.directHitDamageMax, g_config.directHitDamage);
	atMost(g_config.maxGroup, g_config.maxAlive);
	atMost(g_config.boltMinDistance, g_config.boltSpread);
	atLeast(g_config.meteorMaxInterval, g_config.meteorMinInterval);
	atLeast(g_config.burstMaxInterval, g_config.burstMinInterval);
	atLeast(g_config.meteorsMax, g_config.meteorsMin);
	atLeast(g_config.meteorGapMax, g_config.meteorGapMin);
	atLeast(g_config.meteorMaxDistance, g_config.meteorMinDistance);
	atLeast(g_config.meteorHitDamageMax, g_config.meteorHitDamage);
}

// Reads the text of horror-night.toml into g_config. The text is cut up while it is read.
void ParseConfig(char* text) {
	char  section[64] = "";
	char* line        = text;
	while (*line != '\0') {
		char* end = line;
		while (*end != '\0' && *end != '\n') {
			++end;
		}
		const char saved = *end;
		*end             = '\0';

		while (*line == ' ' || *line == '\t' || static_cast<unsigned char>(*line) >= 0x80) {
			++line; // also skips a UTF-8 byte-order mark
		}
		if (*line == '[') {
			const char* close = strchr(line, ']');
			if (close != nullptr && static_cast<size_t>(close - line - 1) < sizeof(section)) {
				memcpy(section, line + 1, close - line - 1);
				section[close - line - 1] = '\0';
			}
		} else if (*line != '#' && *line != '\0') {
			char* equals = strchr(line, '=');
			if (equals != nullptr) {
				char key[64] = "";
				size_t length = 0;
				for (const char* c = line; c < equals && length + 1 < sizeof(key); ++c) {
					if (*c != ' ' && *c != '\t') {
						key[length++] = *c;
					}
				}
				const char* value = equals + 1;
				while (*value == ' ' || *value == '\t') {
					++value;
				}
				ApplyConfigValue(section, key, value);
			}
		}

		if (saved == '\0') {
			break;
		}
		line = end + 1;
	}
	NormalizeConfig();
}

void LoadConfig() {
	static char text[32768];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		Log("config: could not read horror-night.toml (missing, or larger than %u bytes) - using the defaults", static_cast<unsigned>(sizeof(text) - 1));
		return;
	}
	ParseConfig(text);
}

// ---------------------------------------------------------------- I Hate TOMLs
// The "I Hate TOMLs" plugin offers a "settings-menu" service: a plugin hands it a small
// TOML description of its settings and two callbacks (read a value as text, write a value),
// and gets a page in the in-game settings window. Layout read from plugins that use it:
//   table:        { u32 size; u32 version; handle (*add)(context, const Registration*); ... }
//   Registration: { u32 size = 0x28; u32 flags; const char* description; get; set; reserved }
//   get(key, out, outSize) -> found      set(key, value, error, errorSize) -> accepted
// Every change is applied at once and written back to horror-night.toml.

struct MenuRegistration {
	uint32_t    structSize;
	uint32_t    flags;
	const char* description;
	bool (*get)(const char* key, char* out, uint32_t outSize);
	bool (*set)(const char* key, const char* value, char* error, uint32_t errorSize);
	void* reserved;
};

char             g_menuText[24576];
MenuRegistration g_menuRegistration {};
bool             g_menuJoined = false;

// Menu keys are "<section>_<key>".
const Setting* MenuFind(const char* key) {
	if (key == nullptr) {
		return nullptr;
	}
	for (const Setting& setting : kSettings) {
		if (setting.section == nullptr || setting.label == nullptr) {
			continue;
		}
		const size_t sectionLength = strlen(setting.section);
		if (strncmp(key, setting.section, sectionLength) == 0 && key[sectionLength] == '_' && strcmp(key + sectionLength + 1, setting.key) == 0) {
			return &setting;
		}
	}
	return nullptr;
}

void SettingText(const Setting& setting, char* out, size_t outSize) {
	if (setting.isBool) {
		snprintf(out, outSize, "%s", *setting.target != 0 ? "true" : "false");
	} else {
		snprintf(out, outSize, "%d", *setting.target);
	}
}

bool MenuGet(const char* key, char* out, uint32_t outSize) {
	const Setting* setting = MenuFind(key);
	if (setting == nullptr || out == nullptr || outSize == 0) {
		return false;
	}
	SettingText(*setting, out, outSize);
	return true;
}

// Writes into result the config text with one "key = value" line of [section] replaced; the
// line, or the whole section, is added when an older file does not have it. Returns the length.
size_t RewriteConfig(const char* text, const char* section, const char* key, const char* value, char* result, size_t resultSize) {
	const size_t keyLength     = strlen(key);
	const size_t sectionLength = strlen(section);
	size_t       used          = 0;
	bool         inSection     = false;
	bool         done          = false;
	bool         seen          = false;
	const char*  line          = text;
	const auto   append        = [&](const char* data, size_t length) {
		if (used + length < resultSize - 1) {
			memcpy(result + used, data, length);
			used += length;
		}
	};
	// a new line always starts on a line of its own, also after a last line with no line end
	const auto appendLine = [&](const char* data, size_t length) {
		if (used > 0 && result[used - 1] != '\n') {
			append("\r\n", 2);
		}
		append(data, length);
	};
	while (*line != '\0') {
		const char*  end    = strchr(line, '\n');
		const size_t length = end != nullptr ? static_cast<size_t>(end - line) + 1 : strlen(line);
		const char*  c      = line;
		while (*c == ' ' || *c == '\t') {
			++c;
		}
		if (*c == '[') {
			if (inSection && !done) {
				char      added[160];
				const int n = snprintf(added, sizeof(added), "%s = %s\r\n", key, value);
				appendLine(added, static_cast<size_t>(n));
				done = true;
			}
			inSection = strncmp(c + 1, section, sectionLength) == 0 && c[1 + sectionLength] == ']';
			seen      = seen || inSection;
		} else if (inSection && !done && strncmp(c, key, keyLength) == 0) {
			const char* after = c + keyLength;
			while (*after == ' ' || *after == '\t') {
				++after;
			}
			if (*after == '=') {
				char       replaced[160];
				const bool crlf = length >= 2 && line[length - 2] == '\r';
				const int  n    = snprintf(replaced, sizeof(replaced), "%s = %s%s", key, value, end != nullptr ? (crlf ? "\r\n" : "\n") : "");
				append(replaced, static_cast<size_t>(n));
				done = true;
				line += length;
				continue;
			}
		}
		append(line, length);
		line += length;
	}
	if (!done) {
		char      added[200];
		const int n = seen ? snprintf(added, sizeof(added), "%s = %s\r\n", key, value) : snprintf(added, sizeof(added), "\r\n[%s]\r\n%s = %s\r\n", section, key, value);
		appendLine(added, static_cast<size_t>(n));
	}
	result[used] = '\0';
	return used;
}

void SaveConfigValue(const char* section, const char* key, const char* value) {
	static char text[32768];
	static char result[36864];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		return;
	}
	RewriteConfig(text, section, key, value, result, sizeof(result));
	static_cast<void>(g_ctx->WriteConfig(result));
}

bool MenuSet(const char* key, const char* value, char* error, uint32_t errorSize) {
	const Setting* setting = MenuFind(key);
	if (setting == nullptr || value == nullptr) {
		if (error != nullptr && errorSize > 0) {
			snprintf(error, errorSize, "unknown setting");
		}
		return false;
	}
	while (*value == ' ' || *value == '"') {
		++value;
	}
	SetSetting(*setting, setting->isBool ? ((strncmp(value, "true", 4) == 0 || value[0] == '1') ? 1 : 0) : atoi(value));
	NormalizeConfig();
	char text[32];
	SettingText(*setting, text, sizeof(text));
	SaveConfigValue(setting->section, setting->key, text);
	return true;
}

void JoinSettingsMenu() {
	if (g_menuJoined) {
		return;
	}
	const D2RL::PluginCommunicationService* communication = nullptr;
	if (g_ctx->QueryService(&communication) != D2RL::ServiceQueryResult::Success || communication == nullptr
		|| communication->serviceSize < offsetof(D2RL::PluginCommunicationService, releaseService) + sizeof(void*) || communication->acquireService == nullptr) {
		return;
	}
	D2RL::PluginCommunication::AcquireServiceRequest request {};
	request.structSize       = D2RL::PluginCommunication::AcquireServiceRequestSize;
	request.providerPluginId = "i-hate-tomls";
	request.name             = "settings-menu";
	request.minimumVersion   = 1;
	request.minimumTableSize = 0x30;
	D2RL::PluginCommunication::AcquiredService acquired {};
	acquired.structSize = D2RL::PluginCommunication::AcquiredServiceSize;
	if (communication->acquireService(g_ctx, &request, &acquired) != D2RL::PluginCommunication::Result::Success || acquired.table == nullptr) {
		return; // I Hate TOMLs is not installed, or has not loaded yet
	}

	size_t used = static_cast<size_t>(snprintf(g_menuText, sizeof(g_menuText), "title = \"Horrors in the Night\"\n"));
	for (const Setting& setting : kSettings) {
		if (setting.label == nullptr) {
			continue;
		}
		if (used > sizeof(g_menuText) - 600) {
			Log("settings menu: the list of settings is too long for its buffer - the last ones are missing from the menu");
			break;
		}
		used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "[[setting]]\n"));
		if (setting.section == nullptr) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "type = \"header\"\nlabel = \"%s\"\n", setting.label));
			continue;
		}
		used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "key = \"%s_%s\"\ntype = \"%s\"\nlabel = \"%s\"\n", setting.section, setting.key,
			setting.isBool ? "bool" : "int", setting.label));
		if (setting.note != nullptr) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "note = \"%s\"\n", setting.note));
		}
		if (!setting.isBool) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "min = %d\nmax = %d\nstep = %d\nshift_step = %d\nctrl_step = %d\n", setting.min,
				setting.max, setting.step, setting.step * 5, setting.step * 10));
		}
	}

	g_menuRegistration.structSize  = 0x28;
	g_menuRegistration.flags       = 0;
	g_menuRegistration.description = g_menuText;
	g_menuRegistration.get         = MenuGet;
	g_menuRegistration.set         = MenuSet;
	g_menuRegistration.reserved    = nullptr;
	using AddFn = void* (*)(const D2RL::PluginContext* context, const MenuRegistration* registration);
	const AddFn add = *reinterpret_cast<const AddFn*>(static_cast<const char*>(acquired.table) + 8);
	if (add == nullptr) {
		return;
	}
	void* handle = add(g_ctx, &g_menuRegistration);
	g_menuJoined = handle != nullptr;
	Log("settings menu: %s", g_menuJoined ? "joined I Hate TOMLs - every setting can be changed in game" : "I Hate TOMLs refused the registration");
}

// ---------------------------------------------------------------- engine functions (server)

using Unit = void;
using Room = void;
using Game = void;

using PlayerCallback = void (*)(Game* game, Unit* unit, void* user);

using FrameFn       = void (*)(Game* game);
using IterPlayersFn = void (*)(Game* game, PlayerCallback callback, void* user);
using UnitRoomFn    = Room* (*)(Unit* unit);
using UnitPosFn     = void (*)(Unit* unit, int32_t* xy);
using FindUnitFn    = Unit* (*)(Game* game, uint32_t type, uint32_t id);
using RoomAtFn      = Room* (*)(Room* room, int32_t x, int32_t y);
using RoomLevelFn   = int32_t (*)(Room* room);
using SpawnFn       = Unit* (*)(Game* game, Room* room, int32_t x, int32_t y, int32_t monsterClass, int32_t mode, int32_t search, uint16_t flags);
using KillFn        = void (*)(Game* game, Unit* victim, Unit* killer, int32_t flag);
// unsummon (how the game makes a replaced pet vanish): detach, tell the room's clients, free
using DetachUnitFn  = void (*)(Game* game, Unit* unit, int32_t a, int32_t b, int32_t c, int32_t d);
using RoomForgetFn  = void (*)(Room* room, uint32_t type, uint32_t id);
using RemoveUnitFn  = void (*)(Game* game, Unit* unit);
using GetStatFn     = int32_t (*)(Unit* unit, uint32_t stat, uint16_t layer);
using SetStatFn     = void (*)(Unit* unit, uint32_t stat, int32_t value, uint16_t layer);
using MissileCreateFn = Unit* (*)(Game* game, void* params);
using PlayerClientFn  = void* (*)(Unit* player);
using SendMissileFn   = void (*)(void* client, Game* game, Unit* missile, int32_t flag);
using CastIdFn        = uint32_t (*)(Game* game);
using UnitOwnerFn     = Unit* (*)(Unit* unit);
using UnitDistanceFn  = int32_t (*)(Unit* unit, Unit* other);
using UnitDirectionFn = int32_t (*)(Unit* unit, Unit* other, int32_t a, int32_t b);
using NearestTargetFn = Unit* (*)(Game* game, Unit* unit, int32_t* distance, int32_t* direction);
using MonsterTargetFn = Unit* (*)(Game* game, Unit* unit, void* ai, int32_t* distance, int32_t* direction, uint8_t flag);

struct EngineFunction {
	const char* name;
	uint32_t    rva;
	uint8_t     entry[12];
};

enum : int {
	kIterPlayers, kUnitRoom, kUnitPos, kFindUnit, kRoomAt, kRoomLevel, kSpawn, kKill, kGetStat, kSetStat,
	kMissileCreate, kPlayerClient, kSendMissile, kCastId, kUnitOwner, kUnitDistance, kUnitDirection, kDetachUnit, kRoomForget, kRemoveUnit, kFunctionCount
};

const EngineFunction kFunctions[kFunctionCount] = {
	{ "IterPlayers", 0x490d00, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89 } },
	{ "UnitRoom", 0x34b440, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 } },
	{ "UnitPos", 0x34a040, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b } },
	{ "FindUnit", 0x48fe80, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48 } },
	{ "RoomAt", 0x38dfb0, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x20 } },
	{ "RoomLevel", 0x2efc10, { 0x48, 0x85, 0xc9, 0x75, 0x03, 0x33, 0xc0, 0xc3, 0x48, 0x8b, 0x49, 0x18 } },
	{ "Spawn", 0x492cf0, { 0x4c, 0x8b, 0xdc, 0x48, 0x83, 0xec, 0x68, 0x8b, 0x84, 0x24, 0x90, 0x00 } },
	{ "Kill", 0x44db20, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89 } },
	// the two stat functions are jump thunks into D2RCore; only the opcode is checked
	{ "GetStat", 0x2f5020, { 0xff, 0x25 } },
	{ "SetStat", 0x2f7d10, { 0xff, 0x25 } },
	{ "MissileCreate", 0x5371a0, { 0x40, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xc0, 0x48, 0x81 } },
	{ "PlayerClient", 0x48fde0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 } },
	{ "SendMissile", 0x537bc0, { 0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24 } },
	{ "CastId", 0x3e2b64c, { 0xff, 0x25 } }, // thunk into D2RCore GenerateUnitCastId
	{ "UnitOwner", 0x4a53c0, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8 } },
	{ "UnitDistance", 0x596720, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89 } },
	{ "UnitDirection", 0x348650, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89 } },
	{ "DetachUnit", 0x4a5800, { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83 } },
	{ "RoomForget", 0x2ef4d0, { 0x48, 0x85, 0xc9, 0x74, 0x69, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89 } },
	{ "RemoveUnit", 0x48faa0, { 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b } },
};

const uint8_t kFrameEntry[16] = { 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb9, 0x48, 0x81 };
constexpr uint32_t kFrameRva = 0x406550;

template <typename Fn>
Fn Engine(int index) {
	return reinterpret_cast<Fn>(g_base + kFunctions[index].rva);
}

// unit header: +0x00 type, +0x04 class, +0x08 id, +0x0c mode
uint32_t UnitType(Unit* unit) { return *reinterpret_cast<uint32_t*>(unit); }
uint32_t UnitClass(Unit* unit) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(unit) + 0x04); }
uint32_t UnitId(Unit* unit) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(unit) + 0x08); }
uint32_t UnitMode(Unit* unit) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(unit) + 0x0c); }

uint32_t GameFrame(Game* game) { return *reinterpret_cast<uint32_t*>(static_cast<char*>(game) + 0x170); }
uint8_t  GameBank(Game* game) { return *reinterpret_cast<uint8_t*>(static_cast<char*>(game) + 0x106); }

// ---------------------------------------------------------------- phantoms (server)


constexpr int kMaxClasses  = 600;
constexpr int kMaxLevels   = 256;
constexpr int kMaxRows     = 8192;
constexpr int kMaxPerLevel = 50;
constexpr int kDiabloClass = 243; // monstats row of Diablo
constexpr int kMaxPhantoms = 16;
constexpr int kMaxPlayers  = 8;
constexpr int32_t kPhantomLife = 4000000 * 256; // 4 million life in 1/256 units

struct ClassList {
	int     count = 0;
	int     ids[kMaxClasses] {};   // phantom monstats rows
	int16_t bases[kMaxClasses] {}; // the monstats row each one imitates
	int8_t  acts[kMaxClasses] {};  // -1 = plain twin; 0-4 = townsfolk-named copy for that act
	// direct lookups by monstats row, for the code that runs on every monster think
	uint8_t isPhantom[kMaxRows] {};
	int16_t twin[kMaxRows] {}; // plain twin of this row, 0 = none (row 0 is never a twin)
};

// which monsters belong to a level (from the mod's levels.tsv): [0] Normal, [1] Nightmare/Hell
struct LevelMonsters {
	int     count = 0;
	int16_t bases[kMaxPerLevel] {};
};
LevelMonsters     g_levelMonsters[2][kMaxLevels];
int               g_levelsLoaded = 0;

// What the D2RMM mod says about each level (the extra columns of levels.tsv) and which of its
// options this install was built with (the "#mod" line). An older mod writes neither: then the
// vanilla level lists in DefaultLevelInfo apply.
enum : uint8_t { kFallNone = 0, kFallRain = 1, kFallSnow = 2, kFallSand = 3, kFallAsh = 4 };
struct LevelInfo {
	uint8_t weather; // what falls in the level itself
	uint8_t under;   // what falls outside, for a level that is indoors or sheltered
	uint8_t town;
};
LevelInfo        g_levelInfo[kMaxLevels];
bool             g_levelTableFromMod = false;
std::atomic<int> g_modKnown { 0 };     // levels.tsv carried the "#mod" line
std::atomic<int> g_modSkyFlash { 0 };  // the "day" lighting of stormy areas is the lit-by-lightning look
std::atomic<int> g_modSandstorm { 0 }; // the "day" lighting of the Act 2 desert is the sandstorm look
std::atomic<int> g_modAshstorm { 0 };  // the "day" lighting of Act 4 is the ash storm look

void DefaultLevelInfo() {
	memset(g_levelInfo, 0, sizeof(g_levelInfo));
	struct Range {
		int              first;
		int              last;
		uint8_t LevelInfo::* field;
		uint8_t          value;
	};
	const Range ranges[] = {
		// underground and sheltered: Acts 1 and 3 under the rain, Act 5 under the snow
		{ 8, 37, &LevelInfo::under, kFallRain }, { 84, 102, &LevelInfo::under, kFallRain }, { 133, 133, &LevelInfo::under, kFallRain },
		{ 113, 132, &LevelInfo::under, kFallSnow }, { 135, 135, &LevelInfo::under, kFallSnow },
		// outdoors (levels.txt Rain = 1)
		{ 1, 7, &LevelInfo::weather, kFallRain }, { 17, 17, &LevelInfo::weather, kFallRain }, { 38, 39, &LevelInfo::weather, kFallRain },
		{ 75, 83, &LevelInfo::weather, kFallRain }, { 136, 136, &LevelInfo::weather, kFallRain },
		{ 109, 112, &LevelInfo::weather, kFallSnow }, { 117, 117, &LevelInfo::weather, kFallSnow }, { 120, 121, &LevelInfo::weather, kFallSnow },
		{ 137, 137, &LevelInfo::weather, kFallSnow },
		// the desert
		{ 41, 46, &LevelInfo::weather, kFallSand }, { 134, 134, &LevelInfo::weather, kFallSand },
		{ 1, 1, &LevelInfo::town, 1 }, { 40, 40, &LevelInfo::town, 1 }, { 75, 75, &LevelInfo::town, 1 }, { 103, 103, &LevelInfo::town, 1 }, { 109, 109, &LevelInfo::town, 1 },
	};
	for (const Range& range : ranges) {
		for (int level = range.first; level <= range.last && level < kMaxLevels; ++level) {
			g_levelInfo[level].*range.field = range.value;
		}
	}
	for (LevelInfo& info : g_levelInfo) {
		if (info.weather != kFallNone) {
			info.under = kFallNone;
		}
	}
	g_levelTableFromMod = false;
}

int LevelFall(int level) {
	return level > 0 && level < kMaxLevels ? g_levelInfo[level].weather : 0;
}

int LevelUnder(int level) {
	return level > 0 && level < kMaxLevels ? g_levelInfo[level].under : 0;
}
std::atomic<bool> g_diabloDead { false }; // Diablo died in this game: no more phantoms

ClassList         g_classes[4]; // by data bank 1..3
std::atomic<bool> g_serverReady { false };
std::atomic<bool> g_forceSpawn { false };
std::atomic<int>  g_aliveCount { 0 };
std::atomic<int>  g_totalSpawned { 0 };
std::atomic<int>  g_totalDrains { 0 };

struct Phantom {
	uint32_t id;
	uint32_t expireFrame;
	uint32_t lastMode;
};

Game*    g_game      = nullptr;
uint32_t g_lastFrame = 0;
uint32_t g_nextSpawn = 0;
Phantom  g_phantoms[kMaxPhantoms];
int      g_phantomCount = 0;
FrameFn  g_originalFrame = nullptr;

// Lightning: the client asks, the next server frame drops a harmless bolt near each player.
std::atomic<uint32_t> g_flashRequests { 0 };
uint32_t              g_flashSeen    = 0;
int                   g_flashHitsLeft = 0; // flickers still to come in the current strike
uint32_t              g_flashNextHit = 0;  // frame of the next flicker
bool                  g_flashDirect  = false; // this strike lands on the players
bool                  g_flashFirst   = false; // its first flicker is still to come
int                   g_flashLogs    = 0;
int                   g_lightningRow[4] = { -1, -1, -1, -1 }; // missiles row by data bank
int                   g_lightningSkill[4] = { 57, 57, 57, 57 }; // the row's Skill (Thunder Storm)
std::atomic<int>      g_testRow { -1 };        // "horror test <row>": fire this missiles row once

// Meteors (the ash storm): the same arrangement. The client's storm asks for a volley, the next
// server frames drop its meteors around each player who is out in the ash. An aimed one shows
// the Meteor's marker under the player and is remembered: when it lands, a player still standing
// in it is burned.
struct MeteorBurn {
	uint32_t playerId;
	uint32_t due; // the frame it lands
	int32_t  x;
	int32_t  y;
};
constexpr uint32_t    kMeteorFallFrames = 60; // the Meteor row's Range: from its appearing to its landing
constexpr int         kMeteorReach      = 5;  // how far from where it lands it still burns, in the game's position units
constexpr int         kMaxBurns         = 16;
std::atomic<uint32_t> g_meteorRequests { 0 };
std::atomic<bool>     g_forceMeteor { false }; // "horror meteor": a volley now, also outside the ash
uint32_t              g_meteorSeen     = 0;
int                   g_meteorsLeft    = 0;     // meteors still to come in the current volley
uint32_t              g_meteorNext     = 0;     // frame of the next one
bool                  g_meteorAimed    = false; // this volley's first meteor is aimed at the players
bool                  g_meteorAnywhere = false; // asked for in the console
MeteorBurn            g_burns[kMaxBurns];
int                   g_burnCount      = 0;
int                   g_meteorRow[4]    = { -1, -1, -1, -1 }; // missiles rows by data bank: the plain meteor...
int                   g_meteorHitRow[4] = { -1, -1, -1, -1 }; // ...and the one with the marker
int                   g_meteorSkill[4]  = { 56, 56, 56, 56 }; // their Skill (Meteor)
std::atomic<int>      g_totalMeteors { 0 };
std::atomic<int>      g_totalBurns { 0 };



struct PlayerList {
	int   count = 0;
	Unit* units[kMaxPlayers] {};
};

void CollectPlayer(Game*, Unit* unit, void* user) {
	auto* list = static_cast<PlayerList*>(user);
	if (unit != nullptr && list->count < kMaxPlayers) {
		const uint32_t mode = UnitMode(unit);
		if (mode != 0 && mode != 17) { // not dying, not dead
			list->units[list->count++] = unit;
		}
	}
}

bool IsTown(int level) {
	return level > 0 && level < kMaxLevels && g_levelInfo[level].town != 0;
}

const ClassList* ClassesFor(Game* game) {
	const uint8_t bank = GameBank(game);
	if (bank >= 1 && bank <= 3 && g_classes[bank].count > 0) {
		return &g_classes[bank];
	}
	return nullptr;
}

bool IsPhantomClass(const ClassList* classes, uint32_t classId) {
	return classId < kMaxRows && classes->isPhantom[classId] != 0;
}

void DrainPlayer(Unit* player) {
	const int32_t life = Engine<GetStatFn>(kGetStat)(player, 6, 0); // hitpoints, 1/256 units
	if (life <= 512) {
		return; // 2 life or less: leave it alone
	}
	int64_t loss = static_cast<int64_t>(life) * g_config.drainPercent / 100;
	int64_t left = life - loss;
	if (left < 256) {
		left = 256;
	}
	if (left >= life) {
		return;
	}
	Engine<SetStatFn>(kSetStat)(player, 6, static_cast<int32_t>(left), 0);
	const int count = ++g_totalDrains;
	if (count <= 10) {
		Log("phantoms: hit drained life %d -> %d (1/256 units)", life, static_cast<int32_t>(left));
	}
}

// A phantom twin of a random monster that belongs to this level, or -1.
int PickPhantomClass(Game* game, const ClassList* classes, int level, int wantedAct = -1) {
	if (level <= 0 || level >= kMaxLevels) {
		return -1;
	}
	const uint8_t        difficulty = *reinterpret_cast<uint8_t*>(static_cast<char*>(game) + 0x104);
	const LevelMonsters& monsters   = g_levelMonsters[difficulty == 0 ? 0 : 1][level];
	int                  candidates[kMaxPerLevel];
	int                  count = 0;
	for (int m = 0; m < monsters.count; ++m) {
		for (int i = 0; i < classes->count; ++i) {
			if (classes->bases[i] == monsters.bases[m] && classes->acts[i] == wantedAct) {
				candidates[count++] = classes->ids[i];
				break;
			}
		}
	}
	return count > 0 ? candidates[Rand() % static_cast<uint32_t>(count)] : -1;
}

int ActOfLevel(int level) {
	return level < 40 ? 0 : level < 75 ? 1 : level < 103 ? 2 : level < 109 ? 3 : 4;
}

// The plain twin of one monster class, or -1.
int TwinOf(const ClassList* classes, uint32_t monsterClass) {
	return monsterClass < kMaxRows && classes->twin[monsterClass] > 0 ? classes->twin[monsterClass] : -1;
}

// ---- what the client half needs to know (single player: same process) ----
std::atomic<uint32_t> g_localPlayerId { 0 };
std::atomic<int32_t>  g_realLife { 0 };       // 1/256 units
std::atomic<bool>     g_haunting { false };   // a townsfolk haunting is running
std::atomic<uint32_t> g_hauntCount { 0 };     // bumped when one starts (the client plays a sting)
std::atomic<bool>     g_forceHaunt { false };
uint32_t              g_hauntUntil = 0;

// ---- recent deaths: the corpses a haunting can raise ----
struct Death {
	uint32_t classId;
	int32_t  x;
	int32_t  y;
	uint32_t frame;
	bool     used;
};
Death g_deaths[32];
int   g_deathNext = 0;

// A phantom whose time is up leaves no body: it is taken out of the game the way the game
// unsummons a replaced pet (detach, tell the clients in its room, free). Falls back to a
// plain kill if it has no room.
void Vanish(Game* game, Unit* unit) {
	Room* room = Engine<UnitRoomFn>(kUnitRoom)(unit);
	if (room == nullptr) {
		Engine<KillFn>(kKill)(game, unit, nullptr, 0);
		return;
	}
	const uint32_t id = UnitId(unit);
	Engine<DetachUnitFn>(kDetachUnit)(game, unit, -1, 1, 0, 0);
	Engine<RoomForgetFn>(kRoomForget)(room, 1, id);
	Engine<RemoveUnitFn>(kRemoveUnit)(game, unit);
}

// Puts one phantom of this class at (or next to) the spot. Returns false when the game refuses.
bool SpawnPhantomAt(Game* game, Room* nearRoom, int32_t x, int32_t y, int monsterClass, uint32_t frame, uint32_t lifeFrames, const char* what) {
	if (g_phantomCount >= kMaxPhantoms) {
		return false;
	}
	Room* room = Engine<RoomAtFn>(kRoomAt)(nearRoom, x, y);
	if (room == nullptr) {
		return false;
	}
	// mode 1 = neutral, search 3 = try rings around the spot, flags 0 = plain monster
	Unit* unit = Engine<SpawnFn>(kSpawn)(game, room, x, y, monsterClass, 1, 3, 0);
	if (unit == nullptr) {
		return false;
	}
	// The monstats life columns cannot hold a big enough number, so the life is set here
	// and refilled every frame: a phantom only dies when the plugin removes it.
	Engine<SetStatFn>(kSetStat)(unit, 7, kPhantomLife, 0);
	Engine<SetStatFn>(kSetStat)(unit, 6, kPhantomLife, 0);
	Phantom& phantom    = g_phantoms[g_phantomCount++];
	phantom.id          = UnitId(unit);
	phantom.expireFrame = frame + lifeFrames;
	phantom.lastMode    = UnitMode(unit);
	const int total     = ++g_totalSpawned;
	if (total <= 30) {
		Log("phantoms: %s class %d id %u at %d,%d, lives %u frames", what, monsterClass, phantom.id, x, y, lifeFrames);
	}
	return true;
}

// A haunting: one of the dead wearing a townsperson's name walks up, the corpses around
// you get back to their feet, and (client side) your life orb reads 1 until they are gone.
// Any townsfolk-named row for this act (or, failing that, any act): for levels whose own
// monster list has no undead, so a haunting can still happen there.
int AnyTownsfolk(const ClassList* classes, int act) {
	int candidates[kMaxClasses];
	int count = 0;
	for (int i = 0; i < classes->count; ++i) {
		if (classes->acts[i] == act) {
			candidates[count++] = classes->ids[i];
		}
	}
	if (count == 0) {
		for (int i = 0; i < classes->count; ++i) {
			if (classes->acts[i] >= 0) {
				candidates[count++] = classes->ids[i];
			}
		}
	}
	return count > 0 ? candidates[Rand() % static_cast<uint32_t>(count)] : -1;
}

void LogHauntFailure(int level, const char* why) {
	static int logs = 0;
	if (++logs <= 10) {
		Log("phantoms: no haunting in level %d - %s", level, why);
	}
}

// How long a haunting lasts, in game frames: the phantoms' stay, but at least 8 seconds when the
// longest stay allows it, so there is time for the dead to get up. Never below the shortest stay.
uint32_t HauntingFrames() {
	const int shortest = g_config.maxLife > 8 && g_config.minLife < 8 ? 8 : g_config.minLife;
	return static_cast<uint32_t>(RandRange(shortest, g_config.maxLife)) * 25;
}

bool StartHaunting(Game* game, const ClassList* classes, Unit* player, uint32_t frame, int level) {
	const int act       = ActOfLevel(level);
	int       townsfolk = PickPhantomClass(game, classes, level, act); // one of this level's own undead
	if (townsfolk < 0) {
		townsfolk = AnyTownsfolk(classes, act); // no undead live here: one of the act's dead walks in anyway
	}
	Room* playerRoom = Engine<UnitRoomFn>(kUnitRoom)(player);
	if (townsfolk < 0) {
		LogHauntFailure(level, "the D2RMM mod has no townsfolk rows (\"Townsfolk among the dead\" off?)");
		return false;
	}
	if (playerRoom == nullptr) {
		LogHauntFailure(level, "the player has no room");
		return false;
	}
	int32_t origin[2] = { 0, 0 };
	Engine<UnitPosFn>(kUnitPos)(player, origin);
	const uint32_t life = HauntingFrames();

	bool placed = false;
	for (int attempt = 0; attempt < 8 && !placed; ++attempt) {
		const double angle    = (Rand() % 6283) / 1000.0;
		const int    distance = RandRange(g_config.minDistance, g_config.maxDistance);
		placed = SpawnPhantomAt(game, playerRoom, origin[0] + static_cast<int32_t>(std::cos(angle) * distance), origin[1] + static_cast<int32_t>(std::sin(angle) * distance),
			townsfolk, frame, life, "townsfolk");
	}
	if (!placed) {
		LogHauntFailure(level, "the game refused to place the townsperson");
		return false;
	}

	int risen = 0;
	for (Death& death : g_deaths) {
		if (risen >= g_config.riseMax) {
			break;
		}
		if (death.used || death.frame == 0 || frame - death.frame > 25 * 180) {
			continue; // nothing there, already raised, or older than three minutes
		}
		const int64_t dx = death.x - origin[0];
		const int64_t dy = death.y - origin[1];
		if (dx * dx + dy * dy > 30 * 30) {
			continue;
		}
		const int twin = TwinOf(classes, death.classId);
		if (twin >= 0 && SpawnPhantomAt(game, playerRoom, death.x, death.y, twin, frame, life, "risen corpse")) {
			death.used = true;
			++risen;
		}
	}
	g_hauntUntil = frame + life;
	g_haunting   = true;
	++g_hauntCount;
	Log("phantoms: HAUNTING in level %d - townsfolk class %d, %d corpses rose, %u frames", level, townsfolk, risen, life);
	return true;
}

void SpawnGroup(Game* game, const ClassList* classes, Unit* player, uint32_t frame, int level) {
	Room* playerRoom = Engine<UnitRoomFn>(kUnitRoom)(player);
	if (playerRoom == nullptr) {
		return;
	}
	int32_t origin[2] = { 0, 0 };
	Engine<UnitPosFn>(kUnitPos)(player, origin);

	const int monsterClass = PickPhantomClass(game, classes, level);
	if (monsterClass < 0) {
		static int logs = 0;
		if (++logs <= 5) {
			Log("phantoms: level %d has no monster a phantom can imitate (levels loaded: %d)", level, g_levelsLoaded);
		}
		return;
	}
	int       wanted       = RandRange(1, g_config.maxGroup);
	if (wanted > g_config.maxAlive - g_phantomCount) {
		wanted = g_config.maxAlive - g_phantomCount;
	}

	for (int n = 0; n < wanted && g_phantomCount < kMaxPhantoms; ++n) {
		const uint32_t life = static_cast<uint32_t>(RandRange(g_config.minLife, g_config.maxLife)) * 25;
		for (int attempt = 0; attempt < 8; ++attempt) {
			const double angle    = (Rand() % 6283) / 1000.0;
			const int    distance = RandRange(g_config.minDistance, g_config.maxDistance);
			if (SpawnPhantomAt(game, playerRoom, origin[0] + static_cast<int32_t>(std::cos(angle) * distance), origin[1] + static_cast<int32_t>(std::sin(angle) * distance),
					monsterClass, frame, life, "spawned")) {
				break;
			}
		}
	}
}

void WorldFlash(Game* game, const struct PlayerList& players);
void WorldMeteors(Game* game, const struct PlayerList& players);

void ServerTick(Game* game) {
	const uint32_t frame = GameFrame(game);
	if (game != g_game || frame < g_lastFrame) {
		// a new game: units of the old one are gone with it
		g_game         = game;
		g_phantomCount = 0;
		g_diabloDead   = false;
		g_haunting     = false;
		memset(g_deaths, 0, sizeof(g_deaths));
		g_nextSpawn    = frame + static_cast<uint32_t>(RandRange(20, 60)) * 25;
		// a strike cut short by leaving the old game does not carry on in this one
		g_flashHitsLeft = 0;
		g_flashNextHit  = 0;
		g_flashSeen     = g_flashRequests.load(std::memory_order_relaxed);
		g_meteorsLeft   = 0;
		g_burnCount     = 0;
		g_meteorSeen    = g_meteorRequests.load(std::memory_order_relaxed);
	}
	g_lastFrame = frame;

	// No phantom rows in this game's data (the D2RMM mod's phantoms are off): no phantoms, but the
	// storm's bolts still fall, so this tick goes on for them.
	const ClassList* classes = ClassesFor(game);
	if (classes == nullptr) {
		g_forceSpawn = false;
		g_forceHaunt = false;
	}

	// Most frames have nothing to do: no phantom alive, no strike or volley in progress, no spawn
	// due. (No spawn is ever due with phantoms switched off or Diablo dead: without those two
	// tests every frame of such a game would go the long way round and find nothing to do.)
	const bool spawnDue = classes != nullptr && g_config.phantomsEnabled != 0 && !g_diabloDead.load(std::memory_order_relaxed) && frame >= g_nextSpawn;
	if (g_phantomCount == 0 && g_flashHitsLeft <= 0 && g_flashRequests.load(std::memory_order_relaxed) == g_flashSeen && !spawnDue
		&& g_testRow.load(std::memory_order_relaxed) < 0 && !g_forceSpawn.load(std::memory_order_relaxed) && !g_forceHaunt.load(std::memory_order_relaxed)
		&& g_meteorsLeft <= 0 && g_burnCount == 0 && g_meteorRequests.load(std::memory_order_relaxed) == g_meteorSeen && !g_forceMeteor.load(std::memory_order_relaxed)) {
		if (g_haunting.load(std::memory_order_relaxed)) {
			g_haunting = false;
		}
		g_aliveCount = 0;
		return;
	}

	PlayerList players;
	Engine<IterPlayersFn>(kIterPlayers)(game, CollectPlayer, &players);

	int32_t playerPos[kMaxPlayers][2] {};
	for (int i = 0; i < players.count; ++i) {
		Engine<UnitPosFn>(kUnitPos)(players.units[i], playerPos[i]);
	}

	if (players.count > 0) {
		Unit* local     = players.units[0];
		g_localPlayerId = UnitId(local);
		// what the orb goes back to after a haunting (kept current while one could start or is running)
		g_realLife = Engine<GetStatFn>(kGetStat)(local, 6, 0);
	}
	if (g_haunting.load(std::memory_order_relaxed) && frame >= g_hauntUntil) {
		g_haunting = false;
	}

	WorldFlash(game, players);
	WorldMeteors(game, players);
	if (classes == nullptr) {
		g_aliveCount = 0;
		return; // no phantoms in this game's data
	}

	// existing phantoms: expire, lose, or hit
	for (int i = 0; i < g_phantomCount;) {
		Phantom& phantom = g_phantoms[i];
		Unit*    unit    = Engine<FindUnitFn>(kFindUnit)(game, 1, phantom.id);
		bool     drop    = unit == nullptr || UnitType(unit) != 1 || !IsPhantomClass(classes, UnitClass(unit));
		if (!drop) {
			const uint32_t mode = UnitMode(unit);
			if (mode == 0 || mode == 12) {
				drop = true; // already dying or dead
			} else {
				int32_t position[2] = { 0, 0 };
				Engine<UnitPosFn>(kUnitPos)(unit, position);
				int64_t nearest       = INT64_MAX;
				int     nearestPlayer = -1;
				for (int p = 0; p < players.count; ++p) {
					const int64_t dx   = position[0] - playerPos[p][0];
					const int64_t dy   = position[1] - playerPos[p][1];
					const int64_t dist = dx * dx + dy * dy;
					if (dist < nearest) {
						nearest       = dist;
						nearestPlayer = p;
					}
				}

				if (frame >= phantom.expireFrame || nearestPlayer < 0 || nearest > 45 * 45) {
					Vanish(game, unit);
					drop = true;
				} else {
					// topped up only when hits have taken a real share (no hit comes near half of it),
					// instead of every frame: each write marks the unit for an update to the clients
					if (Engine<GetStatFn>(kGetStat)(unit, 6, 0) < kPhantomLife / 2) {
						Engine<SetStatFn>(kSetStat)(unit, 6, kPhantomLife, 0);
					}
					const bool attacking    = mode == 4 || mode == 5;
					const bool wasAttacking = phantom.lastMode == 4 || phantom.lastMode == 5;
					if (attacking && !wasAttacking && nearest <= 7 * 7) {
						DrainPlayer(players.units[nearestPlayer]);
					}
					phantom.lastMode = mode;
				}
			}
		}

		if (drop) {
			g_phantoms[i] = g_phantoms[--g_phantomCount];
		} else {
			++i;
		}
	}
	g_aliveCount = g_phantomCount;

	// new phantoms
	const bool forcedHaunt = g_forceHaunt.exchange(false);
	const bool forced      = g_forceSpawn.exchange(false) || forcedHaunt;
	if (g_diabloDead.load(std::memory_order_relaxed)) {
		return; // Diablo is dead: the haunting is over for this game
	}
	if (!forced && (g_config.phantomsEnabled == 0 || frame < g_nextSpawn)) {
		return;
	}
	g_nextSpawn = frame + static_cast<uint32_t>(RandRange(g_config.minInterval, g_config.maxInterval)) * 25;
	if (players.count == 0 || (g_phantomCount >= g_config.maxAlive && !forcedHaunt)) {
		return;
	}

	// a random player who is outside town
	const int start = static_cast<int>(Rand() % static_cast<uint32_t>(players.count));
	for (int n = 0; n < players.count; ++n) {
		Unit* player = players.units[(start + n) % players.count];
		Room* room   = Engine<UnitRoomFn>(kUnitRoom)(player);
		if (room == nullptr) {
			continue;
		}
		const int level = Engine<RoomLevelFn>(kRoomLevel)(room);
		if (level <= 0 || IsTown(level)) {
			continue;
		}
		const bool haunt = forcedHaunt || (g_config.hauntChance > 0 && !g_haunting.load() && static_cast<int>(Rand() % 100) < g_config.hauntChance);
		if (!haunt || !StartHaunting(game, classes, player, frame, level)) {
			SpawnGroup(game, classes, player, frame, level);
		}
		g_aliveCount = g_phantomCount;
		return;
	}
	g_nextSpawn = frame + 125; // everyone is in town: look again in 5 seconds
}

// spot: where the bolt lands. {0, 0} means "pick a random place around the player" and is
// filled in with the place picked, so later flickers of the same strike can reuse it.
void StrikeNear(Game* game, Unit* player, int missileRow, int skill, int minDistance, int maxDistance, int32_t* spot = nullptr) {
	int32_t origin[2] = { 0, 0 };
	Engine<UnitPosFn>(kUnitPos)(player, origin);
	int32_t x = 0;
	int32_t y = 0;
	if (spot != nullptr && (spot[0] != 0 || spot[1] != 0)) {
		x = spot[0];
		y = spot[1];
	} else {
		const double angle    = (Rand() % 6283) / 1000.0;
		const int    distance = RandRange(minDistance, maxDistance);
		x = origin[0] + static_cast<int32_t>(std::cos(angle) * distance);
		y = origin[1] + static_cast<int32_t>(std::sin(angle) * distance);
		if (spot != nullptr) {
			spot[0] = x;
			spot[1] = y;
		}
	}

	alignas(16) uint8_t params[0xa0] = {};
	const uint32_t flags = 1;
	const int32_t  level = 1;
	const uint32_t castId = Engine<CastIdFn>(kCastId)(game);
	memcpy(params + 0x00, &flags, 4);
	memcpy(params + 0x08, &player, 8);
	memcpy(params + 0x20, &missileRow, 4);
	memcpy(params + 0x24, &x, 4);
	memcpy(params + 0x28, &y, 4);
	memcpy(params + 0x2c, &x, 4);
	memcpy(params + 0x30, &y, 4);
	// The HD bolt shows nothing when its source skill is 0 (Attack), so it carries Thunder Storm.
	memcpy(params + 0x3c, &skill, 4);
	memcpy(params + 0x40, &level, 4);
	memcpy(params + 0x68, &castId, 4);

	Unit* missile = Engine<MissileCreateFn>(kMissileCreate)(game, params);
	if (missile != nullptr) {
		void* client = Engine<PlayerClientFn>(kPlayerClient)(player);
		if (client != nullptr) {
			Engine<SendMissileFn>(kSendMissile)(client, game, missile, 1);
		}
	}
	if (++g_flashLogs <= 12) {
		Log("storm: missile row %d skill %d at %d,%d (player %d,%d) -> %s, server unit id %u mode %u", missileRow, skill, x, y, origin[0], origin[1],
			missile != nullptr ? "created" : "FAILED", missile != nullptr ? UnitId(missile) : 0, missile != nullptr ? UnitMode(missile) : 0);
	}
}

// A direct hit: a share of MAXIMUM life, but it always leaves at least 1 life.
// Takes a share of MAXIMUM life, but always leaves at least 1 life. False when nothing was taken.
bool HurtPlayer(Unit* player, int percent, const char* what) {
	if (percent <= 0) {
		return false;
	}
	const int32_t life    = Engine<GetStatFn>(kGetStat)(player, 6, 0);
	const int32_t maxLife = Engine<GetStatFn>(kGetStat)(player, 7, 0);
	if (life <= 256 || maxLife <= 0) {
		return false;
	}
	int64_t left = life - static_cast<int64_t>(maxLife) * percent / 100;
	if (left < 256) {
		left = 256;
	}
	Engine<SetStatFn>(kSetStat)(player, 6, static_cast<int32_t>(left), 0);
	static int logs = 0;
	if (++logs <= 10) {
		Log("storm: %s %d%% - life %d -> %d of max %d (1/256 units)", what, percent, life, static_cast<int32_t>(left), maxLife);
	}
	return true;
}

void LightningDamage(Unit* player) {
	HurtPlayer(player, RandRange(g_config.directHitDamage, g_config.directHitDamageMax), "DIRECT HIT");
}

int RowSkill(uint32_t bank, int row) {
	D2RL::DataTables::TableView view {};
	view.structSize = D2RL::DataTables::TableViewSize;
	if (g_tables == nullptr || g_tables->getTable(g_ctx, static_cast<D2RL::DataTables::Bank>(bank), D2RL::DataTables::TableId::Missiles, &view) != D2RL::DataTables::Result::Success
		|| view.rows == nullptr || view.rowSize < 0x1b8 || row < 0 || static_cast<uint32_t>(row) >= view.rowCount) {
		return -1;
	}
	int16_t skill = -1;
	memcpy(&skill, static_cast<const uint8_t*>(view.rows) + static_cast<size_t>(row) * view.rowSize + 0x1b6, 2);
	return skill;
}

void WorldFlash(Game* game, const PlayerList& players) {
	const int testRow = g_testRow.exchange(-1);
	if (testRow >= 0 && players.count > 0) {
		const int skill = RowSkill(GameBank(game), testRow);
		if (skill < -1 || RowSkill(GameBank(game), 0) < -1) {
			return;
		}
		Log("storm: test missile row %d (its skill %d)", testRow, skill);
		StrikeNear(game, players.units[0], testRow, skill > 0 ? skill : 0, 3, 9);
		return;
	}
	// A strike is several hits a few frames apart; each hit is a cluster of bolts:
	// one close to the player and the rest spread out, so the whole view lights up.
	int&            hitsLeft     = g_flashHitsLeft;
	uint32_t&       nextHit      = g_flashNextHit;
	bool&           directHit    = g_flashDirect;
	bool&           firstFlicker = g_flashFirst;
	static int32_t  spots[kMaxPlayers][8][2]; // where this strike's bolts land: only the first flicker is random (cleared when a strike starts)
	const uint32_t  frame    = GameFrame(game);
	const uint32_t  requests = g_flashRequests.load();
	if (requests != g_flashSeen) {
		g_flashSeen = requests;
		hitsLeft    = RandRange(g_config.hitsMin, g_config.hitsMax);
		nextHit     = frame;
		directHit   = g_config.directHits != 0 && static_cast<int>(Rand() % 100) < g_config.directHitChance;
		firstFlicker = true;
		memset(spots, 0, sizeof(spots));
	}
	if (hitsLeft <= 0 || frame < nextHit) {
		return;
	}
	--hitsLeft;
	const bool first = firstFlicker;
	firstFlicker     = false;
	nextHit = frame + static_cast<uint32_t>(RandRange(2, 7));

	const uint8_t bank = GameBank(game);
	int           row  = bank <= 3 ? g_lightningRow[bank] : -1;
	int           boltSkill = bank <= 3 ? g_lightningSkill[bank] : 0;
	if (g_config.boltRow >= 0) {
		row       = g_config.boltRow;
		boltSkill = RowSkill(bank, row);
		if (boltSkill < 0) {
			boltSkill = 0;
		}
	}
	if (row < 0) {
		hitsLeft = 0;
		if (++g_flashLogs <= 5) {
			Log("storm: no lightning missile row in bank %u (is the D2RMM mod's lightning option on?)", bank);
		}
		return;
	}
	for (int i = 0; i < players.count; ++i) {
		Room* room = Engine<UnitRoomFn>(kUnitRoom)(players.units[i]);
		const int level = room != nullptr ? Engine<RoomLevelFn>(kRoomLevel)(room) : 0;
		if (level <= 0 || IsTown(level)) {
			continue;
		}
		for (int bolt = 0; bolt < g_config.boltsPerHit && bolt < 8; ++bolt) {
			// every flicker of a strike hits the spot its first flicker picked
			if (bolt == 0 && directHit) {
				StrikeNear(game, players.units[i], row, boltSkill, 0, 1, spots[i][bolt]);
				if (first) {
					LightningDamage(players.units[i]); // a direct hit hurts once, on the first flicker
				}
			} else {
				StrikeNear(game, players.units[i], row, boltSkill, g_config.boltMinDistance, g_config.boltSpread, spots[i][bolt]);
			}
		}
	}
}

// The ash storm's meteors. A volley is a few meteors in quick succession, each landing
// somewhere around every player who is out in the ash. With aimed meteors on and the roll made,
// the volley's first is aimed: it lands where the player stands now, with the Meteor's marker on
// the ground, and burns a player who is still there when it lands.
void WorldMeteors(Game* game, const PlayerList& players) {
	const uint32_t frame = GameFrame(game);

	// aimed meteors that have landed
	for (int i = 0; i < g_burnCount;) {
		const MeteorBurn burn = g_burns[i];
		if (static_cast<int32_t>(frame - burn.due) < 0) {
			++i;
			continue;
		}
		for (int p = 0; p < players.count; ++p) {
			if (UnitId(players.units[p]) != burn.playerId) {
				continue;
			}
			int32_t at[2] = { 0, 0 };
			Engine<UnitPosFn>(kUnitPos)(players.units[p], at);
			const int64_t dx = at[0] - burn.x;
			const int64_t dy = at[1] - burn.y;
			if (dx * dx + dy * dy <= kMeteorReach * kMeteorReach
				&& HurtPlayer(players.units[p], RandRange(g_config.meteorHitDamage, g_config.meteorHitDamageMax), "METEOR HIT")) {
				++g_totalBurns;
			}
		}
		g_burns[i] = g_burns[--g_burnCount];
	}

	const uint32_t requests = g_meteorRequests.load();
	const bool     forced   = g_forceMeteor.exchange(false);
	if (requests != g_meteorSeen || forced) {
		g_meteorSeen     = requests;
		g_meteorsLeft    = RandRange(g_config.meteorsMin, g_config.meteorsMax);
		g_meteorNext     = frame;
		g_meteorAimed    = g_config.meteorHits != 0 && static_cast<int>(Rand() % 100) < g_config.meteorHitChance;
		g_meteorAnywhere = forced;
	}
	const uint8_t bank   = GameBank(game);
	const int     row    = bank <= 3 ? g_meteorRow[bank] : -1;
	const int     hitRow = bank <= 3 ? g_meteorHitRow[bank] : -1;
	const int     skill  = bank <= 3 ? g_meteorSkill[bank] : 0;
	// one meteor after another, the configured gap apart (in game frames, 40 ms each; with no gap
	// the whole volley falls in this frame)
	while (g_meteorsLeft > 0 && static_cast<int32_t>(frame - g_meteorNext) >= 0) {
		--g_meteorsLeft;
		g_meteorNext = frame + static_cast<uint32_t>((RandRange(g_config.meteorGapMin, g_config.meteorGapMax) + 20) / 40);
		if (row < 0) {
			g_meteorsLeft = 0;
			static int logs = 0;
			if (++logs <= 3) {
				Log("ash storm: no meteor missile row in bank %u (is the D2RMM mod's Ash storms option on, and installed?)", bank);
			}
			return;
		}
		const bool aimed = g_meteorAimed && hitRow >= 0;
		g_meteorAimed    = false; // only the first of a volley
		for (int i = 0; i < players.count; ++i) {
			Room*     room  = Engine<UnitRoomFn>(kUnitRoom)(players.units[i]);
			const int level = room != nullptr ? Engine<RoomLevelFn>(kRoomLevel)(room) : 0;
			if (level <= 0 || IsTown(level) || (!g_meteorAnywhere && LevelFall(level) != kFallAsh)) {
				continue;
			}
			if (aimed && g_burnCount < kMaxBurns) {
				int32_t spot[2] = { 0, 0 };
				StrikeNear(game, players.units[i], hitRow, skill, 0, 1, spot);
				g_burns[g_burnCount++] = MeteorBurn { UnitId(players.units[i]), frame + kMeteorFallFrames, spot[0], spot[1] };
			} else {
				StrikeNear(game, players.units[i], row, skill, g_config.meteorMinDistance, g_config.meteorMaxDistance);
			}
			++g_totalMeteors;
		}
	}
}

// ---------------------------------------------------------------- target hooks
// Mercenaries (and other good units) never pick a phantom; a phantom that picks a
// mercenary or summon goes after that unit's owner instead.

NearestTargetFn g_originalNearest = nullptr;
MonsterTargetFn g_originalMonster = nullptr;
std::atomic<int> g_mercIgnores { 0 };
std::atomic<int> g_redirects { 0 };

bool IsPhantom(Game* game, Unit* unit) {
	if (game == nullptr || unit == nullptr || UnitType(unit) != 1) {
		return false;
	}
	const ClassList* classes = ClassesFor(game);
	return classes != nullptr && IsPhantomClass(classes, UnitClass(unit));
}

Unit* NearestTargetHook(Game* game, Unit* unit, int32_t* distance, int32_t* direction) {
	Unit* target = g_originalNearest(game, unit, distance, direction);
	// almost every call ends at the first tests: a target that is not a monster, or no phantoms about
	if (target != nullptr && UnitType(target) == 1 && g_phantomCount > 0 && IsPhantom(game, target) && !IsPhantom(game, unit)) {
		if (distance != nullptr) {
			*distance = 0x7fffffff;
		}
		if (direction != nullptr) {
			*direction = 0;
		}
		if (++g_mercIgnores <= 3) {
			Log("phantoms: a %s ignored a phantom", unit != nullptr && UnitType(unit) == 1 ? "monster/mercenary" : "unit");
		}
		return nullptr;
	}
	return target;
}

Unit* MonsterTargetHook(Game* game, Unit* unit, void* ai, int32_t* distance, int32_t* direction, uint8_t flag) {
	Unit* target = g_originalMonster(game, unit, ai, distance, direction, flag);
	if (target == nullptr || UnitType(target) == 0 || g_phantomCount == 0 || !IsPhantom(game, unit)) {
		return target; // a player target, or no phantoms about: nothing to do
	}
	Unit* owner = Engine<UnitOwnerFn>(kUnitOwner)(target);
	if (owner != nullptr && UnitType(owner) == 0 && UnitMode(owner) != 0 && UnitMode(owner) != 17) {
		if (distance != nullptr) {
			*distance = Engine<UnitDistanceFn>(kUnitDistance)(unit, owner);
		}
		if (direction != nullptr) {
			*direction = Engine<UnitDirectionFn>(kUnitDirection)(unit, owner, 0, 0);
		}
		if (++g_redirects <= 3) {
			Log("phantoms: phantom turned from a mercenary/summon to its owner");
		}
		return owner;
	}
	if (distance != nullptr) {
		*distance = 0x7fffffff;
	}
	if (direction != nullptr) {
		*direction = 0;
	}
	return nullptr;
}

void FrameHook(Game* game) {
	g_originalFrame(game);
	if (game != nullptr && g_serverReady.load(std::memory_order_relaxed)) {
		ServerTick(game);
	}
}

// The missiles rows the mod marked for the plugin: Param4 = 7777 and Param5 = 4242 (the
// lightning bolt), 4243 (the meteor), 4244 (the meteor with its marker).
void FindLightningRow(const D2RL::PluginContext* context, uint32_t bank) {
	g_lightningRow[bank] = -1;
	g_meteorRow[bank]    = -1;
	g_meteorHitRow[bank] = -1;
	D2RL::DataTables::TableView view {};
	view.structSize = D2RL::DataTables::TableViewSize;
	if (g_tables->getTable(context, static_cast<D2RL::DataTables::Bank>(bank), D2RL::DataTables::TableId::Missiles, &view) != D2RL::DataTables::Result::Success
		|| view.rows == nullptr || view.rowSize < 0x70) {
		return;
	}
	const auto* rows = static_cast<const uint8_t*>(view.rows);
	for (uint32_t row = 0; row < view.rowCount; ++row) {
		int32_t param4 = 0;
		int32_t param5 = 0;
		memcpy(&param4, rows + static_cast<size_t>(row) * view.rowSize + 0x68, 4);
		memcpy(&param5, rows + static_cast<size_t>(row) * view.rowSize + 0x6c, 4);
		if (param4 != 7777 || param5 < 4242 || param5 > 4244) {
			continue;
		}
		int16_t skill = -1;
		if (view.rowSize >= 0x1b8) {
			memcpy(&skill, rows + static_cast<size_t>(row) * view.rowSize + 0x1b6, 2);
		}
		if (param5 == 4242 && g_lightningRow[bank] < 0) {
			g_lightningRow[bank]   = static_cast<int>(row);
			g_lightningSkill[bank] = skill > 0 ? skill : 57;
		} else if (param5 == 4243 && g_meteorRow[bank] < 0) {
			g_meteorRow[bank]   = static_cast<int>(row);
			g_meteorSkill[bank] = skill > 0 ? skill : 56;
		} else if (param5 == 4244 && g_meteorHitRow[bank] < 0) {
			g_meteorHitRow[bank] = static_cast<int>(row);
		}
	}
	Log("storm: missiles bank %u has %u rows of 0x%x bytes, lightning row %d (skill %d), meteor rows %d and %d (skill %d)", bank, view.rowCount, view.rowSize, g_lightningRow[bank],
		g_lightningSkill[bank], g_meteorRow[bank], g_meteorHitRow[bank], g_meteorSkill[bank]);
}

// Reads the text of levels.tsv, written by the D2RMM mod. The text is cut up while it is read.
//   #mod <tab> alwaysNight=1 <tab> skyFlash=1 <tab> sandstorm=1 <tab> ashstorm=1   (the mod's own options)
//   level <tab> monsters on Normal <tab> monsters on Nightmare/Hell  (monstats row numbers)
//         [<tab> weather <tab> town <tab> under]                     (see LevelInfo; newer mods only)
//         [<tab> ash]                                                (the Act 4 ash storm; newer still)
// Returns the number of level lines.
int ParseLevels(char* text) {
	memset(g_levelMonsters, 0, sizeof(g_levelMonsters));
	DefaultLevelInfo();
	g_modKnown     = 0;
	g_modSkyFlash  = 0;
	g_modSandstorm = 0;
	g_modAshstorm  = 0;
	int   loaded   = 0;
	bool  overflow = false;
	char* line     = text;
	while (*line != '\0') {
		char* end = line;
		while (*end != '\0' && *end != '\n') {
			++end;
		}
		const bool last = *end == '\0';
		*end            = '\0';
		if (end > line && end[-1] == '\r') {
			end[-1] = '\0';
		}
		if (line[0] == '#' && line[1] == 'm' && line[2] == 'o' && line[3] == 'd') {
			g_modKnown     = 1;
			g_modSkyFlash  = strstr(line, "skyFlash=1") != nullptr ? 1 : 0;
			g_modSandstorm = strstr(line, "sandstorm=1") != nullptr ? 1 : 0;
			g_modAshstorm  = strstr(line, "ashstorm=1") != nullptr ? 1 : 0;
		} else if (*line >= '0' && *line <= '9') {
			char* fields[7] = { line };
			int   count     = 1;
			for (char* c = line; *c != '\0' && count < 7; ++c) {
				if (*c == '\t') {
					*c              = '\0';
					fields[count++] = c + 1;
				}
			}
			const int level = atoi(fields[0]);
			if (level > 0 && level < kMaxLevels) {
				for (int column = 0; column < 2 && column + 1 < count; ++column) {
					LevelMonsters& monsters = g_levelMonsters[column][level];
					for (const char* c = fields[column + 1]; *c != '\0';) {
						if (*c >= '0' && *c <= '9') {
							if (monsters.count < kMaxPerLevel) {
								monsters.bases[monsters.count++] = static_cast<int16_t>(atoi(c));
							} else {
								overflow = true;
							}
							while (*c >= '0' && *c <= '9') {
								++c;
							}
						} else {
							++c;
						}
					}
				}
				if (count >= 6) {
					if (!g_levelTableFromMod) {
						memset(g_levelInfo, 0, sizeof(g_levelInfo)); // the mod's table replaces the vanilla lists
						g_levelTableFromMod = true;
					}
					g_levelInfo[level].weather = static_cast<uint8_t>(Clamp(atoi(fields[3]), 0, 3));
					g_levelInfo[level].town    = atoi(fields[4]) != 0 ? 1 : 0;
					g_levelInfo[level].under   = static_cast<uint8_t>(Clamp(atoi(fields[5]), 0, 3));
				}
				// the ash storm has a column of its own (1 it blows here, 2 sheltered from it), so
				// that the columns before it mean to an older plugin what they always did
				if (count >= 7) {
					const int ash = atoi(fields[6]);
					if (ash == 1 && g_levelInfo[level].weather == kFallNone) {
						g_levelInfo[level].weather = kFallAsh;
					} else if (ash == 2 && g_levelInfo[level].weather == kFallNone && g_levelInfo[level].under == kFallNone) {
						g_levelInfo[level].under = kFallAsh;
					}
				}
			}
			++loaded;
		}
		if (last) {
			break;
		}
		line = end + 1;
	}
	if (overflow) {
		Log("phantoms: a level lists more than %d monsters - the rest are left out", kMaxPerLevel);
	}
	return loaded;
}

// Reads <mod>\<mod>.mpq\data\global\excel\d2rloader\horror-night\levels.tsv.
void LoadLevelMonsters() {
	wchar_t path[1024] = {};
	const DWORD length = GetModuleFileNameW(g_module, path, 700);
	if (length == 0 || length >= 700) {
		return;
	}
	// ...\mods\<mod>\d2rloader\plugins\<dll>  ->  ...\mods\<mod>
	for (int cut = 0; cut < 3; ++cut) {
		wchar_t* slash = wcsrchr(path, L'\\');
		if (slash == nullptr) {
			return;
		}
		*slash = L'\0';
	}
	const wchar_t* name = wcsrchr(path, L'\\');
	if (name == nullptr) {
		return;
	}
	wchar_t file[1400] = {};
	swprintf(file, 1400, L"%ls\\%ls.mpq\\data\\global\\excel\\d2rloader\\horror-night\\levels.tsv", path, name + 1);

	HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		Log("levels: could not open %ls (run Install in D2RMM with the Horrors in the Night mod on)", file);
		return;
	}
	static char text[262144];
	DWORD       read = 0;
	const BOOL  ok   = ReadFile(handle, text, sizeof(text) - 1, &read, nullptr);
	CloseHandle(handle);
	if (!ok) {
		return;
	}
	if (read == sizeof(text) - 1) {
		Log("levels: levels.tsv is larger than %u bytes - the end of it is not read", static_cast<unsigned>(sizeof(text) - 1));
	}
	text[read] = '\0';

	g_levelsLoaded = ParseLevels(text);
	Log("levels: %d read (level 2 on Normal: %d monsters); weather and towns from %s; mod options %s%s%s%s", g_levelsLoaded, g_levelMonsters[0][2].count,
		g_levelTableFromMod ? "the mod's table" : "the built-in vanilla lists (older mod)",
		g_modKnown.load() != 0 ? "known" : "not given (older mod)", g_modSkyFlash.load() != 0 ? ", lightning sky" : "", g_modSandstorm.load() != 0 ? ", sandstorm" : "",
		g_modAshstorm.load() != 0 ? ", ash storm" : "");
}

void __cdecl OnMonsterDeath(const D2RL::PluginContext*, const D2RL::Lifecycle::MonsterDeathEvent* event, void*) noexcept {
	if (!D2RL::Lifecycle::HasMonsterDeathEventField(event, D2RL::Lifecycle::MonsterDeathEventRequiredSize)) {
		return;
	}
	if (event->monster.classId == kDiabloClass && !g_diabloDead.exchange(true)) {
		Log("phantoms: Diablo is dead - no more phantoms in this game");
	}
	// remember where it fell, so a haunting can raise it
	Game* game = g_game;
	if (game == nullptr || !g_serverReady.load(std::memory_order_relaxed)) {
		return;
	}
	const ClassList* classes = ClassesFor(game);
	if (classes == nullptr || IsPhantomClass(classes, event->monster.classId) || TwinOf(classes, event->monster.classId) < 0) {
		return;
	}
	Unit* unit = Engine<FindUnitFn>(kFindUnit)(game, 1, event->monster.id);
	if (unit == nullptr || UnitType(unit) != 1) {
		return;
	}
	int32_t position[2] = { 0, 0 };
	Engine<UnitPosFn>(kUnitPos)(unit, position);
	Death& death  = g_deaths[g_deathNext++ % 32];
	death.classId = event->monster.classId;
	death.x       = position[0];
	death.y       = position[1];
	death.frame   = GameFrame(game);
	death.used    = false;
}

// Finds the phantom rows the D2RMM mod added to monstats (marked through aip8).
void __cdecl OnTablesLoaded(const D2RL::PluginContext* context, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	if (g_tables == nullptr) {
		return;
	}
	LoadLevelMonsters();
	for (uint32_t bank = 1; bank <= 3; ++bank) {
		g_classes[bank].count = 0;
		memset(g_classes[bank].isPhantom, 0, sizeof(g_classes[bank].isPhantom));
		memset(g_classes[bank].twin, 0, sizeof(g_classes[bank].twin));
		D2RL::DataTables::TableView view {};
		view.structSize = D2RL::DataTables::TableViewSize;
		const auto result = g_tables->getTable(context, static_cast<D2RL::DataTables::Bank>(bank), D2RL::DataTables::TableId::MonStats, &view);
		if (result != D2RL::DataTables::Result::Success || view.rows == nullptr) {
			Log("phantoms: monstats bank %u not available (result %u)", bank, static_cast<uint32_t>(result));
			continue;
		}
		if (view.rowSize != kMonstatsSize) {
			Log("phantoms: monstats bank %u row size is 0x%x, expected 0x%x - phantoms off for this bank", bank, view.rowSize, kMonstatsSize);
			continue;
		}
		const auto* rows    = static_cast<const uint8_t*>(view.rows);
		int         skipped = 0;
		for (uint32_t row = 0; row < view.rowCount; ++row) {
			int16_t aip8[3];
			memcpy(aip8, rows + static_cast<size_t>(row) * kMonstatsSize + kAip8Offset, sizeof(aip8));
			if (aip8[0] == kPhantomMark && aip8[2] >= kPhantomMark && aip8[2] <= kPhantomMark + 5 && aip8[1] >= 0 && g_classes[bank].count >= kMaxClasses) {
				++skipped;
			} else if (aip8[0] == kPhantomMark && aip8[2] >= kPhantomMark && aip8[2] <= kPhantomMark + 5 && aip8[1] >= 0) {
				g_classes[bank].acts[g_classes[bank].count]  = static_cast<int8_t>(aip8[2] - kPhantomMark - 1);
				if (row < kMaxRows) {
					g_classes[bank].isPhantom[row] = 1;
					if (aip8[2] == kPhantomMark && aip8[1] < kMaxRows) {
						g_classes[bank].twin[aip8[1]] = static_cast<int16_t>(row);
					}
				}
				g_classes[bank].bases[g_classes[bank].count] = aip8[1];
				g_classes[bank].ids[g_classes[bank].count++] = static_cast<int>(row);
			}
		}
		int16_t zombieAip[2] = { 0, 0 }; // layout check: vanilla zombie1 (row 5) has aip1 30, aip2 10
		if (view.rowCount > 5) {
			memcpy(&zombieAip[0], rows + 5 * kMonstatsSize + 0x90, 2);
			memcpy(&zombieAip[1], rows + 5 * kMonstatsSize + 0x96, 2);
		}
		if (skipped > 0) {
			Log("phantoms: monstats bank %u has more than %d phantom rows - %d are not used", bank, kMaxClasses, skipped);
		}
		FindLightningRow(context, bank);
		Log("phantoms: monstats bank %u has %u rows, %d phantom rows (first %d); layout check zombie aip1=%d aip2=%d", bank, view.rowCount, g_classes[bank].count,
			g_classes[bank].count > 0 ? g_classes[bank].ids[0] : -1, zombieAip[0], zombieAip[1]);
	}
}

bool SetUpServer() {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
	const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
	if (nt->FileHeader.TimeDateStamp != kImageStamp) {
		Log("phantoms: off - game image 0x%08lx is not the supported 0x%08x", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), kImageStamp);
		return false;
	}
	for (const EngineFunction& function : kFunctions) {
		uint32_t size = function.entry[0] == 0xff ? 2 : static_cast<uint32_t>(sizeof(function.entry));
		while (size > 2 && function.entry[size - 1] == 0) {
			--size; // shorter entries are zero padded
		}
		if (!g_ctx->CheckExpectedBytes(function.rva, function.entry, size)) {
			Log("phantoms: off - %s at 0x%x does not look as expected", function.name, function.rva);
			return false;
		}
	}

	const D2RL::LifecycleService* lifecycle = nullptr;
	if (g_ctx->QueryService(&g_tables) != D2RL::ServiceQueryResult::Success || !D2RL::HasDataTableServiceField(g_tables, D2RL::DataTableServiceRequiredSize)
		|| g_ctx->QueryService(&lifecycle) != D2RL::ServiceQueryResult::Success
		|| !D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, unregisterDataTablesLoadedListener))) {
		Log("phantoms: off - the data table or lifecycle service is missing");
		return false;
	}
	const D2RL::Lifecycle::DataTablesLoadedListener listener {
		.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
		.flags      = 0,
		.callback   = OnTablesLoaded,
		.userData   = nullptr,
	};
	D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
	if (lifecycle->registerDataTablesLoadedListener(g_ctx, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
		Log("phantoms: off - could not listen for table loads");
		return false;
	}

	if (D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize) && lifecycle->registerMonsterDeathListener != nullptr) {
		const D2RL::Lifecycle::MonsterDeathListener deaths {
			.structSize = D2RL::Lifecycle::MonsterDeathListenerSize,
			.flags      = 0,
			.callback   = OnMonsterDeath,
			.userData   = nullptr,
		};
		D2RL::Lifecycle::ListenerHandle deathHandle = D2RL::Lifecycle::InvalidHandle;
		const auto result = lifecycle->registerMonsterDeathListener(g_ctx, &deaths, &deathHandle);
		Log("phantoms: Diablo watch %s", result == D2RL::Lifecycle::Result::Success ? "on" : "FAILED - phantoms will not stop after Diablo");
	} else {
		Log("phantoms: this D2RLoader cannot report monster deaths - phantoms will not stop after Diablo");
	}

	const D2RL::PluginApi* api = g_ctx->GetApi();
	D2RL::InlineHookRegistration hook = D2RL::MakeInlineHook(kFrameRva, kFrameEntry, sizeof(kFrameEntry), reinterpret_cast<void*>(&FrameHook),
		reinterpret_cast<void**>(&g_originalFrame));
	if (api == nullptr || D2RL::ApiInstallInlineHook(api) == nullptr || !D2RL::ApiInstallInlineHook(api)(g_ctx, &hook) || g_originalFrame == nullptr) {
		Log("phantoms: off - could not hook the game frame (another plugin may already hook it)");
		return false;
	}
	const uint8_t kNearestEntry[16] = { 0x4c, 0x8b, 0xdc, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x50 };
	const uint8_t kMonsterEntry[16] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x40, 0xff, 0xff, 0xff, 0x48 };
	D2RL::InlineHookRegistration nearest = D2RL::MakeInlineHook(0x595f80, kNearestEntry, sizeof(kNearestEntry), reinterpret_cast<void*>(&NearestTargetHook),
		reinterpret_cast<void**>(&g_originalNearest));
	D2RL::InlineHookRegistration monster = D2RL::MakeInlineHook(0x595750, kMonsterEntry, sizeof(kMonsterEntry), reinterpret_cast<void*>(&MonsterTargetHook),
		reinterpret_cast<void**>(&g_originalMonster));
	const bool nearestOk = D2RL::ApiInstallInlineHook(api)(g_ctx, &nearest) && g_originalNearest != nullptr;
	const bool monsterOk = D2RL::ApiInstallInlineHook(api)(g_ctx, &monster) && g_originalMonster != nullptr;
	Log("phantoms: mercenary target hook %s, phantom target hook %s", nearestOk ? "on" : "FAILED", monsterOk ? "on" : "FAILED");

	g_serverReady = true;
	Log("phantoms: on - every %d-%d s, life %d-%d s, up to %d alive, drain %d%%", g_config.minInterval, g_config.maxInterval, g_config.minLife, g_config.maxLife,
		g_config.maxAlive, g_config.drainPercent);
	return true;
}

// ---------------------------------------------------------------- storm (client)

std::atomic<int>  g_level { -1 };
std::atomic<bool> g_forceStrike { false };
int               g_levelLogs = 0;

// ---- sound ----
// The sounds are loaded on a background thread and played by one mixer thread through one
// audio stream, so the game's threads never wait on the audio device or touch sample data:
// PlayClip only queues a request.
constexpr int kThunderCount = 4;
constexpr int kAmbientCount = 19; // 0-3 breath, 4-11 moan, 12-15 scream, 16-17 raven, 18 laugh
constexpr int kWindCount    = 9;  // gusts of a few seconds each, for the storms
constexpr int kFireCount    = 2;  // the ash storm's fire: a low roar, crackling embers
constexpr int kBurstCount   = 6;  // ...and its steam vents (0-2) and rockfalls (3-5)
struct Clip {
	const int16_t* samples = nullptr;
	uint32_t       count   = 0;
	uint32_t       rate    = 22050;
};
Clip              g_thunder[kThunderCount];
Clip              g_ambient[kAmbientCount];
Clip              g_wind[kWindCount];
Clip              g_fire[kFireCount];
Clip              g_burst[kBurstCount];
std::atomic<bool> g_soundsReady { false }; // set by the loader thread once every clip above is final
HANDLE            g_soundLoader = nullptr;

uint64_t g_nextStrike  = 0; // tick count of the next strike, 0 = not scheduled
uint64_t g_flashStart  = 0;
uint64_t g_thunderAt   = 0;
float    g_thunderGain = 1.0f;
bool     g_flashNow    = false;
bool     g_flashLogged = false;
int      g_strikeCount = 0;

enum class Area { None, Outdoor, Indoor };

Area StormArea(int level) {
	const int fall = LevelFall(level);
	if (fall == kFallRain || (fall == kFallSnow && g_config.snowAreas != 0)) {
		return Area::Outdoor;
	}
	if (g_config.indoorThunder != 0 && fall == kFallNone) {
		const int under = LevelUnder(level);
		if (under == kFallRain || (under == kFallSnow && g_config.snowAreas != 0)) {
			return Area::Indoor;
		}
	}
	return Area::None;
}

// The sounds are the game's own (Blizzard's), so the DLL does not carry them: they are read out of
// the installed game's data at start-up (see game_sounds.cpp).
const char* const kAmbientNames[kAmbientCount] = { "breath1", "breath3", "breath6", "breath8", "moan1", "moan2", "moan3", "moan4", "moan5", "moan6",
	"moan7", "moan8", "scream1", "scream3", "scream5", "scream7", "raven1", "raven3", nullptr /* Diablo's laugh */ };

// Fades a sound in over its first inMs and out over its last outMs: for a recording made to loop
// (it starts and ends in mid-wave) that is played once.
void FadeEnds(int16_t* samples, uint32_t count, uint32_t rate, uint32_t inMs, uint32_t outMs) {
	uint32_t in  = static_cast<uint32_t>(static_cast<uint64_t>(rate) * inMs / 1000);
	uint32_t out = static_cast<uint32_t>(static_cast<uint64_t>(rate) * outMs / 1000);
	in           = in > count / 2 ? count / 2 : in;
	out          = out > count / 2 ? count / 2 : out;
	for (uint32_t i = 0; i < in; ++i) {
		samples[i] = static_cast<int16_t>(static_cast<int64_t>(samples[i]) * i / in);
	}
	for (uint32_t i = 0; i < out; ++i) {
		samples[count - 1 - i] = static_cast<int16_t>(static_cast<int64_t>(samples[count - 1 - i]) * i / out);
	}
}

bool LoadGameClip(void* storage, const char* name, const SoundProcess& process, Clip* clip, uint32_t fadeInMs = 0, uint32_t fadeOutMs = 0) {
	uint32_t count   = 0;
	int16_t* samples = LoadGameSound(storage, name, process, &count);
	if (samples == nullptr) {
		Log("sounds: could not read %s from the game data", name);
		return false;
	}
	if (fadeInMs != 0 || fadeOutMs != 0) {
		FadeEnds(samples, count, process.outRate, fadeInMs, fadeOutMs);
	}
	clip->samples = samples; // kept for the life of the process
	clip->count   = count;
	clip->rate    = process.outRate;
	return true;
}

// The game folder: next to the running executable, or else five folders above this DLL
// (<game>\mods\<mod>\d2rloader\plugins\<dll>).
void* OpenStorage() {
	char  error[700] = {};
	void* storage    = OpenGameStorage(nullptr, error, sizeof(error));
	if (storage != nullptr) {
		return storage;
	}
	wchar_t     path[1024] = {};
	const DWORD length     = GetModuleFileNameW(g_module, path, 1000);
	bool        found      = length > 0 && length < 1000;
	for (int cut = 0; found && cut < 5; ++cut) {
		wchar_t* slash = wcsrchr(path, L'\\');
		found          = slash != nullptr;
		if (found) {
			*slash = L'\0';
		}
	}
	char second[700] = {};
	storage          = found ? OpenGameStorage(path, second, sizeof(second)) : nullptr;
	if (storage == nullptr) {
		Log("sounds: %s%s%s - no thunder or whispers", error, found ? "; " : "", second);
	}
	return storage;
}

int LoadSounds() {
	int            loaded  = 0;
	const uint64_t started = GetTickCount64();
	if (void* storage = OpenStorage()) {
		// the same processing the sounds always had: thunder gets a bass lift and is pushed into a limiter
		const SoundProcess thunder { 32000, 4.0f, 90.0f, 12.0f, 0.95f, 3.0f, 120.0f, 0.0f };
		const SoundProcess ambient { 22050, 0.0f, 0.0f, 3.0f, 0.95f, 5.0f, 50.0f, 9.0f };
		char               name[160];
		for (int i = 0; i < kThunderCount; ++i) {
			snprintf(name, sizeof(name), "data:data\\hd\\global\\sfx\\ambient\\event\\thunder_norm_%d_hd.flac", i + 1);
			loaded += LoadGameClip(storage, name, thunder, &g_thunder[i]) ? 1 : 0;
		}
		for (int i = 0; i < kAmbientCount; ++i) {
			if (kAmbientNames[i] != nullptr) {
				snprintf(name, sizeof(name), "data:data\\global\\sfx\\ambient\\event\\%s.flac", kAmbientNames[i]);
			} else {
				snprintf(name, sizeof(name), "data:data\\global\\sfx\\monster\\diablo\\laugh1.flac");
			}
			loaded += LoadGameClip(storage, name, ambient, &g_ambient[i]) ? 1 : 0;
		}
		// the gusts are quiet recordings: lifted, and kept whole
		const SoundProcess wind { 22050, 0.0f, 0.0f, 6.0f, 0.95f, 5.0f, 50.0f, 0.0f };
		for (int i = 0; i < kWindCount; ++i) {
			snprintf(name, sizeof(name), "data:data\\global\\sfx\\ambient\\event\\wind%d.flac", i + 1);
			loaded += LoadGameClip(storage, name, wind, &g_wind[i]) ? 1 : 0;
		}
		// the ash storm's fire: the roar is a loud recording and is brought down, the embers are a
		// quiet one and are lifted; both at the mixer's own rate, for the crackle
		const char* const  fireNames[kFireCount] = { "tristram_fire_rumble_hd", "bonfire_embers_loop_hd" };
		const SoundProcess fire[kFireCount]      = { { 32000, 0.0f, 0.0f, -6.0f, 0.95f, 5.0f, 50.0f, 0.0f }, { 32000, 0.0f, 0.0f, 12.0f, 0.95f, 5.0f, 50.0f, 0.0f } };
		for (int i = 0; i < kFireCount; ++i) {
			snprintf(name, sizeof(name), "data:data\\hd\\global\\sfx\\ambient\\event\\%s.flac", fireNames[i]);
			loaded += LoadGameClip(storage, name, fire[i], &g_fire[i]) ? 1 : 0;
		}
		const SoundProcess steam { 32000, 0.0f, 0.0f, 6.0f, 0.95f, 5.0f, 50.0f, 0.0f };
		const SoundProcess rockfall { 32000, 0.0f, 0.0f, 3.0f, 0.95f, 5.0f, 50.0f, 0.0f };
		for (int i = 0; i < kBurstCount; ++i) {
			snprintf(name, sizeof(name), i < 3 ? "data:data\\hd\\global\\sfx\\ambient\\event\\event_hell_steam_%d_hd.flac" : "data:data\\hd\\global\\sfx\\ambient\\event\\frontend_act4_rockfall%d_hd.flac",
				i % 3 + 1);
			// the steam is a loop, played once here: it swells in and trails off
			loaded += LoadGameClip(storage, name, i < 3 ? steam : rockfall, &g_burst[i], i < 3 ? 150 : 0, i < 3 ? 900 : 0) ? 1 : 0;
		}
		CloseGameStorage(storage);
		Log("sounds: %d of %d read from the game data in %llu ms", loaded, kThunderCount + kAmbientCount + kWindCount + kFireCount + kBurstCount,
			static_cast<unsigned long long>(GetTickCount64() - started));
	}
	return loaded;
}

DWORD WINAPI SoundLoaderThread(void*) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	LoadSounds();
	g_soundsReady.store(true, std::memory_order_release);
	return 0;
}

constexpr uint32_t kMixRate    = 32000;
constexpr uint32_t kMixBlock   = 1024; // 32 ms per buffer
constexpr int      kMixBuffers = 3;
constexpr int      kMaxVoices  = 12;
constexpr uint32_t kIdleBlocks = kMixRate * 10 / kMixBlock; // close the device after 10 s of silence
constexpr float    kBedStep    = 2.0f * kMixBlock / kMixRate; // most a bed's loudness moves in one buffer

struct PlayRequest {
	const Clip* clip;
	float       gain;
};
SRWLOCK            g_playLock = SRWLOCK_INIT;
PlayRequest        g_requests[16];
int                g_requestCount = 0;
HANDLE             g_mixEvent     = nullptr; // auto-reset: a request came in, or the device finished a buffer
HANDLE             g_mixThread    = nullptr;
std::atomic<bool>  g_mixStop { false };

// A bed is a sound that goes on for as long as a storm wants it: the wind, the ash storm's fire.
// It is kept going as overlapping gusts of its clips, each faded in and out, and all of them as
// loud as the storm wants the bed at that moment (the mixer follows the gain, a little a buffer).
enum : int { kBedWind = 0, kBedFire = 1, kBeds = 2 };
constexpr int8_t kNoBed = -1;
struct Bed {
	const Clip*        clips;
	int                count;
	int                nextMin; // the next gust starts when this share (percent) of the last has played...
	int                nextMax; // ...at the latest this: the lower, the more of them overlap
	std::atomic<float> gain;
	std::atomic<int>   lowPass; // Hz: only what the bed has below this is heard, 0 = all of it
};
Bed g_beds[kBeds] = {
	{ g_wind, kWindCount, 55, 80, 0.0f, 0 },
	{ g_fire, kFireCount, 35, 55, 0.0f, 0 }, // two or three at a time, so roar and crackle are mostly both there
};

// What a bed sounds like below a frequency: a fourth-order low-pass (two second-order sections,
// Butterworth), 24 dB an octave above the cutoff. The game's wind has nearly all its strength
// below 300 Hz and a thin whistle above; cut at 100 to 150 Hz it is a deep roar and nothing else
// (about 2 to 4 dB quieter, which the ash storm's wind volume makes up for).
struct LowPass {
	int   hz = 0; // what it is set for, 0 = off (everything passes)
	float b0[2] {}, b1[2] {}, a1[2] {}, a2[2] {};
	float z1[2] {}, z2[2] {};

	void Set(int wanted) {
		wanted = wanted <= 0 ? 0 : wanted < 30 ? 30 : wanted > static_cast<int>(kMixRate) / 4 ? static_cast<int>(kMixRate) / 4 : wanted;
		if (wanted == hz) {
			return;
		}
		if (hz == 0) {
			Clear(); // it starts from silence
		}
		hz = wanted;
		const float q[2] = { 0.54119610f, 1.30656296f };
		const float w    = 2.0f * 3.14159265f * static_cast<float>(hz) / static_cast<float>(kMixRate);
		for (int stage = 0; stage < 2 && hz != 0; ++stage) {
			const float alpha = std::sin(w) / (2.0f * q[stage]);
			const float a0    = 1.0f + alpha;
			b0[stage]         = (1.0f - std::cos(w)) / 2.0f / a0;
			b1[stage]         = (1.0f - std::cos(w)) / a0;
			a1[stage]         = -2.0f * std::cos(w) / a0;
			a2[stage]         = (1.0f - alpha) / a0;
		}
	}
	void Clear() {
		z1[0] = z1[1] = z2[0] = z2[1] = 0.0f;
	}
	void Run(float* samples, uint32_t count) {
		if (hz == 0) {
			return;
		}
		for (int stage = 0; stage < 2; ++stage) {
			float s1 = z1[stage], s2 = z2[stage];
			for (uint32_t i = 0; i < count; ++i) {
				const float x = samples[i];
				const float y = b0[stage] * x + s1;
				s1            = b1[stage] * x - a1[stage] * y + s2;
				s2            = b0[stage] * x - a2[stage] * y;
				samples[i]    = y;
			}
			// a filter left with a vanishing remainder would crawl through denormal numbers
			z1[stage] = std::fabs(s1) < 1e-20f ? 0.0f : s1;
			z2[stage] = std::fabs(s2) < 1e-20f ? 0.0f : s2;
		}
	}
};

struct Voice {
	const Clip* clip;
	uint64_t    position; // in clip samples, 32.32 fixed point
	uint64_t    step;
	float       gain;
	int8_t      bed; // kNoBed: a sound on its own. Otherwise a gust of that bed: faded at both ends, and as loud as the bed is at that moment
};

// Keeps a bed going as overlapping gusts: a new one starts before the last has died away.
// untilNext counts output samples down to the next gust.
void BedGusts(Voice* voices, int& voiceCount, int bed, bool wanted, bool audible, int32_t& untilNext) {
	if (!wanted) {
		untilNext = 0;
		if (!audible) { // faded right out: gusts still running are dropped
			for (int v = 0; v < voiceCount;) {
				if (voices[v].bed == bed) {
					voices[v] = voices[--voiceCount];
				} else {
					++v;
				}
			}
		}
		return;
	}
	untilNext -= static_cast<int32_t>(kMixBlock);
	if (untilNext > 0 || voiceCount >= kMaxVoices) {
		return;
	}
	const Bed&  from = g_beds[bed];
	const Clip* clip = nullptr;
	for (int attempt = 0; attempt < from.count * 2 && clip == nullptr; ++attempt) {
		const Clip& candidate = from.clips[g_mixRng.Next() % static_cast<uint32_t>(from.count)];
		if (candidate.samples != nullptr && candidate.count > 0) {
			clip = &candidate;
		}
	}
	if (clip == nullptr) {
		untilNext = static_cast<int32_t>(kMixRate); // none of its sounds was read: look again in a second
		return;
	}
	voices[voiceCount++] = Voice { clip, 0, (static_cast<uint64_t>(clip->rate) << 32) / kMixRate, 1.0f, static_cast<int8_t>(bed) };
	const uint64_t length = static_cast<uint64_t>(clip->count) * kMixRate / clip->rate; // in output samples
	untilNext             = static_cast<int32_t>(length * static_cast<uint64_t>(g_mixRng.Range(from.nextMin, from.nextMax)) / 100);
}

// One buffer of sound: every voice added up, finished voices dropped. bedFrom and bedTo are
// each bed's loudness at the start and at the end of the buffer; tone (when given) is each bed's
// low-pass: a bed's gusts are added up on their own and go through it before they join the rest.
void MixBlock(Voice* voices, int& voiceCount, const float* bedFrom, const float* bedTo, int16_t* out, LowPass* tone = nullptr) {
	float mix[kMixBlock] = {};
	float bedMix[kBeds][kMixBlock];
	bool  bedUsed[kBeds] {};
	for (int v = 0; v < voiceCount;) {
		Voice&         voice   = voices[v];
		const int16_t* samples = voice.clip->samples;
		const uint32_t count   = voice.clip->count;
		uint32_t       i       = 0;
		if (voice.bed != kNoBed) {
			float* const into = bedMix[voice.bed];
			if (!bedUsed[voice.bed]) {
				bedUsed[voice.bed] = true;
				memset(into, 0, sizeof(bedMix[0]));
			}
			const float windFrom = bedFrom[voice.bed];
			const float windTo   = bedTo[voice.bed];
			uint32_t    fade = voice.clip->rate * 2 / 5; // 0.4 s in and out
			if (fade > count / 3) {
				fade = count / 3;
			}
			if (fade == 0) {
				fade = 1;
			}
			const bool  soft     = windFrom > 1.0f || windTo > 1.0f;
			const float windStep = (windTo - windFrom) / static_cast<float>(kMixBlock);
			const float perFade  = 1.0f / static_cast<float>(fade);
			for (; i < kMixBlock; ++i) {
				const uint64_t at = voice.position >> 32;
				if (at >= count) {
					break;
				}
				const float frac     = static_cast<float>(voice.position & 0xffffffffULL) * (1.0f / 4294967296.0f);
				const float a        = samples[at];
				const float next     = at + 1 < count ? samples[at + 1] : a;
				const float envelope = at < fade ? static_cast<float>(at) * perFade : count - at < fade ? static_cast<float>(count - at) * perFade : 1.0f;
				const float value = (a + (next - a) * frac) * envelope * (windFrom + windStep * static_cast<float>(i)) * (1.0f / 32768.0f);
				into[i] += soft ? std::tanh(value) : value;
				voice.position += voice.step;
			}
		} else {
			const bool  soft  = voice.gain > 1.0f; // above full strength the peaks are rounded off, not clipped
			const float scale = voice.gain / 32768.0f;
			for (; i < kMixBlock; ++i) {
				const uint64_t at = voice.position >> 32;
				if (at >= count) {
					break;
				}
				const float frac  = static_cast<float>(voice.position & 0xffffffffULL) * (1.0f / 4294967296.0f);
				const float a     = samples[at];
				const float next  = at + 1 < count ? samples[at + 1] : a;
				const float value = (a + (next - a) * frac) * scale;
				mix[i] += soft ? std::tanh(value) : value;
				voice.position += voice.step;
			}
		}
		if (i < kMixBlock) {
			voices[v] = voices[--voiceCount]; // finished
		} else {
			++v;
		}
	}
	for (int bed = 0; bed < kBeds; ++bed) {
		if (!bedUsed[bed]) {
			if (tone != nullptr) {
				tone[bed].Clear(); // nothing of it playing: the next gust starts clean
			}
			continue;
		}
		if (tone != nullptr) {
			tone[bed].Run(bedMix[bed], kMixBlock);
		}
		for (uint32_t i = 0; i < kMixBlock; ++i) {
			mix[i] += bedMix[bed][i];
		}
	}
	for (uint32_t i = 0; i < kMixBlock; ++i) {
		const float value = mix[i] > 1.0f ? 1.0f : mix[i] < -1.0f ? -1.0f : mix[i];
		out[i]            = static_cast<int16_t>(value * 32767.0f);
	}
}

DWORD WINAPI MixerThread(void*) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // little work, but it must not starve
	HWAVEOUT       device = nullptr;
	WAVEHDR        headers[kMixBuffers] {};
	static int16_t buffers[kMixBuffers][kMixBlock];
	Voice          voices[kMaxVoices];
	int            voiceCount   = 0;
	uint32_t       idle         = 0;
	bool           failedLogged = false;
	float          bedNow[kBeds] {};    // each bed's loudness at the end of the last buffer
	int32_t        untilGust[kBeds] {};
	LowPass        tone[kBeds];
	const auto     bedWanted    = [] {
		for (const Bed& bed : g_beds) {
			if (bed.gain.load(std::memory_order_relaxed) > 0.001f) {
				return true;
			}
		}
		return false;
	};

	while (!g_mixStop.load(std::memory_order_relaxed)) {
		WaitForSingleObject(g_mixEvent, INFINITE);
		if (g_mixStop.load(std::memory_order_relaxed)) {
			break;
		}
		AcquireSRWLockExclusive(&g_playLock);
		for (int i = 0; i < g_requestCount && voiceCount < kMaxVoices; ++i) {
			const Clip* clip = g_requests[i].clip;
			voices[voiceCount++] = Voice { clip, 0, (static_cast<uint64_t>(clip->rate) << 32) / kMixRate, g_requests[i].gain, kNoBed };
		}
		g_requestCount = 0;
		ReleaseSRWLockExclusive(&g_playLock);

		if (device == nullptr) {
			if (voiceCount == 0 && !bedWanted()) {
				continue;
			}
			WAVEFORMATEX format {};
			format.wFormatTag      = WAVE_FORMAT_PCM;
			format.nChannels       = 1;
			format.nSamplesPerSec  = kMixRate;
			format.wBitsPerSample  = 16;
			format.nBlockAlign     = 2;
			format.nAvgBytesPerSec = kMixRate * 2;
			if (waveOutOpen(&device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(g_mixEvent), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
				device     = nullptr;
				voiceCount = 0;
				if (!failedLogged) {
					failedLogged = true;
					Log("sounds: could not open the audio device");
				}
				continue;
			}
			for (int b = 0; b < kMixBuffers; ++b) {
				headers[b]                = WAVEHDR {};
				headers[b].lpData         = reinterpret_cast<LPSTR>(buffers[b]);
				headers[b].dwBufferLength = sizeof(buffers[b]);
				waveOutPrepareHeader(device, &headers[b], sizeof(WAVEHDR));
				headers[b].dwFlags |= WHDR_DONE; // free to fill
			}
			idle = 0;
			for (int bed = 0; bed < kBeds; ++bed) {
				bedNow[bed]    = 0.0f;
				untilGust[bed] = 0;
			}
		}

		// refill every buffer the device has finished with
		for (int b = 0; b < kMixBuffers; ++b) {
			if ((headers[b].dwFlags & WHDR_DONE) == 0) {
				continue;
			}
			float bedTo[kBeds];
			for (int bed = 0; bed < kBeds; ++bed) {
				const float target = g_beds[bed].gain.load(std::memory_order_relaxed);
				const float move   = target - bedNow[bed];
				bedTo[bed]         = bedNow[bed] + (move > kBedStep ? kBedStep : move < -kBedStep ? -kBedStep : move);
				BedGusts(voices, voiceCount, bed, target > 0.001f, bedTo[bed] > 0.001f, untilGust[bed]);
				tone[bed].Set(g_beds[bed].lowPass.load(std::memory_order_relaxed));
			}
			MixBlock(voices, voiceCount, bedNow, bedTo, buffers[b], tone);
			memcpy(bedNow, bedTo, sizeof(bedNow));
			idle = voiceCount > 0 ? 0 : idle + 1;
			headers[b].dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
			waveOutWrite(device, &headers[b], sizeof(WAVEHDR));
		}

		if (idle >= kIdleBlocks) {
			waveOutReset(device);
			for (int b = 0; b < kMixBuffers; ++b) {
				waveOutUnprepareHeader(device, &headers[b], sizeof(WAVEHDR));
			}
			waveOutClose(device);
			device = nullptr;
		}
	}
	if (device != nullptr) {
		waveOutReset(device);
		for (int b = 0; b < kMixBuffers; ++b) {
			waveOutUnprepareHeader(device, &headers[b], sizeof(WAVEHDR));
		}
		waveOutClose(device);
	}
	return 0;
}

void StartMixer() {
	g_mixEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_mixThread = g_mixEvent != nullptr ? CreateThread(nullptr, 0, MixerThread, nullptr, 0, nullptr) : nullptr;
	if (g_mixThread == nullptr) {
		Log("sounds: could not start the mixer thread");
	}
}

void StopMixer() {
	for (Bed& bed : g_beds) {
		bed.gain = 0.0f;
	}
	if (g_mixThread != nullptr) {
		g_mixStop = true;
		SetEvent(g_mixEvent);
		WaitForSingleObject(g_mixThread, 2000);
		CloseHandle(g_mixThread);
		g_mixThread = nullptr;
	}
}

// Called from the game's threads: only queues the sound.
void PlayClip(const Clip& clip, float gain) {
	if (gain <= 0.001f || g_mixThread == nullptr || !g_soundsReady.load(std::memory_order_acquire) || clip.samples == nullptr || clip.count == 0) {
		return;
	}
	AcquireSRWLockExclusive(&g_playLock);
	if (g_requestCount < 16) {
		g_requests[g_requestCount++] = PlayRequest { &clip, gain };
	}
	ReleaseSRWLockExclusive(&g_playLock);
	SetEvent(g_mixEvent);
}

void PlayThunder(float gain) {
	PlayClip(g_thunder[ClientRand() % kThunderCount], gain * static_cast<float>(g_config.volume) / 100.0f);
}

// Called from the game's threads: how loud a bed should be from now on (0 = none). The mixer
// is only woken when a bed starts; after that the audio device keeps it running.
void SetBed(int bed, float gain) {
	if (g_mixThread == nullptr || !g_soundsReady.load(std::memory_order_acquire) || gain < 0.001f) {
		gain = 0.0f;
	}
	static float sent[kBeds] {}; // this runs every frame, and most frames nothing has changed
	if (gain == sent[bed]) {
		return;
	}
	const bool start = gain > 0.0f && sent[bed] <= 0.0f;
	sent[bed]        = gain;
	g_beds[bed].gain.store(gain, std::memory_order_relaxed);
	if (start) {
		SetEvent(g_mixEvent);
	}
}

float FlashAlpha(uint64_t elapsed) {
	const float peak = 0.45f;
	if (elapsed < 70) {
		return peak;
	}
	if (elapsed < 140) {
		return peak * 0.12f;
	}
	if (elapsed < 240) {
		return peak * 0.8f;
	}
	if (elapsed < 650) {
		return peak * 0.8f * (1.0f - static_cast<float>(elapsed - 240) / 410.0f);
	}
	return 0.0f;
}

// ---- sky flash ------------------------------------------------------------------
// The renderer object (pointer at 0x34f6480) keeps the time of day as a stage 0-5
// (dawn 1, dawn 2, day, dusk 1, dusk 2, night) at +0x118f0 and a 0-1 blend toward the
// next stage at +0x118f4; the byte at +0x11909 is the game's own "override time of day"
// switch, which stops the server clock from changing them. The mod makes every stage
// night except "day" in stormy areas, which is the lit-by-lightning look. So:
//   idle  = stage 5 (night), blend 0
//   flash = stage 1 (dawn 2 = night) blended by the flash intensity toward stage 2 (day)
constexpr uint32_t kRendererPointer = 0x34f6480;
constexpr uint32_t kStageOffset     = 0x118f0;
constexpr uint32_t kBlendOffset     = 0x118f4;
constexpr uint32_t kOverrideOffset  = 0x11909;

bool g_skyReady   = false;
bool g_skyHolding = false;

struct FlashPeak {
	uint32_t startMs;
	float    strength;
};
FlashPeak g_peaks[12];
int       g_peakCount  = 0;
uint64_t  g_skyStart   = 0;

template <typename T>
bool SafeRead(uintptr_t address, T* out);

uintptr_t g_skyRenderer  = 0; // checked renderer object
uint64_t  g_skyChecked   = 0;
int32_t   g_skyLastStage = -1;
float     g_skyLastBlend = -1.0f;
uint8_t   g_skyLastOverride = 0xff;

// Writes only when something changes; the renderer pointer is re-checked once a second
// and what was written is re-asserted then, in case the game changed it meanwhile.
void SkyWrite(int32_t stage, float blend, uint8_t overrideOn) {
	const uint64_t now = GetTickCount64();
	if (g_skyRenderer == 0 || now - g_skyChecked >= 1000) {
		uintptr_t renderer = 0;
		int32_t   current  = 0;
		g_skyChecked       = now;
		if (!SafeRead(g_base + kRendererPointer, &renderer) || renderer == 0 || !SafeRead(renderer + kStageOffset, &current) || current < 0 || current > 5) {
			g_skyRenderer = 0; // not there, or not the layout this was written for
			return;
		}
		g_skyRenderer     = renderer;
		g_skyLastOverride = 0xff; // force a rewrite
	}
	if (stage == g_skyLastStage && blend == g_skyLastBlend && overrideOn == g_skyLastOverride) {
		return;
	}
	*reinterpret_cast<uint8_t*>(g_skyRenderer + kOverrideOffset) = overrideOn;
	if (overrideOn != 0) {
		*reinterpret_cast<int32_t*>(g_skyRenderer + kStageOffset) = stage;
		*reinterpret_cast<float*>(g_skyRenderer + kBlendOffset)   = blend;
	}
	g_skyLastStage    = stage;
	g_skyLastBlend    = blend;
	g_skyLastOverride = overrideOn;
}

void StartSkyFlash(uint64_t now) {
	g_skyStart  = now;
	g_peakCount = ClientRange(g_config.hitsMin, g_config.hitsMax);
	uint32_t at = 0;
	for (int i = 0; i < g_peakCount; ++i) {
		g_peaks[i].startMs  = at;
		g_peaks[i].strength = i == 0 ? 1.0f : static_cast<float>(ClientRange(55, 100)) / 100.0f;
		at += static_cast<uint32_t>(ClientRange(90, 300));
	}
}

// 0-1: each peak snaps on, holds 50 ms, then fades over 230 ms
float SkyIntensity(uint64_t now) {
	if (g_peakCount == 0) {
		return 0.0f;
	}
	const uint32_t elapsed = static_cast<uint32_t>(now - g_skyStart);
	float          best    = 0.0f;
	bool           pending = false;
	for (int i = 0; i < g_peakCount; ++i) {
		if (elapsed < g_peaks[i].startMs) {
			pending = true;
			continue;
		}
		const uint32_t since = elapsed - g_peaks[i].startMs;
		float          value = 0.0f;
		if (since < 50) {
			value = g_peaks[i].strength;
		} else if (since < 280) {
			value = g_peaks[i].strength * (1.0f - static_cast<float>(since - 50) / 230.0f);
		}
		if (value > best) {
			best = value;
		}
	}
	if (best <= 0.0f && !pending) {
		g_peakCount = 0;
	}
	return best;
}

// ---- storms over terror zones ----
// The scene's lighting is made each frame by the game's profile blend (0xf58c40): every profile
// added moves the scene toward itself by its weight, so the ones added before it count for
// (1 - weight). The time of day goes in first, then any lighting event: a terror zone's look,
// Tainted Sun, Diablo's arrival. At full weight an event hides the time of day, and with it the
// lightning's flash and the sandstorm, which work through the time of day. So while a flash or a
// storm shows, events go in with their weight lowered by as much: the storm shows through a
// terror zone the way it does outside one, and the zone's look comes back as it passes.
using BlendFn = void (*)(void* scene, void* profile, float weight, uint8_t isEvent);
constexpr uint32_t kBlendRva          = 0xf58c40;
const uint8_t      kBlendEntry[16]    = { 0xf3, 0x0f, 0x11, 0x54, 0x24, 0x18, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57 };
BlendFn            g_originalBlend    = nullptr;
std::atomic<float> g_eventYield { 0.0f }; // 0-1: how much of an event's weight gives way to the storm now

void BlendHook(void* scene, void* profile, float weight, uint8_t isEvent) {
	if (isEvent != 0) {
		const float yield = g_eventYield.load(std::memory_order_relaxed);
		if (yield > 0.0f) {
			weight *= 1.0f - (yield < 1.0f ? yield : 1.0f);
		}
	}
	g_originalBlend(scene, profile, weight, isEvent);
}

// sand: 0-1, how much of the sandstorm look to show (the same blend the lightning uses).
void UpdateSky(uint64_t now, bool inGame, float sand) {
	if (!g_skyReady) {
		return;
	}
	// The game is held on night exactly when the mod turned some area's "day" lighting into
	// something else (the lightning look, the sandstorm look): the game's own clock would show
	// that as daytime. An older mod does not say what it installed; then the hold follows the
	// plugin's own sky flash setting, as it always did.
	const bool known   = g_modKnown.load(std::memory_order_relaxed) != 0;
	const bool flashOn = g_config.skyFlash != 0 && (!known || g_modSkyFlash.load(std::memory_order_relaxed) != 0);
	const bool hold    = inGame
		&& (known ? g_modSkyFlash.load(std::memory_order_relaxed) != 0 || g_modSandstorm.load(std::memory_order_relaxed) != 0
					|| g_modAshstorm.load(std::memory_order_relaxed) != 0
				  : g_config.skyFlash != 0);
	if (!hold) {
		if (g_skyHolding) {
			SkyWrite(5, 0.0f, 0); // give the clock back to the game
			g_skyHolding = false;
		}
		g_peakCount = 0;
		g_eventYield.store(0.0f, std::memory_order_relaxed);
		return;
	}
	g_skyHolding = true;
	float intensity = 0.0f;
	if (flashOn) {
		intensity = SkyIntensity(now) * static_cast<float>(g_config.skyStrength) / 100.0f;
	} else {
		g_peakCount = 0;
	}
	if (sand > intensity) {
		intensity = sand;
	}
	g_eventYield.store(intensity > 0.001f ? (intensity < 1.0f ? intensity : 1.0f) : 0.0f, std::memory_order_relaxed);
	if (intensity > 0.001f) {
		SkyWrite(1, intensity > 1.0f ? 1.0f : intensity, 1);
	} else {
		SkyWrite(5, 0.0f, 1);
	}
}

// A read that fails instead of crashing when the game has moved or freed something. It is a
// system call (a wineserver round trip under Proton), so it is kept out of per-frame paths.
bool SafeReadBytes(uintptr_t address, void* out, size_t size) {
	SIZE_T done = 0;
	return address > 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &done) && done == size;
}

template <typename T>
bool SafeRead(uintptr_t address, T* out) {
	return SafeReadBytes(address, out, sizeof(T));
}

// ---- storms ------------------------------------------------------------------------
// A storm is weather the plugin makes itself. The mod turns the unused "day" lighting of some
// areas into the storm's look (for the Act 2 desert: dense sand-coloured fog, hard wind), and a
// storm is that look faded in and out through the time-of-day blend the lightning uses, with a
// field of the game's own effects blowing past and gusts of wind on top. Each kind of storm
// comes and goes on its own clock, whether or not the player is out in one.
struct StormState {
	bool              active = false; // a storm is blowing
	uint64_t          change = 0;     // tick count of the next start or end, 0 = not scheduled
	float             level  = 0.0f;  // 0-1: how much of the storm the scene shows now
	float             wind   = 0.0f;  // 0-1: how much of its wind is heard now
	int               logs   = 0;
	std::atomic<bool> force { false }; // started or stopped from the console
	std::atomic<int>  shown { 0 };     // percent, for "horror status"
};

struct StormKind {
	const char*        name;        // in the log
	uint8_t            fall;        // what "falls" in its levels (LevelInfo)
	const StormEffect* effects;     // what its field can be made of; [0] is "none"
	int                effectCount;
	int                layers;      // how many effects its field can have at once (its layer settings)
	StormConfig*       config;
	std::atomic<int>*  installed;   // the mod installed its look
	bool*              failed;      // per effect: the game would not create it, not tried again
	const int*         fireVolume;  // its fire's volume setting; nullptr: this storm has no fire
	StormState         state;
};

enum : int { kStormSand = 0, kStormAsh = 1, kStormKinds = 2 };
StormKind g_storms[kStormKinds] = {
	{ "sandstorm", kFallSand, kSandEffects, kSandEffectCount, 3, &g_config.sand, &g_modSandstorm, g_sandFailed, nullptr, {} },
	{ "ash storm", kFallAsh, kAshEffects, kAshEffectCount, kMaxLayers, &g_config.ash, &g_modAshstorm, g_ashFailed, &g_config.fireVolume, {} },
};
uint64_t g_stormLast = 0; // tick count of the previous update

StormState& Sand() { return g_storms[kStormSand].state; }

bool StormAllowed(const StormKind& kind) {
	return g_skyReady && kind.config->enabled != 0 && kind.installed->load(std::memory_order_relaxed) != 0;
}

void Approach(float& value, float target, float step) {
	if (value < target) {
		value = value + step < target ? value + step : target;
	} else {
		value = value - step > target ? value - step : target;
	}
}

void UpdateStorms(uint64_t now, int level) {
	float seconds = g_stormLast != 0 && now > g_stormLast ? static_cast<float>(now - g_stormLast) / 1000.0f : 0.0f;
	if (seconds > 0.25f) {
		seconds = 0.25f; // a long hitch does not jump the fade
	}
	g_stormLast = now;

	float bedGain[kBeds] {};
	int   windLowPass = -1; // of the storm whose wind is heard, -1 = no wind: left as it is while the last of it fades
	for (StormKind& kind : g_storms) {
		StormState&        state   = kind.state;
		const StormConfig& config  = *kind.config;
		const bool         allowed = level > 0 && StormAllowed(kind);
		const bool         toggle  = state.force.load(std::memory_order_relaxed) && state.force.exchange(false);
		if (!allowed) {
			state.active = false;
			state.change = 0;
			state.level  = 0.0f;
			state.wind   = 0.0f;
			state.shown  = 0;
			continue;
		}

		const auto calm   = [&] { return static_cast<uint64_t>(ClientRange(config.calmMin, config.calmMax)) * 1000; };
		const auto length = [&] { return static_cast<uint64_t>(ClientRange(config.lengthMin, config.lengthMax)) * 1000; };
		if (toggle) {
			state.active = !state.active;
			state.change = now + (state.active ? length() : calm());
			Log("%s: %s from the console (level %d)", kind.name, state.active ? "started" : "stopped", level);
		} else if (config.always != 0) {
			state.active = true;
			state.change = 0;
		} else if (state.change == 0) {
			state.active = false;
			state.change = now + calm();
		} else if (now >= state.change) {
			state.active = !state.active;
			state.change = now + (state.active ? length() : calm());
			if (++state.logs <= 6) {
				Log("%s: %s (level %d), next change in %llu s", kind.name, state.active ? "rolling in" : "dying down", level,
					static_cast<unsigned long long>((state.change - now) / 1000));
			}
		}

		const bool  inStorm   = LevelFall(level) == kind.fall;
		const bool  sheltered = !inStorm && LevelUnder(level) == kind.fall;
		const float step      = seconds / static_cast<float>(config.fade);
		if (inStorm) {
			Approach(state.level, state.active ? 1.0f : 0.0f, step);
		} else if (sheltered && IsTown(level)) {
			Approach(state.level, 0.0f, step); // its town (Lut Gholein has the desert's lighting): the storm thins out, it is not cut off
		} else {
			state.level = 0.0f; // any other area's "day" lighting is not this storm
		}
		const float wind = !state.active ? 0.0f : inStorm ? 1.0f : sheltered && config.indoorWind != 0 ? 0.3f : 0.0f;
		Approach(state.wind, wind, step);
		// what is heard of it: the wind, and under the ash storm the fire, muffled together
		const float gain   = state.wind * static_cast<float>(config.windVolume) / 100.0f;
		const float fire   = kind.fireVolume != nullptr ? state.wind * static_cast<float>(*kind.fireVolume) / 100.0f : 0.0f;
		if (gain > bedGain[kBedWind]) {
			bedGain[kBedWind] = gain;
			windLowPass       = config.windLowPass;
		}
		bedGain[kBedFire]  = fire > bedGain[kBedFire] ? fire : bedGain[kBedFire];
		state.shown        = static_cast<int>(state.level * 100.0f + 0.5f);
	}
	for (int bed = 0; bed < kBeds; ++bed) {
		SetBed(bed, bedGain[bed]);
	}
	if (windLowPass >= 0 && windLowPass != g_beds[kBedWind].lowPass.load(std::memory_order_relaxed)) {
		g_beds[kBedWind].lowPass.store(windLowPass, std::memory_order_relaxed);
	}
}

// The ash storm's steam and rockfalls: one of them every so often while the player is out in a
// storm that shows at half strength or more.
uint64_t         g_burstNext = 0; // tick count of the next one, 0 = not scheduled
std::atomic<int> g_totalBursts { 0 };

void UpdateBursts(uint64_t now, int level) {
	const bool on = g_config.burstVolume > 0 && level > 0 && LevelFall(level) == kFallAsh && g_storms[kStormAsh].state.level >= 0.5f;
	if (!on) {
		g_burstNext = 0;
		return;
	}
	if (g_burstNext != 0 && now < g_burstNext) {
		return;
	}
	const bool due = g_burstNext != 0;
	g_burstNext    = now + static_cast<uint64_t>(ClientRange(g_config.burstMinInterval, g_config.burstMaxInterval)) * 1000;
	if (due) {
		++g_totalBursts;
		PlayClip(g_burst[ClientRand() % kBurstCount], static_cast<float>(g_config.burstVolume) / 100.0f * static_cast<float>(ClientRange(50, 100)) / 100.0f);
	}
}

// How much of a storm look the scene shows, 0-1 (one kind's at most: each has its own areas).
float StormBlend() {
	float blend = 0.0f;
	for (const StormKind& kind : g_storms) {
		const float own = kind.state.level * static_cast<float>(kind.config->strength) / 100.0f;
		blend           = own > blend ? own : blend;
	}
	return blend;
}

// The blowing field for the weather driver: which kind of storm this level is out in (*which,
// nullptr = none) and how many of its configured effects should be up right now. -1 = this is
// not a place for a field (or it is switched off). The effects come in one after another as the
// storm builds and go the same way: the first early, the last near its height.
int StormLayers(int level, StormKind** which) {
	*which = nullptr;
	if (level <= 0) {
		return -1;
	}
	for (StormKind& kind : g_storms) {
		const StormConfig& config = *kind.config;
		if (LevelFall(level) != kind.fall || !StormAllowed(kind) || config.dust <= 0) {
			continue;
		}
		int configured = 0;
		for (const int effect : config.layer) {
			configured += effect > 0 && effect < kind.effectCount ? 1 : 0;
		}
		if (configured == 0) {
			return -1;
		}
		int up = 0;
		for (int i = 0; i < configured; ++i) {
			const float threshold = 0.1f + (configured > 1 ? 0.6f * static_cast<float>(i) / static_cast<float>(configured - 1) : 0.0f);
			up += kind.state.level >= threshold ? 1 : 0;
		}
		*which = &kind;
		return up;
	}
	return -1;
}

// The ash storm's meteors, client side: while the storm shows at half strength or more in an ash
// level, a volley is asked for every so often (the server drops it, see WorldMeteors).
uint64_t g_nextMeteor = 0; // tick count of the next volley, 0 = not scheduled

void UpdateMeteors(uint64_t now, int level) {
	const bool falling = g_config.meteors != 0 && level > 0 && LevelFall(level) == kFallAsh && g_storms[kStormAsh].state.level >= 0.5f;
	if (!falling) {
		g_nextMeteor = 0;
		return;
	}
	if (g_nextMeteor == 0) {
		g_nextMeteor = now + static_cast<uint64_t>(ClientRange(g_config.meteorMinInterval, g_config.meteorMaxInterval)) * 1000;
	} else if (now >= g_nextMeteor) {
		g_nextMeteor = now + static_cast<uint64_t>(ClientRange(g_config.meteorMinInterval, g_config.meteorMaxInterval)) * 1000;
		++g_meteorRequests;
	}
}

// ---- real weather ----------------------------------------------------------------
// The HD weather object (WeatherTranslation) listens to the old 2D game's rain events.
// It is found through its "start precipitation" handler in the event hub (static object
// at 0x2a9aef0, handler list at +0x388, 0x30-byte entries {vtable, function, flag,
// object}). Its debug switch at +0x7a1 makes it ignore the 2D
// game, and update(object, {type, intensity}, debug = 1) starts or adjusts the effect.
// The game only knows two kinds, rain (0) and snow (1). Each plays the particle file named by
// one of ten strings the object reads from weather_effects.json: {char* text, length, capacity}
// every 0xb8 bytes from +0x70 (rain, snow, four splashes, four bubbles). Starting a kind sets
// the "starting" byte at +0x60 and queues a job that creates the effect from that string and
// stores its entity handle at +0x64 (0xfffff = none); stop destroys the entity whose handle is
// at +0x64. The game keeps that entity on the player.
using WeatherFn = void (*)(void* self, void* params, uint8_t fromDebug);
constexpr uint32_t kWeatherHub    = 0x2a9aef0;
constexpr uint32_t kWeatherStart  = 0x7cb830;
constexpr uint32_t kWeatherUpdate = 0x7cb6b0;
constexpr uint32_t kWeatherStop   = 0x7cba70;
constexpr uint32_t kSnowString    = 0x70 + 0xb8;
constexpr uint32_t kNoHandle      = 0xfffff;

// The renderer's entity world (static object; its +0x68 points at the registry):
//   registry +0x00  component stores, 0x28 bytes each, the store pointer at +0x20
//            +0x30  begin / +0x38 end of the entity handle table (u32 each)
//   a store  +0x08  begin / +0x10 end of its pages: page[(handle >> 8) & 0xfff][handle & 0xff] = index
//            +0x38  its components
// Transform components (store number at kTransformType, 0xd0 bytes) hold three 4x4 matrices;
// the translation of the world one is at +0x70.
constexpr uint32_t kEntityWorld   = 0x2677170;
constexpr uint32_t kTransformType = 0x2a978dc;
// spawn(renderer, path, matrix, parent entity, name, once) -> entity handle: a particle effect
// at a place in the world. The game uses it for rain splashes. once = 0 keeps it until stopped.
// queue(entity world, entity, job, 1): runs a job where the game itself creates entities.
// A job is {vtable {destroy, invoke, clone}, 0x38 bytes of its own, "in use" byte at +0x40}.
using SpawnEffectFn   = uint32_t (*)(void* renderer, const char* path, const float* matrix, uint32_t parent, const char* name, uint8_t once);
using QueueJobFn      = void (*)(void* world, uint32_t entity, void* job, uint32_t flag);
using RendererFn      = void* (*)();
constexpr uint32_t kSpawnEffect = 0xe37040;
constexpr uint32_t kQueueJob    = 0x912ae0;
constexpr uint32_t kRenderer    = 0xe21140;

bool      g_weatherReady = false;
bool      g_fieldReady   = false; // the entity world code looks as expected too: sand can be laid out
bool      g_weatherHeld  = false; // we switched the override on
bool      g_weatherOn    = false; // we started rain or snow (the game's own handle)
uint64_t  g_weatherNext  = 0;
int       g_weatherLogs  = 0;

uintptr_t FindWeather() {
	uint64_t list[2] = {}; // {begin, count}
	if (!SafeRead(g_base + kWeatherHub + 0x388, &list) || list[0] == 0 || list[1] == 0 || list[1] > 64) {
		return 0;
	}
	static uint8_t entries[64 * 0x30]; // {vtable, function, flag, object, ...} each, seen in live memory
	if (!SafeReadBytes(list[0], entries, list[1] * 0x30)) {
		return 0;
	}
	for (uint64_t i = 0; i < list[1]; ++i) {
		uintptr_t function = 0;
		uintptr_t object   = 0;
		memcpy(&function, entries + i * 0x30 + 8, 8);
		memcpy(&object, entries + i * 0x30 + 0x18, 8);
		if (function == g_base + kWeatherStart && object != 0) {
			uint8_t flags[2] = { 2, 2 };
			if (SafeRead(object + 0x7a0, &flags) && flags[0] <= 1 && flags[1] <= 1) {
				return object;
			}
		}
	}
	return 0;
}

uintptr_t g_weatherObject    = 0;  // found once, then only checked
constexpr int kMarkerKind    = 100; // a storm marker's g_weatherKind: 100 + 100 * the storm's number + its effect
int       g_weatherKind      = -1; // what is playing: 0 rain, 1 snow, or a storm's marker (see kMarkerKind)
int       g_weatherSetType   = -2; // what the last update call asked for...
int       g_weatherSetLevel  = -2; // ...in which level...
int       g_weatherSetAmount = -1; // ...at which intensity
uint64_t  g_weatherReassert  = 0;

template <typename T>
T& WeatherField(uint32_t offset) {
	return *reinterpret_cast<T*>(g_weatherObject + offset);
}

// Reads from the game's memory go through a system call that fails instead of crashing (the
// game changes these structures on its own threads), so they are counted: what a pass needs of
// the entity world is read once and kept.
struct WorldView {
	uintptr_t stores     = 0; // component stores
	uintptr_t tableBegin = 0; // entity handle table
	uintptr_t tableEnd   = 0;
	bool      valid      = false;
};

WorldView ReadWorld() {
	WorldView view;
	uintptr_t registry   = 0;
	uintptr_t head[8]    = {}; // +0x00 stores, +0x30 / +0x38 handle table
	if (SafeRead(g_base + kEntityWorld + 0x68, &registry) && registry != 0 && SafeRead(registry, &head) && head[0] != 0 && head[6] != 0 && head[7] > head[6]) {
		view.stores     = head[0];
		view.tableBegin = head[6];
		view.tableEnd   = head[7];
		view.valid      = true;
	}
	return view;
}

// The check the game itself makes before it uses an entity handle.
bool HandleAlive(const WorldView& world, uint32_t handle) {
	uint32_t       entry = 0;
	const uint64_t index = handle & 0xfffff;
	return world.valid && handle != kNoHandle && index < (world.tableEnd - world.tableBegin) / 4 && SafeRead(world.tableBegin + index * 4, &entry) && entry == handle;
}

bool HandleAlive(uint32_t handle) {
	return HandleAlive(ReadWorld(), handle);
}

// The whole handle table in one read, for checking many handles: a copy as of now.
constexpr size_t kTableCopy = 65536;
uint32_t         g_tableCopy[kTableCopy];
size_t           g_tableCopied = 0;

bool CopyHandleTable(const WorldView& world) {
	g_tableCopied = 0;
	if (!world.valid) {
		return false;
	}
	const size_t count = (world.tableEnd - world.tableBegin) / 4;
	if (count == 0 || count > kTableCopy || !SafeReadBytes(world.tableBegin, g_tableCopy, count * 4)) {
		return false;
	}
	g_tableCopied = count;
	return true;
}

bool AliveInCopy(uint32_t handle) {
	const size_t index = handle & 0xfffff;
	return handle != kNoHandle && index < g_tableCopied && g_tableCopy[index] == handle;
}

// Where an entity is in the renderer's world (x, height, z; two units to a sub-tile).
bool EntityPosition(const WorldView& world, uint32_t handle, float* position) {
	static uint32_t type = ~0u; // the store number of transform components never changes once the game has set it
	if (type == ~0u) {
		uint32_t read = 0;
		if (!SafeRead(g_base + kTransformType, &read) || read > 4096) {
			return false;
		}
		type = read;
	}
	uintptr_t store = 0, head[8] = {}, page = 0; // a store: +0x08 / +0x10 pages, +0x38 components
	uint32_t  index = 0;
	if (!world.valid || !SafeRead(world.stores + type * 0x28ULL + 0x20, &store) || store == 0 || !SafeRead(store, &head) || head[1] == 0 || head[7] == 0) {
		return false;
	}
	const uint64_t pageNumber = (handle >> 8) & 0xfff;
	if (pageNumber >= (head[2] - head[1]) / 8 || !SafeRead(head[1] + pageNumber * 8, &page) || page == 0 || !SafeRead(page + (handle & 0xff) * 4ULL, &index)
		|| (index & 0xfffff) == 0xfffff) {
		return false;
	}
	float xyz[3] = {};
	if (!SafeRead(head[7] + (index & 0xfffff) * 0xd0ULL + 0x70, &xyz)) {
		return false;
	}
	memcpy(position, xyz, sizeof(xyz));
	return std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]);
}

struct SlotString {
	uintptr_t text;
	uint64_t  length;
};
SlotString g_snowSaved {};
bool       g_snowSwapped = false;
int        g_swapTicks   = 0;
char       g_snowPath[200];

// Points the game's snow effect name at another particle file. False when the name does not
// look the way it did when this was written.
bool SwapSnowString(const char* path) {
	SlotString& slot = WeatherField<SlotString>(kSnowString);
	if (!g_snowSwapped) {
		char head[5] = {};
		if (slot.length == 0 || slot.length > 250 || !SafeRead(slot.text, &head) || memcmp(head, "data/", 5) != 0) {
			return false;
		}
		g_snowSaved = slot;
	}
	snprintf(g_snowPath, sizeof(g_snowPath), "%s", path);
	slot.text     = reinterpret_cast<uintptr_t>(g_snowPath);
	slot.length   = strlen(g_snowPath);
	g_snowSwapped = true;
	g_swapTicks   = 0;
	return true;
}

void RestoreSnowString() {
	if (g_snowSwapped && g_weatherObject != 0) {
		WeatherField<SlotString>(kSnowString) = g_snowSaved;
	}
	g_snowSwapped = false;
}

// ---- the sandstorm's field of blowing sand ---------------------------------------------------
// The game's sand effects are patches a few tiles across that stay where they are put. A storm
// lays them out on a grid in the world around the player: as the player walks, patches ahead
// are created and patches left behind are removed. Where the player is comes from the game's
// own weather effect, which the game keeps on the player: while a storm blows, the weather is
// "snow", but with the snow effect's name pointed at the storm's first sand effect for the
// moment the game creates it. That one is the storm's marker (and a patch that is always
// where the player is); no snow is ever created.
// Patches are created by a job in the game's entity world, the way the game creates its own.
constexpr int kMaxPatches = kMaxLayers * (2 * kMaxFieldRadius + 3) * (2 * kMaxFieldRadius + 3) + 16; // the field and the cell kept around it
enum : int { kPatchFree = 0, kPatchQueued = 1, kPatchUp = 2, kPatchFailed = 3 };
struct Patch {
	std::atomic<int>      state { kPatchFree };
	std::atomic<bool>     cancel { false }; // queued, but no longer wanted: its job creates nothing
	std::atomic<uint32_t> handle { kNoHandle };
	const StormKind*      kind   = nullptr; // the storm whose field it belongs to
	int                   effect = 0;       // number in that storm's effect list
	int                   layer  = 0;       // which of the three settings it belongs to
	int                   column = 0; // grid cell
	int                   row    = 0;
	float                 position[3] {};
};
Patch    g_patches[kMaxPatches];
int      g_patchLogs  = 0;
float    g_patchBudget = 0.0f; // patches that may be created now (builds up at the configured rate)
uint64_t g_patchClock  = 0;    // when the budget was last topped up
uint64_t g_patchAliveNext = 0; // when every patch is next checked against the game's entity table
int      g_patchFreeHint  = 0; // where the search for a free slot starts
// Each removal is a call into the game that destroys an effect. A whole field going at once
// (leaving the area, a layer dropping as a storm dies down) is hundreds of them, so they are
// spread over passes a tenth of a second apart, as creations are.
constexpr int kMaxStopsAPass = 64;
// how the player is moving: where they were at the last look, their speed (world units a
// second, smoothed), and how far the field leans ahead of them for it
float    g_fieldSeenAt[2] {};
float    g_fieldSpeed[2] {};
float    g_fieldLean[2] {};
uint64_t g_fieldSeenClock = 0;
std::atomic<int> g_patchesUp { 0 };     // for "horror status"

struct Job {
	const void* const* vtable;
	uint64_t           own[7];
	uint8_t            inUse;
	uint8_t            pad[7];
};

void* __cdecl JobDestroy(void* self, uint32_t) { return self; }

void* __cdecl JobClone(void* self, void* target) {
	memcpy(target, self, offsetof(Job, inUse)); // the caller marks the copy as in use
	return target;
}

// Runs inside the game's entity world: creates the patch.
void __cdecl JobInvoke(void* self, void*, void*) {
	const uint64_t slot = static_cast<Job*>(self)->own[0];
	if (slot >= kMaxPatches) {
		return;
	}
	Patch& patch = g_patches[slot];
	if (patch.state.load() != kPatchQueued) {
		return;
	}
	if (patch.cancel.load()) {
		patch.state = kPatchFree;
		return;
	}
	alignas(16) float matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, patch.position[0], patch.position[1], patch.position[2], 1 };
	void*          renderer      = reinterpret_cast<RendererFn>(g_base + kRenderer)();
	const uint32_t handle        = renderer != nullptr
		? reinterpret_cast<SpawnEffectFn>(g_base + kSpawnEffect)(renderer, patch.kind->effects[patch.effect].path, matrix, kNoHandle, nullptr, 0)
		: kNoHandle;
	patch.handle = handle;
	patch.state  = handle != kNoHandle ? kPatchUp : kPatchFailed;
}

const void* const kJobVtable[3] = { reinterpret_cast<const void*>(&JobDestroy), reinterpret_cast<const void*>(&JobInvoke), reinterpret_cast<const void*>(&JobClone) };

void QueuePatch(int slot) {
	Job job {};
	job.vtable = kJobVtable;
	job.own[0] = static_cast<uint64_t>(slot);
	job.inUse  = 1;
	reinterpret_cast<QueueJobFn>(g_base + kQueueJob)(reinterpret_cast<void*>(g_base + kEntityWorld), kNoHandle, &job, 1);
}

// Removes a patch: the game's own stop destroys the entity whose handle the weather object
// holds, so the patch's handle is put there for the call and the game's own put back after.
// (The game's stop checks the handle itself and does nothing for one that is gone.)
void StopPatch(uint32_t handle) {
	const uint32_t ownHandle = WeatherField<uint32_t>(0x64);
	const int32_t  ownType   = WeatherField<int32_t>(0x68);
	WeatherField<uint32_t>(0x64) = handle;
	uint8_t flag                 = 1;
	reinterpret_cast<WeatherFn>(g_base + kWeatherStop)(reinterpret_cast<void*>(g_weatherObject), &flag, 1);
	WeatherField<uint32_t>(0x64) = ownHandle;
	WeatherField<int32_t>(0x68)  = ownType;
}

// The weather object is gone or new: nothing kept about the old one is true any more.
void ForgetWeather() {
	g_weatherObject  = 0;
	g_weatherSetType = -2;
	g_weatherKind    = -1;
	g_weatherOn      = false;
	g_snowSwapped    = false;
}

// The effect each of the storm's three settings wants up right now, 0 = none: the first
// "layers" of the configured ones. The same effect in two settings is two fields of it, one over
// the other: a thicker storm. Returns how many are wanted. storm: nullptr = no storm here.
int WantedEffects(const StormKind* storm, int layers, int* wanted) {
	int count = 0;
	for (int layer = 0; layer < kMaxLayers; ++layer) {
		wanted[layer] = 0;
	}
	if (storm != nullptr && layers > 0 && g_fieldReady) {
		int configured = 0;
		for (int layer = 0; layer < kMaxLayers; ++layer) {
			const int effect = storm->config->layer[layer];
			if (effect <= 0 || effect >= storm->effectCount) {
				continue;
			}
			if (configured++ < layers && !storm->failed[effect]) {
				wanted[layer] = effect;
				++count;
			}
		}
	}
	return count;
}

// A spot inside a grid cell that is always the same for that cell and layer, so the grid does
// not show and layers of the same effect do not sit on top of each other: up to a quarter of
// the spacing off the cell's centre either way.
float PatchJitter(int column, int row, int layer, int axis) {
	uint32_t h = static_cast<uint32_t>(column) * 0x9E3779B1u ^ static_cast<uint32_t>(row) * 0x85EBCA6Bu ^ static_cast<uint32_t>(layer * 2 + axis + 1) * 0xC2B2AE35u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return (static_cast<float>(h & 0xffff) / 65535.0f - 0.5f) * 0.5f;
}

// Brings the field in line with what is wanted. storm: the kind of storm the player is out in
// (nullptr: none); layers: how many of its configured effects should be up (0 or less: none).
// Returns true while there is still work to do.
bool UpdateField(uint64_t now, const StormKind* storm, int layers) {
	// the field's own settings are the storm's (with no storm here nothing is laid out, and what
	// is left of another storm's field is only removed)
	const StormConfig& config = storm != nullptr ? *storm->config : *g_storms[kStormSand].config;
	// how many patches may be created this time: the configured number a second, saved up for
	// at most a quarter of a second (at least one), so a field never appears in one burst
	// unless the rate asks for that
	const float rate   = static_cast<float>(config.fieldRate);
	const float saved  = rate / 4.0f > 1.0f ? rate / 4.0f : 1.0f;
	const float passed = g_patchClock != 0 && now > g_patchClock ? static_cast<float>(now - g_patchClock) / 1000.0f : 1.0f;
	g_patchClock       = now;
	g_patchBudget      = g_patchBudget + rate * passed < saved ? g_patchBudget + rate * passed : saved;
	int       wanted[kMaxLayers];
	const int wantedCount = WantedEffects(storm, layers, wanted);

	// nothing up, nothing queued and nothing wanted: the usual pass outside a storm
	if (wantedCount == 0 && g_patchesUp.load(std::memory_order_relaxed) == 0) {
		bool any = false;
		for (const Patch& patch : g_patches) {
			any = any || patch.state.load(std::memory_order_relaxed) != kPatchFree;
		}
		if (!any) {
			g_fieldSeenClock = 0;
			return false;
		}
	}

	// where the player is: the game's weather entity, once the game has finished creating it
	const WorldView world = ReadWorld();
	float      centre[3]  = {};
	const bool settled    = WeatherField<uint8_t>(0x60) == 0;
	const bool haveCentre = wantedCount > 0 && g_weatherOn && g_weatherKind >= kMarkerKind && settled && HandleAlive(world, WeatherField<uint32_t>(0x64))
		&& EntityPosition(world, WeatherField<uint32_t>(0x64), centre);
	const float spacing   = static_cast<float>(config.fieldSpacing) * 2.0f; // world units
	const int   radius    = config.fieldRadius;

	// The field leans ahead of a moving player: a new patch needs a moment to fill with sand,
	// so it is created further off in the direction of travel, about two seconds' worth but
	// never more than the configured share of the field's reach. Leaning takes half a second;
	// coming back when the player stops takes ten, so that stopping to fight and running on
	// does not tear the field down behind and build it up again in front each time.
	if (haveCentre) {
		const float seconds = g_fieldSeenClock != 0 && now > g_fieldSeenClock ? static_cast<float>(now - g_fieldSeenClock) / 1000.0f : 0.0f;
		const float most    = static_cast<float>(config.fieldLead) / 100.0f * static_cast<float>(radius) * spacing;
		const float moved[2] = { centre[0] - g_fieldSeenAt[0], centre[2] - g_fieldSeenAt[1] };
		const bool  travel  = seconds > 0.02f && seconds < 3.0f && std::fabs(moved[0]) < 400.0f && std::fabs(moved[1]) < 400.0f; // a jump (waypoint, portal) is not travel
		if (travel) {
			const float blend = seconds < 0.5f ? seconds / 0.5f : 1.0f;
			g_fieldSpeed[0] += (moved[0] / seconds - g_fieldSpeed[0]) * blend;
			g_fieldSpeed[1] += (moved[1] / seconds - g_fieldSpeed[1]) * blend;
		} else if (seconds > 0.02f) {
			g_fieldSpeed[0] = g_fieldSpeed[1] = 0.0f;
			g_fieldLean[0] = g_fieldLean[1] = 0.0f;
		}
		const bool  moving = g_fieldSpeed[0] * g_fieldSpeed[0] + g_fieldSpeed[1] * g_fieldSpeed[1] > 8.0f * 8.0f; // faster than 4 sub-tiles a second
		const float blend  = !travel ? 0.0f : moving ? (seconds < 0.5f ? seconds / 0.5f : 1.0f) : (seconds < 10.0f ? seconds / 10.0f : 1.0f);
		for (int axis = 0; axis < 2; ++axis) {
			const float ahead  = g_fieldSpeed[axis] * 2.0f;
			const float target = !moving ? 0.0f : ahead > most ? most : ahead < -most ? -most : ahead;
			g_fieldLean[axis] += (target - g_fieldLean[axis]) * blend;
			g_fieldLean[axis]  = g_fieldLean[axis] > most ? most : g_fieldLean[axis] < -most ? -most : g_fieldLean[axis];
			g_fieldSeenAt[axis] = centre[axis * 2];
		}
		g_fieldSeenClock = now;
	} else {
		g_fieldSeenClock = 0;
		g_fieldSpeed[0] = g_fieldSpeed[1] = 0.0f;
		g_fieldLean[0] = g_fieldLean[1] = 0.0f;
	}
	const float middle[2] = { centre[0] + g_fieldLean[0], centre[2] + g_fieldLean[1] };
	const int   column    = static_cast<int>(std::floor(middle[0] / spacing));
	const int   row       = static_cast<int>(std::floor(middle[1] / spacing));
	// A patch is created when its cell is in the field, and removed when its cell's middle is
	// more than a whole cell beyond the field's reach: half a cell of slack, so that a player
	// standing on a cell's edge does not make patches come and go.
	const float reach = (static_cast<float>(radius) + 1.0f) * spacing;

	// every patch against the game's entity table, once a second, from one copy of the table
	const bool checkAlive = now >= g_patchAliveNext && CopyHandleTable(world);
	if (now >= g_patchAliveNext) {
		g_patchAliveNext = now + 1000;
	}

	bool busy  = false;
	int  up    = 0;
	int  stops = 0;
	bool present[kMaxLayers][2 * kMaxFieldRadius + 1][2 * kMaxFieldRadius + 1] = {};
	for (Patch& patch : g_patches) {
		const int state = patch.state.load(std::memory_order_relaxed);
		if (state == kPatchFree) {
			continue;
		}
		const int layer = patch.layer;
		// kept while this storm's setting still wants that effect and (when the player's place is known) it is within reach
		const float x    = (static_cast<float>(patch.column) + 0.5f) * spacing - middle[0];
		const float z    = (static_cast<float>(patch.row) + 0.5f) * spacing - middle[1];
		const bool  keep = patch.kind == storm && wanted[layer] == patch.effect && (!haveCentre || (std::fabs(x) <= reach && std::fabs(z) <= reach));
		const int   dx   = patch.column - column;
		const int   dy   = patch.row - row;
		const bool  inField = keep && haveCentre && dx >= -radius && dx <= radius && dy >= -radius && dy <= radius;
		if (state == kPatchQueued) {
			if (!keep) {
				patch.cancel = true; // its job has not run yet: it will create nothing
			}
			busy = true;
		} else if (state == kPatchFailed) {
			if (!patch.kind->failed[patch.effect]) {
				patch.kind->failed[patch.effect] = true;
				Log("%s: effect %d (%s) left out - the game did not create it", patch.kind->name, patch.effect, patch.kind->effects[patch.effect].name);
			}
			patch.state = kPatchFree;
			continue;
		} else if (checkAlive && !AliveInCopy(patch.handle.load(std::memory_order_relaxed))) {
			patch.state = kPatchFree; // it went with its scene
			continue;
		} else if (!keep) {
			if (settled && stops < kMaxStopsAPass) {
				StopPatch(patch.handle.load(std::memory_order_relaxed));
				patch.state = kPatchFree;
				++stops;
			} else {
				busy = true; // the game is busy with its own effect, or enough for one frame: next time
			}
			continue;
		} else {
			++up;
		}
		if (inField) {
			present[layer][dx + radius][dy + radius] = true;
		}
	}
	g_patchesUp = up;

	// missing patches, as many as the budget allows, the nearest to the player first
	while (haveCentre) {
		int   bestLayer = -1, bestX = 0, bestY = 0;
		float bestDistance = 0.0f;
		for (int layer = 0; layer < kMaxLayers; ++layer) {
			if (wanted[layer] == 0) {
				continue;
			}
			for (int dx = -radius; dx <= radius; ++dx) {
				for (int dy = -radius; dy <= radius; ++dy) {
					if (present[layer][dx + radius][dy + radius]) {
						continue;
					}
					const float x        = (static_cast<float>(column + dx) + 0.5f) * spacing - centre[0];
					const float z        = (static_cast<float>(row + dy) + 0.5f) * spacing - centre[2];
					const float distance = x * x + z * z + static_cast<float>(layer); // layers of one cell in their own order
					if (bestLayer < 0 || distance < bestDistance) {
						bestLayer    = layer;
						bestX        = dx;
						bestY        = dy;
						bestDistance = distance;
					}
				}
			}
		}
		if (bestLayer < 0) {
			break; // the field is whole
		}
		busy = true;
		if (g_patchBudget < 1.0f) {
			break;
		}
		int slot = -1;
		for (int i = 0; i < kMaxPatches && slot < 0; ++i) {
			const int candidate = (g_patchFreeHint + i) % kMaxPatches;
			if (g_patches[candidate].state.load(std::memory_order_relaxed) == kPatchFree) {
				slot = candidate;
			}
		}
		if (slot < 0) {
			break; // every slot is taken (queued patches free theirs as they come up or are removed)
		}
		g_patchFreeHint   = slot + 1;
		Patch& patch      = g_patches[slot];
		patch.kind        = storm;
		patch.effect      = wanted[bestLayer];
		patch.layer       = bestLayer;
		patch.column      = column + bestX;
		patch.row         = row + bestY;
		patch.position[0] = (static_cast<float>(patch.column) + 0.5f + PatchJitter(patch.column, patch.row, bestLayer, 0)) * spacing;
		patch.position[1] = centre[1];
		patch.position[2] = (static_cast<float>(patch.row) + 0.5f + PatchJitter(patch.column, patch.row, bestLayer, 1)) * spacing;
		patch.handle      = kNoHandle;
		patch.cancel      = false;
		patch.state       = kPatchQueued;
		QueuePatch(slot);
		g_patchBudget -= 1.0f;
		present[bestLayer][bestX + radius][bestY + radius] = true;
		if (++g_patchLogs <= 6) {
			Log("%s: patch of effect %d (%s) at %.0f, %.0f, %.0f", storm->name, patch.effect, storm->effects[patch.effect].name, patch.position[0], patch.position[1], patch.position[2]);
		}
	}
	return busy;
}

// Queued patches create nothing from now on (the plugin is going away, or the game is).
void CancelField() {
	for (Patch& patch : g_patches) {
		if (patch.state.load() == kPatchQueued) {
			patch.cancel = true;
		}
	}
}

// ---- probe: can the game create this effect here? ("horror probe") ----
// For choosing a storm's effects. One effect of the list is made the weather's marker for a few
// seconds, the way a storm's first effect is (so it sits on the player), and whether the game
// created it is said in the console and the log. "horror probe all" goes through the list.
const StormEffect kProbeEffects[] = {
	{ "none", nullptr },
	{ "Act 4 Diab lava splash", "data/hd/vfx/particles/env/Act4/Diab/FX_Act4Diab_lavasplash.particles" },
	{ "Act 4 Expansion edge blend 50", "data/hd/vfx/particles/env/Act4/Expansion/FX_Act4Expansion_edgeblend_50.particles" },
	{ "Act 4 Expansion lava glow 30x30", "data/hd/vfx/particles/env/Act4/Expansion/FX_Act4Expansion_lavaglow_30x30.particles" },
	{ "Act 4 Expansion lava splash", "data/hd/vfx/particles/env/Act4/Expansion/FX_Act4Expansion_lavasplash.particles" },
	{ "Act 4 Fort falling rocks", "data/hd/vfx/particles/env/Act4/Fort/FX_Act4Fort_fallingrocks.particles" },
	{ "Act 4 Fort fog 30x30", "data/hd/vfx/particles/env/Act4/Fort/FX_Act4Fort_fog_30x30.particles" },
	{ "Act 4 Mesa chimney pit 10x10", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_ChimneyPit_10x10.particles" },
	{ "Act 4 Mesa chimney pit 20x20", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_ChimneyPit_20x20.particles" },
	{ "Act 4 Mesa chimney pit 40x40", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_ChimneyPit_40x40.particles" },
	{ "Act 4 Mesa falling rocks", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_FallingRocks.particles" },
	{ "Act 4 Mesa fog 30x30", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_fog_30x30.particles" },
	{ "Act 4 Mesa hole haze 10x10", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_HoleHaze_10x10.particles" },
	{ "Act 4 Mesa hole haze 20x20", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_HoleHaze_20x20.particles" },
	{ "Act 4 Mesa hole haze 40x40", "data/hd/vfx/particles/env/Act4/Mesa/fx_act4mesa_holehaze_40x40.particles" },
	{ "Act 4 Mesa hole steam 10x10", "data/hd/vfx/particles/env/Act4/Mesa/fx_act4mesa_holesteam_10x10.particles" },
	{ "Act 4 Mesa hole steam 20x20", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_HoleSteam_20x20.particles" },
	{ "Act 4 Mesa hole steam 40x40", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_HoleSteam_40x40.particles" },
	{ "Act 4 Mesa wind huge 40x40", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_Wind_Huge_40x40.particles" },
	{ "Act 4 Mesa wind small 10x10", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_Wind_Small_10x10.particles" },
	{ "Act 4 Mesa wind ribbons 20x40", "data/hd/vfx/particles/env/Act4/Mesa/FX_Act4Mesa_WindRibbons_20x40.particles" },
	{ "Mesa hole haze 10x10 (other file)", "data/hd/vfx/particles/env/Act4/Mesa/FX_HoleHaze_Mesa_10x10.particles" },
	{ "Mesa hole haze 20x20 (other file)", "data/hd/vfx/particles/env/Act4/Mesa/FX_HoleHaze_Mesa_20x20.particles" },
	{ "Mesa hole haze 40x40 (other file)", "data/hd/vfx/particles/env/Act4/Mesa/FX_HoleHaze_Mesa_40x40.particles" },
	{ "Mesa wind huge (other file)", "data/hd/vfx/particles/env/Act4/Mesa/FX_Wind_Huge.particles" },
	{ "Mesa wind small (other file)", "data/hd/vfx/particles/env/Act4/Mesa/FX_Wind_Small.particles" },
	{ "Mesa wind ribbons (other file)", "data/hd/vfx/particles/env/Act4/Mesa/FX_WindRibbons_Mesa.particles" },
	{ "ash embers 10x10", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_10x10.particles" },
	{ "ash embers 10x30", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_10x30.particles" },
	{ "ash embers 20x20", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_20x20.particles" },
	{ "ash embers 30x30", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_30x30.particles" },
	{ "ash embers high 30x30", "data/hd/vfx/particles/env/common/generic/FX_ashEmbers_high_30x30.particles" },
	{ "ambient haze 30x30", "data/hd/vfx/particles/env/common/generic/FX_AmbientHaze_30x30.particles" },
	{ "ambient haze high 30x30", "data/hd/vfx/particles/env/common/generic/FX_AmbientHaze_high_30x30.particles" },
	{ "dust motes 30x30", "data/hd/vfx/particles/env/common/generic/FX_DustMotes_30x30.particles" },
	{ "floor haze 40x40", "data/hd/vfx/particles/env/common/generic/FX_FloorHaze_40x40.particles" },
	{ "scattered haze 30x30", "data/hd/vfx/particles/env/common/generic/FX_ScatteredHaze_30x30.particles" },
	{ "soft haze 30x30", "data/hd/vfx/particles/env/common/generic/FX_SoftHaze_30x30.particles" },
	{ "Travincal embers 20x20", "data/hd/vfx/particles/env/Act3/Travincal/FX_Act3Travincal_embers_20x20.particles" },
	{ "Act 4 menu clouds", "data/hd/vfx/particles/env/UI/FX_UIFrontEndAct4_clouds.particles" },
	{ "Act 4 menu haze", "data/hd/vfx/particles/env/UI/FX_UIFrontEndAct4_haze.particles" },
	{ "Act 4 menu lava", "data/hd/vfx/particles/env/UI/FX_UIFrontEndAct4_lava.particles" },
	{ "Act 4 menu motes", "data/hd/vfx/particles/env/UI/FX_UIFrontEndAct4_motes.particles" },
	{ "campfire smoke", "data/hd/vfx/particles/env/Act1/Outdoors/FX_CampfireSmoke.particles" },
	{ "Tristram fire, tiny", "data/hd/vfx/particles/env/Act1/Tristram/FX_Act1Tristram_fire_tiny.particles" },
	{ "Tristram fire wall 10x10", "data/hd/vfx/particles/env/Act1/Tristram/FX_Act1Tristram_firewall_10x10.particles" },
	{ "harem smoke 30x30", "data/hd/vfx/particles/env/Act2/palace/FX_Act2PalaceHarem_smoke_30x30.particles" },
	{ "Lut Gholein smoke", "data/hd/vfx/particles/env/Act2/town/FX_Smoke.particles" },
	{ "Lut Gholein smoke, white", "data/hd/vfx/particles/env/Act2/town/FX_Smoke_White.particles" },
	{ "Kurast docks smoke", "data/hd/vfx/particles/env/Act3/DockTown/FX_Act3Docktown_smoke.particles" },
	{ "Harrogath forge smoke", "data/hd/vfx/particles/env/Expansion/Town/FX_Act5Town_forgesmoke.particles" },
};
constexpr int      kProbeCount  = static_cast<int>(sizeof(kProbeEffects) / sizeof(kProbeEffects[0]));
constexpr int      kProbeKind   = 1000; // a probe's g_weatherKind: 1000 + effect
constexpr uint64_t kProbeShowMs = 6000; // how long each one is left up
enum : int8_t { kProbeUnknown = 0, kProbeCreated = 1, kProbeNotCreated = 2 };
enum : int { kProbeAskAll = -1, kProbeAskStop = -2 };
std::atomic<int> g_probeRequest { 0 }; // from the console: an effect number, or one of the two above
int      g_probeEffect   = 0;     // the one up now, 0 = none
bool     g_probeAll      = false; // going through the whole list
bool     g_probeReported = false; // its outcome has been said
uint64_t g_probeUntil    = 0;
int8_t   g_probeOutcome[kProbeCount]; // what the game did with each, this session

void ProbeSay(const char* format, ...) {
	char    text[400];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Log("%s", text);
	if (g_ctx != nullptr) {
		g_ctx->WriteConsoleMessage(text);
	}
}

// The outcome of the probe that is up. One the game did not create is not waited on.
void ProbeResult(uint64_t now, bool created, const char* why) {
	if (g_probeEffect <= 0 || g_probeEffect >= kProbeCount || g_probeReported) {
		return;
	}
	g_probeReported               = true;
	g_probeOutcome[g_probeEffect] = created ? kProbeCreated : kProbeNotCreated;
	if (created) {
		ProbeSay("probe %d (%s): created - it is on you for %d seconds", g_probeEffect, kProbeEffects[g_probeEffect].name, static_cast<int>(kProbeShowMs / 1000));
	} else {
		ProbeSay("probe %d (%s): NOT created - %s", g_probeEffect, kProbeEffects[g_probeEffect].name, why);
		g_probeUntil = now;
	}
}

// The probe's time is up: the next of the list, or the end.
void NextProbe(uint64_t now) {
	const bool more = g_probeAll && g_probeEffect + 1 < kProbeCount;
	if (!more && g_probeAll) {
		int created = 0, missing = 0;
		for (int i = 1; i < kProbeCount; ++i) {
			created += g_probeOutcome[i] == kProbeCreated ? 1 : 0;
			missing += g_probeOutcome[i] == kProbeNotCreated ? 1 : 0;
		}
		ProbeSay("probe: the whole list is done - %d created, %d not (every line is in horror-night.log)", created, missing);
	}
	g_probeEffect   = more ? g_probeEffect + 1 : 0;
	g_probeAll      = more;
	g_probeReported = false;
	g_probeUntil    = now + kProbeShowMs;
}

// Once a second at most (ten times while the sandstorm's field is changing), and normally one
// memory check: the weather object is remembered, and the game's update is only called when
// the wanted weather changes (and every 10 s, in case the game stopped it on its own).
// storm, layers: see StormLayers.
void UpdateWeather(uint64_t now, int level, const StormKind* storm, int layers) {
	if (!g_weatherReady || now < g_weatherNext) {
		return;
	}
	g_weatherNext     = now + 1000;
	const bool inGame = level > 0;
	// a probe asked for in the console, or called off (leaving the game calls it off too)
	if (const int asked = g_probeRequest.exchange(0); asked != 0 || !inGame) {
		const bool start = inGame && (asked == kProbeAskAll || (asked > 0 && asked < kProbeCount));
		g_probeAll       = start && asked == kProbeAskAll;
		g_probeEffect    = !start ? 0 : asked == kProbeAskAll ? 1 : asked;
		g_probeReported  = false;
		g_probeUntil     = now + kProbeShowMs;
	}
	// turned off (also from the in-game settings menu): stop our rain and give the weather back.
	// A storm's blowing field does not need the rain and snow switched on.
	const bool wanted = inGame && (g_config.weatherEnabled != 0 || layers >= 0 || g_probeEffect != 0);
	if (!wanted && !g_weatherHeld) {
		return;
	}
	uint8_t flags[2] = { 2, 2 }; // +0x7a0 effects loaded, +0x7a1 override
	if (g_weatherObject != 0 && (!SafeRead(g_weatherObject + 0x7a0, &flags) || flags[0] > 1 || flags[1] > 1)) {
		ForgetWeather(); // gone or reused: look again
	}
	if (g_weatherObject == 0) {
		ForgetWeather();
		g_weatherObject = FindWeather();
		if (g_weatherObject == 0 || !SafeRead(g_weatherObject + 0x7a0, &flags)) {
			g_weatherObject = 0;
			if (inGame && g_weatherLogs < 2) {
				++g_weatherLogs;
				Log("weather: the game's weather object was not found (yet)");
			}
			return;
		}
	}
	if (flags[0] == 0) {
		return; // its effects are still loading; starting now would wait inside the game
	}
	auto* const weather = reinterpret_cast<void*>(g_weatherObject);

	// the snow effect's name is pointed at a sand effect: it goes back as soon as the game has
	// created the storm's marker from it, and nothing else happens until then
	if (g_snowSwapped) {
		if (WeatherField<uint8_t>(0x60) == 0) {
			RestoreSnowString();
		} else if (++g_swapTicks > 20) {
			// five seconds: the effect could not be made, and the game would refuse every start from now on
			WeatherField<uint8_t>(0x60) = 0;
			RestoreSnowString();
			if (g_weatherKind >= kProbeKind) {
				ProbeResult(now, false, "the game never finished creating it");
			} else if (g_weatherKind >= kMarkerKind && (g_weatherKind - kMarkerKind) / kMarkerKind < kStormKinds) {
				// a storm's marker: 100 + 100 * the storm's number + its effect
				const StormKind& made   = g_storms[(g_weatherKind - kMarkerKind) / kMarkerKind];
				const int        effect = g_weatherKind % kMarkerKind;
				if (effect > 0 && effect < made.effectCount) {
					made.failed[effect] = true;
					Log("%s: effect %d (%s) left out - the game never finished creating it", made.name, effect, made.effects[effect].name);
				}
			}
			g_weatherOn      = false;
			g_weatherKind    = -1;
			g_weatherSetType = -2;
		} else {
			g_weatherNext = now + 250;
			return;
		}
	}

	// a probe: what became of it, and the next one when its time is up
	if (g_probeEffect != 0) {
		g_weatherNext = now + 250;
		if (g_weatherOn && g_weatherKind == kProbeKind + g_probeEffect && !g_snowSwapped && WeatherField<uint8_t>(0x60) == 0) {
			ProbeResult(now, HandleAlive(WeatherField<uint32_t>(0x64)), "the game made nothing of it");
		}
		if (now >= g_probeUntil) {
			NextProbe(now);
		}
	}

	// the field first: patches that are no longer wanted go before the weather is handed back
	const bool fieldBusy = UpdateField(now, wanted ? storm : nullptr, wanted ? layers : 0);
	if (fieldBusy) {
		g_weatherNext = now + 100;
	}

	if (!wanted) {
		if (fieldBusy) {
			return; // patches are still being removed: the weather is handed back when they are gone
		}
		if (g_weatherOn) {
			uint8_t flag = 1;
			reinterpret_cast<WeatherFn>(g_base + kWeatherStop)(weather, &flag, 1);
			g_weatherOn = false;
		}
		g_weatherKind                = -1;
		WeatherField<uint8_t>(0x7a1) = 0; // hand the weather back to the game
		g_weatherHeld    = false;
		g_weatherSetType = -2;
		return;
	}
	if (flags[1] != 1) {
		WeatherField<uint8_t>(0x7a1) = 1;
	}
	g_weatherHeld = true;

	// what should be on: rain (0), snow (1), or nothing (-1). While a storm blows it is the
	// storm's marker: "snow" to the game, made from the first effect the storm wants
	// (see UpdateField).
	int       type   = -1;
	int       marker = 0;
	int       amount = g_config.weatherIntensity;
	const int fall   = LevelFall(level);
	const bool probe = g_probeEffect != 0; // a probe is a marker too, and goes before the rest
	if (probe) {
		marker = g_probeEffect;
		type   = 1;
		amount = 100;
	} else if (layers >= 0) {
		int wantedEffects[kMaxLayers];
		WantedEffects(storm, layers, wantedEffects);
		for (int layer = kMaxLayers - 1; layer >= 0; --layer) {
			if (wantedEffects[layer] != 0) {
				marker = wantedEffects[layer];
			}
		}
		if (marker != 0) {
			type   = 1;
			amount = 100;
		}
	} else if (g_config.weatherEnabled != 0 && (fall == kFallRain || fall == kFallSnow)) {
		type = fall == kFallRain ? 0 : 1;
	}
	const StormEffect* const markerEffect = marker == 0 ? nullptr : probe ? &kProbeEffects[marker] : &storm->effects[marker];
	const int kind = marker == 0 ? type : probe ? kProbeKind + marker : kMarkerKind + kMarkerKind * static_cast<int>(storm - g_storms) + marker;
	// something else is playing (a storm's marker where snow should fall, another effect chosen
	// for the storm): to the game they are all "snow", so it has to be stopped first
	if (g_weatherOn && kind != g_weatherKind) {
		uint8_t flag = 1;
		reinterpret_cast<WeatherFn>(g_base + kWeatherStop)(weather, &flag, 1);
		g_weatherOn      = false;
		g_weatherSetType = -2;
	}
	// the sandstorm's marker went with its scene: ask for it again at once, not at the next reassert
	if (g_weatherOn && marker != 0 && WeatherField<uint8_t>(0x60) == 0 && !HandleAlive(WeatherField<uint32_t>(0x64))) {
		g_weatherSetType = -2;
	}
	if (type >= 0) {
		// Rain and snow are asked for again on a change of level or intensity and every ten
		// seconds, in case the game stopped them. A storm's marker is watched directly (above),
		// so it is only asked for when it is not there: asking again would have the game set
		// an intensity the sand effect does not have, every time.
		if (type != g_weatherSetType || (marker == 0 && (level != g_weatherSetLevel || amount != g_weatherSetAmount || now >= g_weatherReassert))) {
			if (marker != 0 && !SwapSnowString(markerEffect->path)) {
				if (probe) {
					ProbeResult(now, false, "the game's weather effect names are not where they were expected");
					g_probeEffect = 0; // the same goes for every other one
					g_probeAll    = false;
				} else {
					storm->failed[marker] = true;
					Log("%s: effect %d (%s) left out - the game's weather effect names are not where they were expected", storm->name, marker, markerEffect->name);
				}
				return;
			}
			struct {
				int32_t type;
				float   intensity;
			} params { type, static_cast<float>(amount) / 100.0f };
			reinterpret_cast<WeatherFn>(g_base + kWeatherUpdate)(weather, &params, 1);
			if (g_snowSwapped && WeatherField<uint8_t>(0x60) == 0) {
				RestoreSnowString(); // nothing had to be created (it is already there), or it was made on the spot
			}
			if (!g_weatherOn && !probe && g_weatherLogs < 6) {
				++g_weatherLogs;
				if (marker != 0) {
					Log("weather: the %s's marker (effect %d, %s) started in level %d", storm->name, marker, markerEffect->name, level);
				} else {
					Log("weather: %s started in level %d at %d%%", type == 0 ? "rain" : "snow", level, amount);
				}
			}
			if (marker != 0) {
				g_weatherNext = now + 250; // the snow name goes back, and the field is laid out, as soon as the marker is there
			}
			g_weatherKind      = kind;
			g_weatherOn        = true;
			g_weatherSetType   = type;
			g_weatherSetLevel  = level;
			g_weatherSetAmount = amount;
			g_weatherReassert  = now + 10000;
		}
	} else if (g_weatherOn) {
		uint8_t flag = 1;
		reinterpret_cast<WeatherFn>(g_base + kWeatherStop)(weather, &flag, 1);
		g_weatherOn      = false;
		g_weatherSetType = -2;
	}
}

// ---- false health ----------------------------------------------------------------
// During a haunting the client's copy of the player says 1 life. The server's copy, the
// one that decides whether you live, is untouched; the real value is written back after.
bool g_falseApplied = false;
const D2RL::ThreadService* g_threads = nullptr;
std::atomic<bool>          g_falseJobQueued { false };

uintptr_t FindClientPlayer(uint32_t id) {
	uintptr_t buckets[128] = {}; // type 0 = players
	if (!SafeRead(g_base + 0x2A23910, &buckets)) {
		return 0;
	}
	for (int bucket = 0; bucket < 128; ++bucket) {
		uintptr_t unit = buckets[bucket];
		for (int guard = 0; unit != 0 && guard < 64; ++guard) {
			uint32_t header[4] = {};
			if (!SafeRead(unit, &header) || header[0] != 0) {
				break;
			}
			if (header[2] == id) {
				return unit;
			}
			if (!SafeRead(unit + 0x158, &unit)) {
				break;
			}
		}
	}
	return 0;
}

// One cheap check per call instead of walking the whole unit table every frame.
uintptr_t CachedClientPlayer(uint32_t id) {
	static uintptr_t cached = 0;
	uint32_t         header[3] = {};
	if (cached != 0 && SafeRead(cached, &header) && header[0] == 0 && header[2] == id) {
		return cached;
	}
	cached = FindClientPlayer(id);
	return cached;
}

// The overlay callback runs after the HUD has been drawn, so a life update from the
// server that arrived earlier in the frame would show for one frame. This job runs in the
// UI update before the HUD is drawn and writes the 1 again.
void __cdecl FalseHealthJob(const D2RL::PluginContext*, void*) noexcept {
	g_falseJobQueued = false;
	if (!g_haunting.load(std::memory_order_relaxed) || g_level.load(std::memory_order_relaxed) <= 0) {
		return;
	}
	const uintptr_t player = CachedClientPlayer(g_localPlayerId.load());
	if (player != 0) {
		Engine<SetStatFn>(kSetStat)(reinterpret_cast<Unit*>(player), 6, 256, 0);
	}
}

void UpdateFalseHealth(bool inGame) {
	if (!g_serverReady.load(std::memory_order_relaxed) || g_config.falseHealth == 0) {
		return;
	}
	const bool wanted = inGame && g_haunting.load(std::memory_order_relaxed);
	if (!wanted && !g_falseApplied) {
		return;
	}
	const uintptr_t player = inGame ? CachedClientPlayer(g_localPlayerId.load()) : 0;
	if (player == 0) {
		g_falseApplied = false;
		return;
	}
	if (wanted) {
		Engine<SetStatFn>(kSetStat)(reinterpret_cast<Unit*>(player), 6, 256, 0);
		if (g_threads != nullptr && !g_falseJobQueued.exchange(true)) {
			if (g_threads->runOnUiThread(g_ctx, FalseHealthJob, nullptr) != D2RL::Threads::Result::Success) {
				g_falseJobQueued = false;
			}
		}
		if (!g_falseApplied) {
			static int logs = 0;
			if (++logs <= 5) {
				Log("haunting: life orb set to 1 (real life %d/256)", g_realLife.load());
			}
		}
		g_falseApplied = true;
	} else {
		const int32_t real = g_realLife.load();
		Engine<SetStatFn>(kSetStat)(reinterpret_cast<Unit*>(player), 6, real > 0 ? real : 256, 0);
		g_falseApplied = false;
	}
}

// ---- whispers and stingers ----------------------------------------------------------
uint64_t g_ambientNext = 0;
int      g_ravenLeft   = 0;
uint64_t g_ravenNext   = 0;
float    g_ravenGain   = 0.0f;
uint32_t g_hauntSeen   = 0;

const Clip& AmbientClip(int first, int last) {
	return g_ambient[ClientRange(first, last)];
}

void UpdateSounds(uint64_t now, int level) {
	const bool  inGame = level > 0;
	const float volume = static_cast<float>(g_config.ambientVolume) / 100.0f;

	// a haunting announces itself
	const uint32_t haunts = g_hauntCount.load();
	if (haunts != g_hauntSeen) {
		g_hauntSeen = haunts;
		if (g_config.ambientEnabled != 0 && inGame) {
			PlayClip(AmbientClip(12, 15), volume * 0.9f);
			PlayClip(AmbientClip(0, 3), volume);
		}
	}

	if (g_config.ambientEnabled != 0 && inGame && !IsTown(level)) {
		if (g_ambientNext == 0) {
			g_ambientNext = now + static_cast<uint64_t>(ClientRange(g_config.ambientMin, g_config.ambientMax)) * 1000;
		} else if (now >= g_ambientNext) {
			g_ambientNext = now + static_cast<uint64_t>(ClientRange(g_config.ambientMin, g_config.ambientMax)) * 1000;
			const int   roll = static_cast<int>(ClientRand() % 100);
			const float gain = volume * static_cast<float>(ClientRange(35, 100)) / 100.0f;
			if (roll < 32) {
				PlayClip(AmbientClip(0, 3), gain); // breathing
			} else if (roll < 70) {
				PlayClip(AmbientClip(4, 11), gain); // moans
			} else if (roll < 80) {
				PlayClip(AmbientClip(12, 15), gain * 0.7f); // a distant scream
			} else if (roll < 97) {
				// ravens: three calls in a row
				g_ravenLeft = 3;
				g_ravenNext = now;
				g_ravenGain = gain;
			} else {
				PlayClip(g_ambient[18], gain * 0.6f); // a laugh, rarely
			}
		}
	} else {
		g_ambientNext = 0;
	}

	if (g_ravenLeft > 0 && now >= g_ravenNext) {
		--g_ravenLeft;
		g_ravenNext = now + static_cast<uint64_t>(ClientRange(650, 1150));
		if (inGame) {
			PlayClip(AmbientClip(16, 17), g_ravenGain);
		} else {
			g_ravenLeft = 0;
		}
	}
}

void SetUpClientExtras() {
	if (g_ctx->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads = nullptr;
	}
	const uint8_t kUpdate[16] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x40, 0x41, 0x0f, 0xb6, 0xf0, 0x48, 0x8b, 0xda, 0x48 };
	const uint8_t kStop[16]   = { 0x40, 0x55, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb1, 0x48, 0x81, 0xec, 0xf8, 0x00, 0x00, 0x00, 0x48 };
	const uint8_t kStart[16]  = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xec, 0xc0, 0x00 };
	const auto*   dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
	const auto*   nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
	g_weatherReady = nt->FileHeader.TimeDateStamp == kImageStamp && g_ctx->CheckExpectedBytes(kWeatherUpdate, kUpdate, sizeof(kUpdate))
		&& g_ctx->CheckExpectedBytes(kWeatherStop, kStop, sizeof(kStop)) && g_ctx->CheckExpectedBytes(kWeatherStart, kStart, sizeof(kStart));
	Log("weather: %s", g_weatherReady ? (g_config.weatherEnabled != 0 ? "on - the game's own rain and snow, always falling outdoors" : "available, switched off in the config")
										: "off - weather code does not look as expected");
	const uint8_t kSpawn[16]    = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56 };
	const uint8_t kQueue[16]    = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
	const uint8_t kGetRender[16] = { 0x48, 0x83, 0xec, 0x28, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0x8b, 0x0d, 0x75 };
	g_fieldReady = g_weatherReady && g_ctx->CheckExpectedBytes(kSpawnEffect, kSpawn, sizeof(kSpawn)) && g_ctx->CheckExpectedBytes(kQueueJob, kQueue, sizeof(kQueue))
		&& g_ctx->CheckExpectedBytes(kRenderer, kGetRender, sizeof(kGetRender));
	{
		// what a read of the game's memory costs on this system, for anyone tuning or debugging
		LARGE_INTEGER frequency {}, from {}, to {};
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&from);
		uint32_t value = 0;
		for (int i = 0; i < 1000; ++i) {
			static_cast<void>(SafeRead(g_base, &value));
		}
		QueryPerformanceCounter(&to);
		Log("memory reads: %.2f microseconds each on this system", frequency.QuadPart > 0 ? static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart) : 0.0);
	}
	Log("sandstorm: blowing sand %s", g_fieldReady ? "available" : "off - the game's effect code does not look as expected (the storm's fog and wind still work)");
	Log("sounds: whispers %s every %d-%d s", g_config.ambientEnabled != 0 ? "on" : "off", g_config.ambientMin, g_config.ambientMax);
}

void ReleaseClientExtras() {
	CancelField();
	RestoreSnowString();
	if (g_weatherHeld) {
		const uintptr_t weather = g_weatherObject != 0 ? g_weatherObject : FindWeather();
		if (weather != 0) {
			*reinterpret_cast<uint8_t*>(weather + 0x7a1) = 0;
		}
		g_weatherHeld = false;
	}
}

void __cdecl OnOverlayFrame(const D2RL::PluginContext* context, const D2RL::Overlay::Frame* frame, void*) noexcept {
	if (frame == nullptr || g_overlay == nullptr) {
		return;
	}
	const uint64_t now    = GetTickCount64();
	const bool     forced = g_forceStrike.exchange(false);
	const Area     area   = forced ? Area::Outdoor : StormArea(g_level.load(std::memory_order_relaxed));

	// no weather, no storm: lightning only comes with the rain and snow
	if (area == Area::None || ((g_config.stormEnabled == 0 || g_config.weatherEnabled == 0) && !forced)) {
		g_nextStrike = 0;
	} else {
		if (g_nextStrike == 0) {
			g_nextStrike = now + static_cast<uint64_t>(ClientRange(g_config.stormMinInterval, g_config.stormMaxInterval)) * 1000;
		}
		if (forced || now >= g_nextStrike) {
			g_nextStrike = now + static_cast<uint64_t>(ClientRange(g_config.stormMinInterval, g_config.stormMaxInterval)) * 1000;
			// a close strike: bright flash, thunder right behind it, loud; far: the opposite
			const int delay = ClientRange(150, 2600);
			const float closeness = 1.0f - static_cast<float>(delay - 150) / 2450.0f;
			g_thunderAt   = now + static_cast<uint64_t>(delay);
			g_thunderGain = (0.7f + 0.3f * closeness) * (area == Area::Indoor ? 0.45f : 1.0f);
			if (++g_strikeCount <= 5) {
				Log("storm: strike %d (%s), level %d, screen %.0fx%.0f, thunder in %d ms", g_strikeCount, forced ? "forced" : area == Area::Indoor ? "indoor" : "outdoor",
					g_level.load(), frame->screenWidth, frame->screenHeight, delay);
			}
			g_flashNow    = area == Area::Outdoor && g_config.screenFlash != 0;
			if (area == Area::Outdoor) {
				StartSkyFlash(now);
			}
			if (area == Area::Outdoor && g_config.flashStrength != 0) {
				++g_flashRequests;
			}
			g_flashStart  = now;
		}
	}

	const int level = g_level.load(std::memory_order_relaxed);
	UpdateStorms(now, level);
	UpdateMeteors(now, level);
	UpdateBursts(now, level);
	UpdateSky(now, level > 0, StormBlend());
	if (now >= g_weatherNext) { // once a second, ten times while a storm's field is changing
		StormKind* storm  = nullptr;
		const int  layers = StormLayers(level, &storm);
		UpdateWeather(now, level, storm, layers);
	}
	UpdateFalseHealth(level > 0);
	UpdateSounds(now, level);

	if (g_thunderAt != 0 && now >= g_thunderAt) {
		g_thunderAt = 0;
		PlayThunder(g_thunderGain);
	}

	if (g_flashNow) {
		const float alpha = FlashAlpha(now - g_flashStart);
		if (now - g_flashStart >= 650) {
			g_flashNow = false;
		} else if (alpha > 0.001f) {
			D2RL::Overlay::FilledRectangleRequest request {};
			request.structSize = D2RL::Overlay::FilledRectangleRequestSize;
			request.canvas     = frame->canvas;
			request.rect       = { 0.0f, 0.0f, frame->screenWidth, frame->screenHeight };
			request.color      = { 0.82f, 0.88f, 1.0f, alpha };
			const auto result = g_overlay->drawFilledRectangle(context, &request);
			auto lineResult   = D2RL::Overlay::Result::Success;
			if (result != D2RL::Overlay::Result::Success) {
				// fallback: one very thick line across the middle of the screen
				D2RL::Overlay::LineRequest line {};
				line.structSize = D2RL::Overlay::LineRequestSize;
				line.canvas     = frame->canvas;
				line.start      = { 0.0f, frame->screenHeight * 0.5f };
				line.end        = { frame->screenWidth, frame->screenHeight * 0.5f };
				line.color      = request.color;
				line.thickness  = frame->screenHeight;
				lineResult      = g_overlay->drawLine(context, &line);
			}
			if (!g_flashLogged) {
				g_flashLogged = true;
				Log("storm: first flash draw - rectangle result %u, line result %u, canvas %llu, alpha %.2f", static_cast<uint32_t>(result), static_cast<uint32_t>(lineResult),
					static_cast<unsigned long long>(frame->canvas), alpha);
			}
		}
	}
}

void __cdecl OnGameplayEvent(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (!D2RL::Lifecycle::HasGameplayEventField(event, D2RL::Lifecycle::GameplayEventRequiredSize)) {
		return;
	}
	if (event->kind == D2RL::Lifecycle::GameplayEventKind::LevelChanged) {
		g_level = event->currentValue;
		JoinSettingsMenu();
		if (++g_levelLogs <= 10) {
			Log("storm: level changed to %d", event->currentValue);
		}
	} else if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft) {
		g_level = -1;
	}
}

bool SetUpStorm() {
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (g_ctx->QueryService(&lifecycle) != D2RL::ServiceQueryResult::Success
		|| !D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, unregisterGameplayEventListener))) {
		Log("storm: off - the lifecycle service is missing");
		return false;
	}
	for (const auto kind : { D2RL::Lifecycle::GameplayEventKind::LevelChanged, D2RL::Lifecycle::GameplayEventKind::GameLeft }) {
		const D2RL::Lifecycle::GameplayEventListener listener {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.flags      = 0,
			.kind       = kind,
			.reserved   = 0,
			.callback   = OnGameplayEvent,
			.userData   = nullptr,
		};
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		if (lifecycle->registerGameplayEventListener(g_ctx, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
			Log("storm: off - could not listen for level changes");
			return false;
		}
	}

	if (g_ctx->QueryService(&g_overlay) != D2RL::ServiceQueryResult::Success || g_overlay == nullptr
		|| g_overlay->serviceSize < offsetof(D2RL::OverlayService, drawFilledRectangle) + sizeof(void*)) {
		g_overlay = nullptr;
		Log("storm: off - this D2RLoader has no overlay service");
		return false;
	}
	const D2RL::Overlay::CallbackRegistration registration {
		.structSize = D2RL::Overlay::CallbackRegistrationSize,
		.flags      = 0,
		.phase      = D2RL::Overlay::Phase::AfterGameUi,
		.priority   = 0,
		.callback   = OnOverlayFrame,
		.userData   = nullptr,
	};
	D2RL::Overlay::CallbackHandle handle = D2RL::Overlay::InvalidCallbackHandle;
	const auto result = g_overlay->registerFrameCallback(g_ctx, &registration, &handle);
	if (result != D2RL::Overlay::Result::Success) {
		Log("storm: off - overlay callback refused (result %u)", static_cast<uint32_t>(result));
		return false;
	}

	// sky flash: only when the code that reads these fields looks as expected
	const uint8_t kSetTime[16]  = { 0xf3, 0x0f, 0x11, 0x89, 0x94, 0x01, 0x00, 0x00, 0x44, 0x89, 0x81, 0x90, 0x01, 0x00, 0x00, 0xc3 };
	const uint8_t kOverride[7]  = { 0x80, 0xb8, 0x09, 0x19, 0x01, 0x00, 0x00 };
	const auto*   dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
	const auto*   nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
	g_skyReady = nt->FileHeader.TimeDateStamp == kImageStamp && g_ctx->CheckExpectedBytes(0xf5b190, kSetTime, sizeof(kSetTime))
		&& g_ctx->CheckExpectedBytes(0x7cd8fe, kOverride, sizeof(kOverride));
	Log("storm: sky flash %s", g_skyReady ? (g_config.skyFlash != 0 ? "on" : "available, switched off in the config") : "off - renderer code does not look as expected");
	if (g_skyReady) {
		const D2RL::PluginApi*       api   = g_ctx->GetApi();
		D2RL::InlineHookRegistration blend = D2RL::MakeInlineHook(kBlendRva, kBlendEntry, sizeof(kBlendEntry), reinterpret_cast<void*>(&BlendHook),
			reinterpret_cast<void**>(&g_originalBlend));
		const bool hooked = api != nullptr && D2RL::ApiInstallInlineHook(api) != nullptr && D2RL::ApiInstallInlineHook(api)(g_ctx, &blend) && g_originalBlend != nullptr;
		Log("storm: %s", hooked ? "lightning and sandstorms show through terror zones and other lighting events"
			: "could not hook the lighting blend - in terror zones the flash and the sandstorm look stay hidden");
	}
	Log("sandstorm: %s", !g_skyReady ? "off - renderer code does not look as expected"
		: g_config.sand.enabled != 0 ? "on where the mod installed the storm look (Act 2 desert)" : "switched off in the config");
	Log("ash storm: %s", !g_skyReady ? "off - renderer code does not look as expected"
		: g_config.ash.enabled != 0 ? "on where the mod installed the storm look (Act 4)" : "switched off in the config");

	// reading and processing the sounds takes most of a second: done off the game's threads
	StartMixer();
	g_soundLoader = CreateThread(nullptr, 0, SoundLoaderThread, nullptr, 0, nullptr);
	if (g_soundLoader == nullptr) {
		Log("sounds: could not start the loader thread");
	}
	SetUpClientExtras();
	Log("storm: on - strike every %d-%d s, flash %d, volume %d", g_config.stormMinInterval, g_config.stormMaxInterval, g_config.flashStrength, g_config.volume);
	return true;
}

// ---------------------------------------------------------------- console

// How each kind of storm is spoken of in the console, and where its settings are in the toml.
struct StormWords {
	const char* section;   // in horror-night.toml
	const char* stuff;     // what blows
	const char* toggle;    // "horror <toggle>" starts or stops one
	const char* fx;        // "horror <fx>" lists and sets its effects
	const char* field;     // "horror <field>" sets its field
	const char* where;
	const char* modOption; // the D2RMM option that installs its look
};
const StormWords kStormWords[kStormKinds] = {
	{ "sandstorm", "sand", "sand", "fx", "field", "the Act 2 desert", "Sandstorms in Act 2" },
	{ "ashstorm", "ash", "ash", "ashfx", "ashfield", "Act 4", "Ash storms in Act 4" },
};

// "horror field 2 20 24 50": patches out from the player, sub-tiles between them, patches created a second, lean ahead in percent
void ConsoleStormField(int which, const char* c) {
	StormConfig&      config = *g_storms[which].config;
	const StormWords& words  = kStormWords[which];
	int               values[4] = { config.fieldRadius, config.fieldSpacing, config.fieldRate, config.fieldLead };
	for (int given = 0; given < 4; ++given) {
		while (*c == ' ' || *c == ',') {
			++c;
		}
		if (*c < '0' || *c > '9') {
			break;
		}
		values[given] = atoi(c);
		while (*c >= '0' && *c <= '9') {
			++c;
		}
	}
	config.fieldRadius  = Clamp(values[0], 0, kMaxFieldRadius);
	config.fieldSpacing = Clamp(values[1], 5, 60);
	config.fieldRate    = Clamp(values[2], 1, 100);
	config.fieldLead    = Clamp(values[3], 0, 100);
	char value[16];
	snprintf(value, sizeof(value), "%d", config.fieldRadius);
	SaveConfigValue(words.section, "field_radius", value);
	snprintf(value, sizeof(value), "%d", config.fieldSpacing);
	SaveConfigValue(words.section, "field_spacing", value);
	snprintf(value, sizeof(value), "%d", config.fieldRate);
	SaveConfigValue(words.section, "field_rate", value);
	snprintf(value, sizeof(value), "%d", config.fieldLead);
	SaveConfigValue(words.section, "field_lead", value);
	const int side = 2 * config.fieldRadius + 1;
	char      text[512];
	snprintf(text, sizeof(text), "Horrors in the Night: blowing %s is %d by %d patches of each effect, %d sub-tiles apart (about %d sub-tiles across), created %d a second, leaning %d%% ahead when you move. Set with: horror %s <how far out 0-%d> <spacing 5-60> <per second 1-100> <lean 0-100>",
		words.stuff, side, side, config.fieldSpacing, side * config.fieldSpacing, config.fieldRate, config.fieldLead, words.field, kMaxFieldRadius);
	g_ctx->WriteConsoleMessage(text);
}

// "horror fx" lists the effects; "horror fx 2 10 12" sets the storm's layers (0 = none)
void ConsoleStormFx(int which, const char* c) {
	StormKind&        storm  = g_storms[which];
	StormConfig&      config = *storm.config;
	const StormWords& words  = kStormWords[which];
	int               layers[kMaxLayers] = {};
	int               given              = 0;
	char              text[512];
	while (given < storm.layers) {
		while (*c == ' ' || *c == ',') {
			++c;
		}
		if (*c < '0' || *c > '9') {
			break;
		}
		layers[given++] = Clamp(atoi(c), 0, storm.effectCount - 1);
		while (*c >= '0' && *c <= '9') {
			++c;
		}
	}
	if (given > 0) {
		for (int i = 0; i < storm.layers; ++i) {
			config.layer[i] = layers[i];
			char key[16], value[16];
			snprintf(key, sizeof(key), "layer%d", i + 1);
			snprintf(value, sizeof(value), "%d", layers[i]);
			SaveConfigValue(words.section, key, value);
		}
		memset(storm.failed, 0, static_cast<size_t>(storm.effectCount) * sizeof(bool));
	} else {
		size_t used = 0;
		text[0]     = '\0';
		for (int i = 1; i < storm.effectCount; ++i) {
			used += static_cast<size_t>(snprintf(text + used, sizeof(text) - used, "%s%d %s", used > 0 ? ", " : "Effects: ", i, storm.effects[i].name));
			if (used > 150 || i == storm.effectCount - 1) {
				g_ctx->WriteConsoleMessage(text);
				used    = 0;
				text[0] = '\0';
			}
		}
	}
	size_t used = static_cast<size_t>(snprintf(text, sizeof(text), "Horrors in the Night: %s layers are", storm.name));
	for (int i = 0; i < storm.layers && used < sizeof(text) - 80; ++i) {
		used += static_cast<size_t>(snprintf(text + used, sizeof(text) - used, "%s %d (%s)", i > 0 ? "," : "", config.layer[i],
			storm.effects[Clamp(config.layer[i], 0, storm.effectCount - 1)].name));
	}
	snprintf(text + used, sizeof(text) - used, ". Set them with: horror %s <first> [second] ... (up to %d numbers, 0 = none)", words.fx, storm.layers);
	g_ctx->WriteConsoleMessage(text);
}

// "horror sand" / "horror ash": starts or stops a storm that comes and goes
void ConsoleStormToggle(int which) {
	StormKind&        storm = g_storms[which];
	const StormWords& words = kStormWords[which];
	char              text[512];
	if (!StormAllowed(storm)) {
		if (storm.config->enabled == 0) {
			snprintf(text, sizeof(text), "Horrors in the Night: the %s is switched off in the plugin's settings.", storm.name);
		} else if (storm.installed->load() == 0) {
			snprintf(text, sizeof(text), "Horrors in the Night: no %s look installed - turn on %s (and Always night) in D2RMM and Install.", storm.name, words.modOption);
		} else {
			snprintf(text, sizeof(text), "Horrors in the Night: the %s is not available with this game version.", storm.name);
		}
	} else if (storm.config->always != 0) {
		snprintf(text, sizeof(text), "Horrors in the Night: the %s is set to never stop (%s always). Turn that off to have storms come and go, and to start and stop them here.", storm.name,
			words.section);
	} else {
		storm.state.force = true;
		snprintf(text, sizeof(text), "Horrors in the Night: %s started or stopped (seen in %s).", storm.name, words.where);
	}
	g_ctx->WriteConsoleMessage(text);
}

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	const char* args = command != nullptr && command->args != nullptr ? command->args : "";
	while (*args == ' ') {
		++args;
	}
	char text[512];
	if (strncmp(args, "spawn", 5) == 0) {
		g_forceSpawn = true;
		g_ctx->WriteConsoleMessage("Horrors in the Night: a phantom appears on the next game frame (not in town).");
	} else if (strncmp(args, "test", 4) == 0) {
		const int row = atoi(args + 4);
		g_testRow     = row;
		snprintf(text, sizeof(text), "Horrors in the Night: firing missiles row %d near you (not in town).", row);
		g_ctx->WriteConsoleMessage(text);
	} else if (strncmp(args, "haunt", 5) == 0) {
		g_forceHaunt = true;
		g_ctx->WriteConsoleMessage("Horrors in the Night: a haunting starts on the next game frame (not in town).");
	} else if (strncmp(args, "strike", 6) == 0) {
		g_forceStrike = true;
		g_ctx->WriteConsoleMessage("Horrors in the Night: lightning strike.");
	} else if (strncmp(args, "meteor", 6) == 0) {
		g_forceMeteor = true;
		g_ctx->WriteConsoleMessage("Horrors in the Night: a volley of meteors falls around you on the next game frames (not in town).");
	} else if (strncmp(args, "probe", 5) == 0) {
		// "horror probe" lists the effects; "horror probe 27" has the game create one on you;
		// "horror probe all" goes through the list; "horror probe stop" ends it
		const char* c = args + 5;
		while (*c == ' ') {
			++c;
		}
		if (!g_weatherReady) {
			g_ctx->WriteConsoleMessage("Horrors in the Night: probing effects is not available with this game version.");
		} else if (strncmp(c, "all", 3) == 0) {
			g_probeRequest = kProbeAskAll;
			snprintf(text, sizeof(text), "Horrors in the Night: trying all %d effects on you, %d seconds each (in a game). Each outcome is said here and in horror-night.log. Stop with: horror probe stop",
				kProbeCount - 1, static_cast<int>(kProbeShowMs / 1000));
			g_ctx->WriteConsoleMessage(text);
		} else if (strncmp(c, "stop", 4) == 0) {
			g_probeRequest = kProbeAskStop;
			g_ctx->WriteConsoleMessage("Horrors in the Night: probing stopped.");
		} else if (*c >= '0' && *c <= '9') {
			const int effect = Clamp(atoi(c), 1, kProbeCount - 1);
			g_probeRequest   = effect;
			snprintf(text, sizeof(text), "Horrors in the Night: trying effect %d (%s) on you (in a game).", effect, kProbeEffects[effect].name);
			g_ctx->WriteConsoleMessage(text);
		} else {
			size_t used = 0;
			text[0]     = '\0';
			for (int i = 1; i < kProbeCount; ++i) {
				used += static_cast<size_t>(snprintf(text + used, sizeof(text) - used, "%s%d %s", used > 0 ? ", " : "Effects: ", i, kProbeEffects[i].name));
				if (used > 150 || i == kProbeCount - 1) {
					g_ctx->WriteConsoleMessage(text);
					used    = 0;
					text[0] = '\0';
				}
			}
			g_ctx->WriteConsoleMessage("Horrors in the Night: horror probe <number> has the game create that effect on you for a few seconds; horror probe all tries every one in turn.");
		}
	} else if (strncmp(args, "ashfield", 8) == 0) {
		ConsoleStormField(kStormAsh, args + 8);
	} else if (strncmp(args, "ashfx", 5) == 0) {
		ConsoleStormFx(kStormAsh, args + 5);
	} else if (strncmp(args, "ash", 3) == 0) {
		ConsoleStormToggle(kStormAsh);
	} else if (strncmp(args, "field", 5) == 0) {
		ConsoleStormField(kStormSand, args + 5);
	} else if (strncmp(args, "fx", 2) == 0) {
		ConsoleStormFx(kStormSand, args + 2);
	} else if (strncmp(args, "sand", 4) == 0) {
		ConsoleStormToggle(kStormSand);
	} else {
		snprintf(text, sizeof(text), "Horrors in the Night: phantoms %s%s (alive %d, spawned %d, hits %d, rows %d/%d/%d, lightning row %d); storm %s, sandstorm %s (%d%%), ash storm %s (%d%%, %d meteors, %d burns), %d field patches up, level %d. Commands: horror spawn | haunt | strike | sand | fx | field | ash | ashfx | ashfield | meteor | probe | status",
			g_serverReady.load() ? "on" : "off", g_diabloDead.load() ? ", stopped: Diablo is dead" : "", g_aliveCount.load(), g_totalSpawned.load(), g_totalDrains.load(), g_classes[1].count, g_classes[2].count,
			g_classes[3].count, g_lightningRow[3], g_overlay != nullptr ? "on" : "off", StormAllowed(g_storms[kStormSand]) ? "available" : "off", Sand().shown.load(), StormAllowed(g_storms[kStormAsh]) ? "available" : "off",
			g_storms[kStormAsh].state.shown.load(), g_totalMeteors.load(), g_totalBurns.load(), g_patchesUp.load(), g_level.load());
		g_ctx->WriteConsoleMessage(text);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "horror-night",
	.name        = "Horrors in the Night",
	.version     = "1.2.0",
	.author      = "Latedozer",
	.description = "Phantom monsters that cannot be killed and vanish on a timer, lightning and thunder, sandstorms in the Act 2 desert, and ash storms with meteors in Act 4.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
	.reserved    = {},
};

}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr || context->exeBase == 0) {
		return false;
	}
	g_ctx  = context;
	g_base = context->exeBase;
	StartLogThread();

	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	SeedRandom(static_cast<uint64_t>(counter.QuadPart));

	Log("Horrors in the Night %s loading (D2R %s, SDK %s)", kPluginInfo.version, context->buildVersion != nullptr ? context->buildVersion : "?", D2RL_SDK_VERSION);
	LoadConfig();
	DefaultLevelInfo();
	SetUpServer();
	SetUpStorm();
	static_cast<void>(context->RegisterConsoleCommand("horror", OnConsole, "Horrors in the Night: status, spawn, haunt, strike, sand, fx, field, ash, ashfx, ashfield, meteor, probe"));
	JoinSettingsMenu();
	if (!g_menuJoined) {
		Log("settings menu: I Hate TOMLs not available at load (will try again when a game starts)");
	}
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_serverReady = false;
	g_eventYield  = 0.0f;
	if (g_skyHolding) {
		SkyWrite(5, 0.0f, 0);
		g_skyHolding = false;
	}
	ReleaseClientExtras();
	if (g_soundLoader != nullptr) {
		WaitForSingleObject(g_soundLoader, 5000); // it must not outlive the DLL
		CloseHandle(g_soundLoader);
		g_soundLoader = nullptr;
	}
	StopMixer();
	StopLogThread(); // last: everything above may still log
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_module = instance;
	}
	return TRUE;
}
