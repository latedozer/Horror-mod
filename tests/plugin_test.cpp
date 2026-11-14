// Tests for the parts of the plugin that do not need the game: the config file, the level
// table, the sound mixer, the sandstorm clock and the hold on the game's time of day.
// The whole plugin is included as one translation unit, so its internals are reachable here.
// Built for Windows like the plugin and run under Wine by tests/run.sh.
#include "../plugin/horror_night.cpp"

#include <cstdio>
#include <vector>

// the sound loader is not linked into the test
void* OpenGameStorage(const wchar_t*, char*, int) { return nullptr; }
void  CloseGameStorage(void*) {}
int16_t* LoadGameSound(void*, const char*, const SoundProcess&, uint32_t*) { return nullptr; }

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
	if (!ok) {
		++g_failures;
	}
	printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

bool Near(float a, float b, float tolerance = 0.02f) {
	return a > b - tolerance && a < b + tolerance;
}

int Lines(const char* text) {
	int count = 0;
	for (const char* c = text; *c != '\0'; ++c) {
		count += *c == '\n' ? 1 : 0;
	}
	return count;
}

// ---- config ------------------------------------------------------------------------------

void TestConfig(const char* tomlPath) {
	printf("-- config\n");
	static char shipped[32768];
	FILE*       file = fopen(tomlPath, "rb");
	const size_t size = file != nullptr ? fread(shipped, 1, sizeof(shipped) - 1, file) : 0;
	if (file != nullptr) {
		fclose(file);
	}
	shipped[size] = '\0';
	Check(size > 0, "the shipped horror-night.toml can be read");

	// every setting has its line in the shipped file: rewriting it adds no line
	static char result[40000];
	bool        allPresent = true;
	for (const Setting& setting : kSettings) {
		if (setting.section == nullptr) {
			continue;
		}
		RewriteConfig(shipped, setting.section, setting.key, "1", result, sizeof(result));
		if (Lines(result) != Lines(shipped)) {
			allPresent = false;
			printf("      missing from the shipped file: [%s] %s\n", setting.section, setting.key);
		}
	}
	Check(allPresent, "every setting is in the shipped horror-night.toml");

	// ...and the shipped values are the built-in defaults
	static char copy[32768];
	memcpy(copy, shipped, sizeof(copy));
	g_config = Config {};
	for (const Setting& setting : kSettings) {
		if (setting.target != nullptr) {
			*setting.target = setting.isBool ? 1 - *setting.target : setting.min; // anything but the default
		}
	}
	ParseConfig(copy);
	const Config defaults {};
	Check(memcmp(&g_config, &defaults, sizeof(Config)) == 0, "the shipped file holds the built-in defaults");

	char text[] = "[phantoms]\nenabled = false\nmin_interval = 99999\nmax_interval = 7\nold_setting = 3\n\n[sandstorm]\n  fade=0 # too short\nalways = true\n[nonsense]\nfade = 30\n";
	g_config    = Config {};
	ParseConfig(text);
	Check(g_config.phantomsEnabled == 0, "false switches a setting off");
	Check(g_config.minInterval == 3600, "a value above its range is brought down to the top of it");
	Check(g_config.maxInterval == 3600, "the longest of a pair is never below the shortest");
	Check(g_config.sandFade == 1 && g_config.sandAlways == 1, "spaces, no spaces and a trailing comment all read; a value below its range is raised");
	Check(g_config.minLife == defaults.minLife && g_config.sandCalmMin == defaults.sandCalmMin, "settings the file does not mention keep their defaults");

	// one line replaced, the rest untouched
	const char* before = "# top\r\n[phantoms]\r\nenabled = true\r\nmin_interval = 40\r\n\r\n[storm]\r\nenabled = true\r\n";
	RewriteConfig(before, "storm", "enabled", "false", result, sizeof(result));
	Check(strcmp(result, "# top\r\n[phantoms]\r\nenabled = true\r\nmin_interval = 40\r\n\r\n[storm]\r\nenabled = false\r\n") == 0, "a value is replaced in its own section only");
	RewriteConfig(before, "phantoms", "max_life", "9", result, sizeof(result));
	Check(strstr(result, "max_life = 9\r\n[storm]") != nullptr && Lines(result) == Lines(before) + 1, "a missing key is added to its section");
	RewriteConfig(before, "sandstorm", "fade", "12", result, sizeof(result));
	Check(strstr(result, "[storm]\r\nenabled = true\r\n\r\n[sandstorm]\r\nfade = 12\r\n") != nullptr, "a missing section is added at the end");
	RewriteConfig("[storm]\nenabled = true", "storm", "volume", "80", result, sizeof(result));
	Check(strcmp(result, "[storm]\nenabled = true\r\nvolume = 80\r\n") == 0, "a file with no last line end does not get two settings on one line");
	RewriteConfig("[storm]\nmin_interval2 = 4\nmin_interval = 5\n", "storm", "min_interval", "9", result, sizeof(result));
	Check(strcmp(result, "[storm]\nmin_interval2 = 4\nmin_interval = 9\n") == 0, "a key that only starts the same is left alone");
	g_config = Config {};
}

// ---- level table -------------------------------------------------------------------------

void TestLevels() {
	printf("-- level table\n");
	// an older mod: three columns, no options line
	char older[] = "level\tnormal\tnightmare_hell\r\n2\t5,6\t7\r\n41\t\t30,31,32\r\n";
	Check(ParseLevels(older) == 2, "an older levels.tsv is read");
	Check(g_levelMonsters[0][2].count == 2 && g_levelMonsters[0][2].bases[1] == 6 && g_levelMonsters[1][2].bases[0] == 7, "...with its monsters");
	Check(g_levelMonsters[0][41].count == 0 && g_levelMonsters[1][41].count == 3, "...and an empty list stays empty");
	Check(!g_levelTableFromMod && g_modKnown.load() == 0, "...and it says nothing about weather or the mod's options");
	Check(LevelFall(2) == kFallRain && LevelFall(136) == kFallRain && LevelFall(137) == kFallSnow && LevelFall(117) == kFallSnow, "built-in lists: rain and snow, with Uber Tristram and the Colossal Summit");
	Check(LevelFall(8) == kFallNone && LevelUnder(8) == kFallRain && LevelUnder(113) == kFallSnow && LevelUnder(2) == kFallNone, "built-in lists: what is overhead underground");
	Check(IsTown(1) && IsTown(40) && IsTown(75) && IsTown(103) && IsTown(109) && !IsTown(2) && !IsTown(0) && !IsTown(9999), "built-in lists: the five towns");

	char newer[] = "#mod\talwaysNight=1\tskyFlash=0\tsandstorm=1\r\nlevel\tnormal\tnightmare_hell\tweather\ttown\tunder\r\n"
		"1\t\t\t1\t1\t0\r\n2\t5,6\t7\t1\t0\t0\r\n8\t\t\t0\t0\t1\r\n40\t\t\t0\t1\t3\r\n41\t9\t9\t3\t0\t0\r\n55\t\t\t0\t0\t3\r\n200\t\t\t2\t1\t0\r\n";
	Check(ParseLevels(newer) == 7, "a current levels.tsv is read");
	Check(g_levelTableFromMod && g_modKnown.load() == 1 && g_modSkyFlash.load() == 0 && g_modSandstorm.load() == 1, "...with the mod's options");
	Check(LevelFall(41) == kFallSand && LevelUnder(40) == kFallSand && IsTown(40) && LevelUnder(55) == kFallSand, "...and the desert, its town and its tombs");
	Check(LevelFall(200) == kFallSnow && IsTown(200), "...and a level no vanilla list knows");
	Check(LevelFall(136) == kFallNone && !IsTown(75), "the mod's table replaces the built-in lists, it does not add to them");
	Check(g_levelMonsters[0][2].count == 2 && g_levelMonsters[1][41].bases[0] == 9, "...and the monsters are still read");

	g_config = Config {};
	Check(StormArea(2) == Area::Outdoor && StormArea(8) == Area::Indoor && StormArea(41) == Area::None && StormArea(40) == Area::None, "lightning: outdoors in the rain, muffled underground, none in the desert");
	Check(StormArea(200) == Area::Outdoor, "lightning in the snow");
	g_config.snowAreas     = 0;
	g_config.indoorThunder = 0;
	Check(StormArea(200) == Area::None && StormArea(8) == Area::None && StormArea(2) == Area::Outdoor, "...unless those are switched off");
	g_config = Config {};

	// more monsters than a level can hold
	static char many[2048];
	size_t      used = static_cast<size_t>(snprintf(many, sizeof(many), "3\t"));
	for (int i = 0; i < kMaxPerLevel + 20; ++i) {
		used += static_cast<size_t>(snprintf(many + used, sizeof(many) - used, "%d,", i + 1));
	}
	snprintf(many + used, sizeof(many) - used, "\t1\r\n");
	ParseLevels(many);
	Check(g_levelMonsters[0][3].count == kMaxPerLevel && g_levelMonsters[1][3].count == 1, "a level with too many monsters is cut at the limit, the next column still reads");
}

// ---- mixer -------------------------------------------------------------------------------

void TestMixer() {
	printf("-- mixer\n");
	static int16_t flat[64000];
	for (int16_t& sample : flat) {
		sample = 16384; // half scale
	}
	const Clip shortClip { flat, 16000, kMixRate };
	const Clip longClip { flat, 64000, kMixRate };
	const auto step = [](const Clip& clip) { return (static_cast<uint64_t>(clip.rate) << 32) / kMixRate; };

	int16_t out[kMixBlock];
	Voice   voices[kMaxVoices];
	int     count = 0;
	voices[count++] = Voice { &shortClip, 0, step(shortClip), 0.5f, false };
	MixBlock(voices, count, 0.0f, 0.0f, out);
	Check(out[0] > 8150 && out[0] < 8230 && out[kMixBlock - 1] == out[0] && count == 1, "a sound plays at its gain");
	int blocks = 1;
	while (count > 0 && blocks < 100) {
		MixBlock(voices, count, 0.0f, 0.0f, out);
		++blocks;
	}
	Check(blocks == 16 && out[kMixBlock - 1] == 0, "...and is dropped when it ends (16 buffers for half a second)");

	// a gust fades in, holds, and follows the wind's loudness
	voices[count++] = Voice { &longClip, 0, step(longClip), 1.0f, true };
	MixBlock(voices, count, 1.0f, 1.0f, out);
	Check(out[0] == 0 && out[kMixBlock - 1] > 1200 && out[kMixBlock - 1] < 1400, "a gust fades in from silence");
	for (int i = 0; i < 20; ++i) {
		MixBlock(voices, count, 1.0f, 1.0f, out);
	}
	Check(out[0] > 16300 && out[kMixBlock - 1] > 16300, "...to full strength");
	MixBlock(voices, count, 1.0f, 0.0f, out);
	Check(out[0] > 16300 && out[kMixBlock / 2] > 8000 && out[kMixBlock / 2] < 8400 && out[kMixBlock - 1] < 40, "...and gets quieter within a buffer as the wind drops");
	MixBlock(voices, count, 0.0f, 0.0f, out);
	Check(out[0] == 0 && out[kMixBlock - 1] == 0 && count == 1, "...down to silence, still running");
	count = 0;

	// the bed of gusts
	for (Clip& clip : g_wind) {
		clip = longClip;
	}
	int32_t until = 0;
	voices[count++] = Voice { &shortClip, 0, step(shortClip), 0.5f, false };
	WindGusts(voices, count, true, false, until);
	Check(count == 2 && voices[1].wind && until >= 35200 && until <= 51200, "wind wanted: a gust starts, the next is due before it ends");
	const int32_t first = until;
	WindGusts(voices, count, true, true, until);
	Check(count == 2 && until == first - static_cast<int32_t>(kMixBlock), "...and nothing more until then");
	until = 1;
	WindGusts(voices, count, true, true, until);
	Check(count == 3 && voices[2].wind, "...then a second gust overlaps the first");
	WindGusts(voices, count, false, true, until);
	Check(count == 3 && until == 0, "wind no longer wanted: the gusts play on while it fades");
	WindGusts(voices, count, false, false, until);
	Check(count == 1 && !voices[0].wind, "...and are dropped once it is silent; other sounds are not touched");
	for (Clip& clip : g_wind) {
		clip = Clip {};
	}
	count = 0;
	until = 0;
	WindGusts(voices, count, true, false, until);
	Check(count == 0 && until == static_cast<int32_t>(kMixRate), "no wind sounds read: nothing plays, and it looks again later");
}

// ---- sandstorm clock ---------------------------------------------------------------------

// the defaults, with storms that come and go (shipped: the storm never stops)
Config Clocked() {
	Config config {};
	config.sandAlways = 0;
	return config;
}

void TestSandstorm() {
	printf("-- sandstorm\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\r\n2\t\t\t1\t0\t0\r\n40\t\t\t0\t1\t3\r\n41\t\t\t3\t0\t0\r\n55\t\t\t0\t0\t3\r\n";
	ParseLevels(table);
	g_config   = Clocked();
	g_skyReady = true;
	uint64_t   now = 1000000;
	const auto run = [&](int level, int seconds10) { // tenths of a second
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateSandstorm(now, level);
		}
	};

	g_modSandstorm = 0;
	g_forceSand    = true;
	run(41, 20);
	Check(!g_sandActive && g_sandLevel == 0.0f && SandLayers(41) == -1, "no storm look installed by the mod: nothing happens, even when asked");
	g_modSandstorm = 1;
	g_forceSand    = false;

	run(41, 10);
	Check(!g_sandActive && g_sandChange > now && g_sandChange <= now + 240000, "it starts calm, with the first storm on the clock");
	Check(SandLayers(41) == 0, "...and no blowing sand yet");
	g_forceSand = true;
	run(41, 40);
	Check(g_sandActive && Near(g_sandLevel, 0.5f, 0.03f), "a storm rolls in: half way after half the roll-in time");
	Check(SandLayers(41) == 2 && Near(SandBlend(), 0.5f, 0.03f), "...with two of the three sand layers up and the storm look in step");
	run(41, 45);
	Check(g_sandLevel == 1.0f && SandLayers(41) == 3 && SandBlend() == 1.0f && Near(g_windLevel, 1.0f), "...to full strength, all layers up");
	g_config.sandStrength = 50;
	Check(Near(SandBlend(), 0.5f) && SandLayers(41) == 3, "the strength setting scales the look");
	g_config.sandLayer2 = 0;
	g_config.sandLayer3 = 0;
	Check(SandLayers(41) == 1, "one layer configured: one layer");
	g_config.sandLayer1 = 0;
	Check(SandLayers(41) == -1, "no layer configured leaves the weather alone");
	g_config          = Clocked();
	g_config.sandDust = 0;
	Check(SandLayers(41) == -1, "...and so does blowing sand set to 0");
	g_config = Clocked();

	run(40, 10);
	Check(g_sandActive && g_sandLevel < 0.95f && g_sandLevel > 0.8f, "walking into Lut Gholein: the storm thins out, it is not cut off");
	Check(g_windLevel < 0.95f && g_windLevel > 0.3f, "...and the wind drops towards muffled");
	run(40, 100);
	Check(g_sandLevel == 0.0f && Near(g_windLevel, 0.3f), "...until the town is clear, with the wind still heard outside");
	g_config.sandIndoorWind = 0;
	run(40, 30);
	Check(g_windLevel == 0.0f, "...or not, with the muffled wind switched off");
	g_config = Clocked();

	run(41, 100);
	Check(g_sandLevel == 1.0f, "back out in the desert the same storm rolls in again");
	run(55, 1);
	Check(g_sandLevel == 0.0f && SandLayers(55) == -1, "in a tomb the storm look is gone at once");
	run(2, 1);
	Check(g_sandLevel == 0.0f && g_sandActive, "in another act too, while the storm goes on by its clock");
	run(-1, 1);
	Check(!g_sandActive && g_sandChange == 0 && g_windLevel == 0.0f, "leaving the game ends it");

	// the clock itself
	g_config.sandCalmMin = g_config.sandCalmMax = 5;
	g_config.sandLengthMin = g_config.sandLengthMax = 6;
	run(41, 45);
	Check(!g_sandActive, "calm for the calm time");
	run(41, 10);
	Check(g_sandActive, "...then a storm");
	run(41, 55);
	Check(g_sandActive, "...for its length");
	run(41, 10);
	Check(!g_sandActive, "...then calm again");
	g_config.sandAlways = 1;
	run(41, 200);
	Check(g_sandActive && g_sandLevel == 1.0f && g_sandChange == 0, "never-ending storm: it stays");
	g_config.sandAlways = 0;
	run(41, 1);
	Check(!g_sandActive && g_sandChange > now, "...and goes back on the clock when that is switched off");
	g_config.sandEnabled = 0;
	run(41, 1);
	Check(g_sandLevel == 0.0f && SandLayers(41) == -1, "sandstorms switched off: none");
	g_config   = Clocked();
	g_skyReady = false;
}

// ---- time of day hold --------------------------------------------------------------------

void TestSky() {
	printf("-- time of day hold\n");
	// a stand-in for the game image and its renderer object
	auto* image    = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRendererPointer + 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	auto* renderer = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x12000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	if (image == nullptr || renderer == nullptr) {
		Check(false, "memory for the stand-in renderer");
		return;
	}
	memcpy(image + kRendererPointer, &renderer, sizeof(renderer));
	const auto stage     = [&] { return *reinterpret_cast<int32_t*>(renderer + kStageOffset); };
	const auto blend     = [&] { return *reinterpret_cast<float*>(renderer + kBlendOffset); };
	const auto held      = [&] { return renderer[kOverrideOffset] == 1; };
	const auto gameClock = [&] { // what the game itself would have there
		*reinterpret_cast<int32_t*>(renderer + kStageOffset) = 2;
		*reinterpret_cast<float*>(renderer + kBlendOffset)   = 0.25f;
	};
	const uintptr_t savedBase = g_base;
	g_base                    = reinterpret_cast<uintptr_t>(image);
	g_skyReady                = true;
	g_config                  = Config {};
	const uint64_t now        = GetTickCount64();

	// an older mod (no options line): as before, the hold follows the plugin's own setting
	g_modKnown = 0;
	gameClock();
	UpdateSky(now, true, 0.0f);
	Check(held() && stage() == 5 && blend() == 0.0f, "older mod, sky flash on: held on night");
	g_config.skyFlash = 0;
	gameClock();
	UpdateSky(now, true, 0.0f);
	Check(!held() && stage() == 2, "older mod, sky flash switched off in game: the clock is given back");
	g_config.skyFlash = 1;
	UpdateSky(now, true, 0.0f);
	Check(held() && stage() == 5, "...and taken again when it is switched back on");

	// a current mod says what it installed
	g_modKnown     = 1;
	g_modSkyFlash  = 0;
	g_modSandstorm = 0;
	gameClock();
	UpdateSky(now, true, 0.0f);
	Check(!held() && stage() == 2, "mod without Always night profiles: the day and night cycle is left alone");
	g_modSkyFlash     = 1;
	g_config.skyFlash = 0;
	UpdateSky(now, true, 0.0f);
	Check(held() && stage() == 5, "mod with the lightning profile: held on night even with the plugin's flash off (no blinding daytime)");
	g_modSkyFlash  = 0;
	g_modSandstorm = 1;
	UpdateSky(now, true, 0.0f);
	Check(held() && stage() == 5 && blend() == 0.0f, "mod with the sandstorm profile only: held on night");
	UpdateSky(now, true, 0.5f);
	Check(held() && stage() == 1 && Near(blend(), 0.5f), "half a sandstorm: half way to the storm profile");
	UpdateSky(now, true, 1.4f);
	Check(stage() == 1 && blend() == 1.0f, "never past the storm profile");
	UpdateSky(now, true, 0.0f);
	Check(stage() == 5 && blend() == 0.0f, "storm over: night again");

	// lightning and sand share the blend: the brighter one shows
	g_modSkyFlash     = 1;
	g_config.skyFlash = 1;
	StartSkyFlash(now);
	UpdateSky(now + 10, true, 0.3f);
	Check(stage() == 1 && blend() == 1.0f, "a lightning flash outshines the storm");
	UpdateSky(now + 60000, true, 0.3f);
	Check(stage() == 1 && Near(blend(), 0.3f), "...and the storm is still there after it");

	gameClock();
	UpdateSky(now, false, 0.0f);
	Check(!held() && stage() == 2, "out of the game: the clock is given back");

	g_base     = savedBase;
	g_skyReady = false;
	g_config   = Config {};
	VirtualFree(image, 0, MEM_RELEASE);
	VirtualFree(renderer, 0, MEM_RELEASE);
}

// ---- weather driver and the sand field ------------------------------------------------------
// A stand-in for the game that follows what the disassembly of the real one does:
//   weather object: update starts an effect when none is running (the "starting" byte, then a
//     job that creates it and leaves its handle), stop destroys the entity whose handle is stored
//   entity world: a handle table, transform components found through pages, a queue of jobs
//     ({destroy, invoke, clone} vtable) that run later, and "create an effect at a place"

constexpr uint32_t kFakeEntities = 16384;

struct FakeEffect {
	char  path[200];
	float position[3];
	bool  fromWeather;
};

struct FakeGame {
	uint8_t*  object = nullptr; // the weather object
	uint32_t* table  = nullptr; // entity handle table
	float (*transforms)[52] = nullptr;
	uint32_t  nextIndex  = 5;
	bool      jobPending = false;
	char      jobPath[200] = "";
	float     lastIntensity = -1.0f;
	int       updates = 0;
	std::vector<uint32_t>   stopped;
	std::vector<FakeEffect> effects;         // by table index - 5
	std::vector<Job>        queue;           // jobs waiting in the entity world
	const char*             refuse = nullptr; // an effect the game cannot create at a place
	const char*             refuseWeather = nullptr; // ...or as the weather
	float                   player[3] = { 0, 0, 0 };

	uint32_t& Handle() { return *reinterpret_cast<uint32_t*>(object + 0x64); }
	int32_t&  Type() { return *reinterpret_cast<int32_t*>(object + 0x68); }
	uint8_t&  Starting() { return object[0x60]; }
	bool      Alive(uint32_t handle) const { return handle != 0xfffff && (handle & 0xfffff) < kFakeEntities && table[handle & 0xfffff] == handle; }
	const FakeEffect& Effect(uint32_t handle) const { return effects[(handle & 0xfffff) - 5]; }
	// patches of an effect (not counting one the weather made)
	int       AliveCount(const char* path) const {
		int count = 0;
		for (uint32_t index = 5; index < nextIndex; ++index) {
			count += table[index] != 0 && !effects[index - 5].fromWeather && strcmp(effects[index - 5].path, path) == 0 ? 1 : 0;
		}
		return count;
	}

	uint32_t Create(const char* path, float x, float y, float z, bool fromWeather) {
		const uint32_t handle = (3u << 20) | nextIndex;
		FakeEffect effect {};
		snprintf(effect.path, sizeof(effect.path), "%s", path);
		effect.position[0] = x;
		effect.position[1] = y;
		effect.position[2] = z;
		effect.fromWeather = fromWeather;
		effects.push_back(effect);
		transforms[nextIndex][28] = x; // the world matrix's translation, at +0x70
		transforms[nextIndex][29] = y;
		transforms[nextIndex][30] = z;
		table[nextIndex++] = handle;
		return handle;
	}
	void Stop() {
		if (Alive(Handle())) {
			stopped.push_back(Handle());
			table[Handle() & 0xfffff] = 0;
		}
		Handle() = 0xfffff;
		Type()   = -1;
	}
	void Start(int type) {
		if (Starting() != 0) {
			return;
		}
		if (Alive(Handle())) {
			Stop();
		}
		const auto* slot = reinterpret_cast<const uintptr_t*>(object + 0x70 + type * 0xb8);
		if (type < 0 || type >= 2 || slot[1] == 0) {
			return;
		}
		Starting() = 1;
		jobPending = true;
		snprintf(jobPath, sizeof(jobPath), "%s", reinterpret_cast<const char*>(slot[0]));
		Type() = type;
	}
	void Update(int type, float intensity) {
		++updates;
		lastIntensity = intensity;
		if (type != Type() && Type() != -1) {
			Stop();
		}
		if (!Alive(Handle())) {
			Start(type);
		}
	}
	// the weather's queued job runs: its effect exists, on the player. A file that cannot be
	// made into an effect never does, and the real job then leaves the "starting" byte set.
	void RunJob() {
		if (!jobPending) {
			return;
		}
		jobPending = false;
		if (refuseWeather != nullptr && strcmp(jobPath, refuseWeather) == 0) {
			return;
		}
		Handle()   = Create(jobPath, player[0], player[1], player[2], true);
		Starting() = 0;
	}
	// the game keeps its weather effect on the player
	void MovePlayer(float x, float y, float z) {
		player[0] = x;
		player[1] = y;
		player[2] = z;
		if (Alive(Handle())) {
			const uint32_t index = Handle() & 0xfffff;
			transforms[index][28] = x;
			transforms[index][29] = y;
			transforms[index][30] = z;
		}
	}
	// the entity world reaches the point where it runs queued jobs
	void RunQueue() {
		std::vector<Job> jobs;
		jobs.swap(queue);
		for (Job& job : jobs) {
			using InvokeFn  = void (*)(void*, void*, void*);
			using DestroyFn = void* (*)(void*, uint32_t);
			reinterpret_cast<InvokeFn>(const_cast<void*>(job.vtable[1]))(&job, nullptr, nullptr);
			reinterpret_cast<DestroyFn>(const_cast<void*>(job.vtable[0]))(&job, 0);
		}
	}
};
FakeGame g_fake;

void FakeUpdate(void*, void* params, uint8_t) {
	g_fake.Update(*static_cast<int32_t*>(params), *reinterpret_cast<float*>(static_cast<char*>(params) + 4));
}
void FakeStop(void*, void*, uint8_t) { g_fake.Stop(); }
void* FakeRenderer() { return &g_fake; }
uint32_t FakeSpawn(void*, const char* path, const float* matrix, uint32_t, const char*, uint8_t) {
	if (g_fake.refuse != nullptr && strcmp(path, g_fake.refuse) == 0) {
		return 0xfffff;
	}
	return g_fake.Create(path, matrix[12], matrix[13], matrix[14], false);
}
// the game copies the job into its queue with the job's own clone, then destroys the original
void FakeQueue(void*, uint32_t, void* job, uint32_t) {
	using CloneFn   = void* (*)(void*, void*);
	using DestroyFn = void* (*)(void*, uint32_t);
	Job*  source = static_cast<Job*>(job);
	Job   copy {};
	reinterpret_cast<CloneFn>(const_cast<void*>(source->vtable[2]))(source, &copy);
	reinterpret_cast<DestroyFn>(const_cast<void*>(source->vtable[0]))(source, 0);
	source->inUse = 0;
	copy.inUse    = 1;
	g_fake.queue.push_back(copy);
}

void PlaceJump(uint8_t* at, void* target) {
	const uint8_t code[] = { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 }; // mov rax, target; jmp rax
	memcpy(at, code, sizeof(code));
	memcpy(at + 2, &target, sizeof(target));
}

int PatchCount(int state) {
	int count = 0;
	for (const Patch& patch : g_patches) {
		count += patch.state.load() == state ? 1 : 0;
	}
	return count;
}

// what the field checks start from: a small field, kept centred on the player (its lean
// ahead has its own checks), whatever the shipped settings are
Config Still() {
	Config config {};
	config.sandAlways       = 0;
	config.sandFieldRadius  = 1;
	config.sandFieldSpacing = 20;
	config.sandFieldRate    = 24;
	config.sandFieldLead    = 0;
	return config;
}

void TestWeather() {
	printf("-- weather driver and sand field\n");
	auto* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRendererPointer + 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (image == nullptr) {
		Check(false, "memory for the stand-in game");
		return;
	}
	static uint8_t   object[0x800];
	static uint32_t  table[kFakeEntities];
	static float     transforms[kFakeEntities][52]; // 0xd0 bytes each
	static uint32_t  page[kFakeEntities / 256][256];
	static uintptr_t pages[kFakeEntities / 256];
	static uintptr_t store[8];           // +0x08 / +0x10 pages, +0x38 components
	static uintptr_t stores[10];         // 0x28 bytes each, the store pointer at +0x20
	static uintptr_t registry[8];        // +0x00 stores, +0x30 / +0x38 handle table
	static uint64_t  handler[6];         // one event hub entry: {vtable, function, flag, object}
	static char      rainPath[] = "data/hd/vfx/particles/env/common/weather/Rain.particles";
	static char      snowPath[] = "data/hd/vfx/particles/env/common/weather/Snow.particles";
	memset(object, 0, sizeof(object));
	memset(table, 0, sizeof(table));
	memset(transforms, 0, sizeof(transforms));
	for (uint32_t i = 0; i < kFakeEntities; ++i) {
		page[i / 256][i % 256] = i;
	}
	for (uint32_t i = 0; i < kFakeEntities / 256; ++i) {
		pages[i] = reinterpret_cast<uintptr_t>(page[i]);
	}
	g_fake            = FakeGame {};
	g_fake.object     = object;
	g_fake.table      = table;
	g_fake.transforms = transforms;
	g_fake.Handle()   = 0xfffff;
	g_fake.Type()     = -1;
	object[0x7a0]     = 1; // effects loaded
	const uintptr_t rainString[2] = { reinterpret_cast<uintptr_t>(rainPath), strlen(rainPath) };
	const uintptr_t snowString[2] = { reinterpret_cast<uintptr_t>(snowPath), strlen(snowPath) };
	memcpy(object + 0x70, rainString, sizeof(rainString));
	memcpy(object + 0x70 + 0xb8, snowString, sizeof(snowString));
	store[1]    = reinterpret_cast<uintptr_t>(pages);
	store[2]    = reinterpret_cast<uintptr_t>(pages + kFakeEntities / 256);
	store[7]    = reinterpret_cast<uintptr_t>(transforms);
	stores[5 + 4] = reinterpret_cast<uintptr_t>(store); // transform components are store number 1 here
	registry[0] = reinterpret_cast<uintptr_t>(stores);
	registry[6] = reinterpret_cast<uintptr_t>(table);
	registry[7] = reinterpret_cast<uintptr_t>(table + kFakeEntities);
	const uintptr_t registryAddress = reinterpret_cast<uintptr_t>(registry);
	memcpy(image + kEntityWorld + 0x68, &registryAddress, 8);
	const uint32_t transformType = 1;
	memcpy(image + kTransformType, &transformType, 4);
	handler[1] = reinterpret_cast<uintptr_t>(image) + kWeatherStart;
	handler[3] = reinterpret_cast<uintptr_t>(object);
	const uint64_t list[2] = { reinterpret_cast<uintptr_t>(handler), 1 };
	memcpy(image + kWeatherHub + 0x388, list, sizeof(list));
	PlaceJump(image + kWeatherUpdate, reinterpret_cast<void*>(&FakeUpdate));
	PlaceJump(image + kWeatherStop, reinterpret_cast<void*>(&FakeStop));
	PlaceJump(image + kRenderer, reinterpret_cast<void*>(&FakeRenderer));
	PlaceJump(image + kSpawnEffect, reinterpret_cast<void*>(&FakeSpawn));
	PlaceJump(image + kQueueJob, reinterpret_cast<void*>(&FakeQueue));

	const uintptr_t savedBase = g_base;
	g_base                    = reinterpret_cast<uintptr_t>(image);
	g_weatherReady            = true;
	g_fieldReady              = true;
	g_weatherNext             = 0;
	g_config                  = Still();
	char levels[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\r\n2\t\t\t1\t0\t0\r\n41\t\t\t3\t0\t0\r\n110\t\t\t2\t0\t0\r\n";
	ParseLevels(levels);
	uint64_t   now  = 5000000;
	const auto tick = [&](int level, int sandLayers) {
		now += 1000;
		UpdateWeather(now, level, sandLayers);
	};
	// ticks and lets the game run its queued jobs, until nothing changes
	const auto settle = [&](int level, int sandLayers) {
		for (int i = 0; i < 12; ++i) {
			tick(level, sandLayers);
			g_fake.RunJob();
			g_fake.RunQueue();
		}
	};
	const char* const first  = kSandEffects[g_config.sandLayer1].path;
	const char* const second = kSandEffects[g_config.sandLayer2].path;
	const char* const third  = kSandEffects[g_config.sandLayer3].path;
	const float spacing = static_cast<float>(g_config.sandFieldSpacing) * 2.0f; // world units
	const auto  snowName = [&] { return reinterpret_cast<const char*>(*reinterpret_cast<const uintptr_t*>(object + 0x70 + 0xb8)); };

	// rain, as before
	tick(2, -1);
	Check(object[0x7a1] == 1 && g_fake.jobPending && strcmp(g_fake.jobPath, rainPath) == 0 && g_fake.lastIntensity == 1.0f, "in the rain: the override is taken and rain is started");
	g_fake.RunJob();
	const uint32_t rain = g_fake.Handle();
	tick(2, -1);
	Check(g_weatherOn && g_fake.Alive(rain) && g_fake.queue.empty(), "...and left running");

	// into the desert, calm
	tick(41, 0);
	Check(!g_fake.Alive(rain) && !g_weatherOn && !g_fake.jobPending && g_fake.queue.empty(), "into the calm desert: the rain stops, nothing starts");

	// a storm begins: the marker, then the first effect laid out around the player
	g_fake.MovePlayer(1010.0f, 5.0f, 2030.0f);
	tick(41, 1);
	Check(g_fake.jobPending && strcmp(g_fake.jobPath, first) == 0, "storm begins: the game's weather is started with the snow name pointed at the first sand effect - the storm's marker");
	Check(snowName() == g_snowPath && g_fake.queue.empty(), "...and no sand is laid out before the marker exists");
	tick(41, 1);
	Check(g_fake.updates == 2 && snowName() == g_snowPath, "...nor anything else done while the game is creating it");
	g_fake.RunJob();
	const uint32_t marker = g_fake.Handle();
	Check(strcmp(g_fake.Effect(marker).path, first) == 0, "the marker is a sand effect: no snow was created");
	tick(41, 1);
	Check(snowName() == snowPath, "the snow name is the game's own again as soon as the marker exists");
	Check(g_fake.queue.size() == 6 && PatchCount(kPatchQueued) == 6, "then patches are asked for through the game's own job queue, a few at a time");
	Check(g_fake.AliveCount(first) == 0, "...and nothing exists until the game runs the jobs");
	g_fake.RunQueue();
	Check(g_fake.AliveCount(first) == 6 && PatchCount(kPatchUp) == 6, "the game runs them: the patches exist");
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 9 && g_patchesUp.load() == 9, "a 3 by 3 field of the first effect");
	{
		bool inside = true, level = true;
		bool cells[3][3] = {};
		for (uint32_t index = 5; index < g_fake.nextIndex; ++index) {
			const FakeEffect& effect = g_fake.effects[index - 5];
			if (effect.fromWeather || table[index] == 0) {
				continue;
			}
			const int column = static_cast<int>(std::floor(effect.position[0] / spacing)) - static_cast<int>(std::floor(1010.0f / spacing));
			const int row    = static_cast<int>(std::floor(effect.position[2] / spacing)) - static_cast<int>(std::floor(2030.0f / spacing));
			inside = inside && column >= -1 && column <= 1 && row >= -1 && row <= 1;
			level  = level && effect.position[1] == 5.0f;
			if (inside) {
				cells[column + 1][row + 1] = true;
			}
		}
		bool all = true;
		for (const auto& column : cells) {
			for (const bool cell : column) {
				all = all && cell;
			}
		}
		Check(inside && all, "...one patch in each grid cell around the player, none outside");
		Check(level, "...at the height of the ground the player stands on");
	}
	Check(g_fake.Handle() == marker && g_fake.Alive(marker), "the marker is untouched");
	const int updatesWhenSettled = g_fake.updates;
	const size_t stoppedWhenSettled = g_fake.stopped.size();
	tick(41, 1);
	tick(41, 1);
	Check(g_fake.updates == updatesWhenSettled && g_fake.stopped.size() == stoppedWhenSettled && g_fake.queue.empty(), "a settled storm leaves the game alone");

	// the storm grows
	settle(41, 3);
	Check(g_fake.AliveCount(first) == 9 && g_fake.AliveCount(second) == 9 && g_fake.AliveCount(third) == 9, "storm at its height: all three effects, 27 patches");

	// the player walks: one cell, then another
	g_fake.MovePlayer(1010.0f + spacing, 5.0f, 2030.0f);
	settle(41, 3);
	Check(g_fake.AliveCount(first) == 12 && g_fake.stopped.size() == stoppedWhenSettled, "the player walks one cell: new patches ahead, the ones behind are kept for now");
	g_fake.MovePlayer(1010.0f + 2 * spacing, 5.0f, 2030.0f);
	settle(41, 3);
	Check(g_fake.AliveCount(first) == 12 && g_fake.AliveCount(second) == 12 && g_fake.stopped.size() == stoppedWhenSettled + 9, "another cell: more ahead, and the column two cells behind is removed");
	Check(g_fake.Handle() == marker && g_fake.Alive(marker) && g_fake.Type() == 1, "removing patches leaves the game's own marker as it was");

	// dying down
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 12 && g_fake.AliveCount(second) == 0 && g_fake.AliveCount(third) == 0, "storm dies down: the later effects go, the first stays");

	// settings changed while it blows
	g_config.sandFieldRadius = 0;
	settle(41, 1);
	Check(g_fake.AliveCount(first) <= 9 && g_fake.AliveCount(first) >= 1, "a smaller field: patches beyond it are removed");
	g_config.sandFieldRadius = 2;
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 25, "a larger field: 5 by 5");
	g_config.sandLayer1 = 8;
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 0 && g_fake.AliveCount(kSandEffects[8].path) == 25, "another effect chosen: the old patches go, the new ones come");
	g_config = Still();

	// how fast the field fills in
	settle(41, 0);
	g_config.sandFieldRate = 2;
	tick(41, 1);
	g_fake.RunJob();
	tick(41, 1);
	Check(g_fake.queue.size() == 1, "a low rate: one patch at a time");
	now += 400;
	UpdateWeather(now, 41, 1);
	Check(g_fake.queue.size() == 1, "...none before its time has come (2 a second: not after 0.4 s)");
	now += 100;
	UpdateWeather(now, 41, 1);
	Check(g_fake.queue.size() == 2, "...the next after half a second");
	tick(41, 1);
	tick(41, 1);
	Check(g_fake.queue.size() == 4, "...and never more than one at once, however long the wait");
	g_config.sandFieldRate = 100;
	tick(41, 1);
	Check(g_fake.queue.size() == 9, "a high rate: the rest of the field at once");
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 9, "...and the field is whole either way");
	g_config = Still();
	g_config.sandLayer1 = g_config.sandLayer2 = g_config.sandLayer3 = 4;
	settle(41, 3);
	settle(41, 3);
	Check(g_fake.AliveCount(kSandEffects[4].path) == 27 && g_fake.queue.empty(), "the same effect chosen for all three: three fields of it stacked, for a thicker storm");
	{
		// ...and the copies in one cell are not at the very same spot
		bool apart = true;
		for (uint32_t a = 5; a < g_fake.nextIndex; ++a) {
			for (uint32_t b = a + 1; b < g_fake.nextIndex; ++b) {
				const FakeEffect& one = g_fake.effects[a - 5];
				const FakeEffect& two = g_fake.effects[b - 5];
				if (table[a] != 0 && table[b] != 0 && !one.fromWeather && !two.fromWeather && one.position[0] == two.position[0] && one.position[2] == two.position[2]) {
					apart = false;
				}
			}
		}
		Check(apart, "...each copy at its own spot in the cell");
	}
	settle(41, 1);
	Check(g_fake.AliveCount(kSandEffects[4].path) == 9, "...and they thin out one field at a time as the storm dies down");
	g_config.sandLayer2 = 9;
	settle(41, 3);
	Check(g_fake.AliveCount(kSandEffects[4].path) == 18 && g_fake.AliveCount(kSandEffects[9].path) == 9, "one of three changed: only that field is replaced");
	g_config = Still();

	// a large, dense field while the player runs diagonally (straight up the screen): every cell
	// around the player has its patch after each step, however many are kept behind
	g_config.sandFieldRadius  = 3;
	g_config.sandFieldSpacing = 8;
	g_config.sandFieldRate    = 100;
	{
		const float dense = 16.0f; // world units between patches
		float       x = 1010.0f, z = 2030.0f;
		bool        whole = true;
		settle(41, 3); // the 147 patches of the new field, 25 at a time
		Check(g_fake.AliveCount(first) == 49 && g_fake.AliveCount(second) == 49 && g_fake.AliveCount(third) == 49, "a 7 by 7 field of three effects: 147 patches");
		for (int step = 0; step < 10; ++step) {
			x += dense;
			z += dense;
			g_fake.MovePlayer(x, 5.0f, z);
			for (int i = 0; i < 4; ++i) {
				tick(41, 3);
				g_fake.RunJob();
				g_fake.RunQueue();
			}
			for (int layer = 0; layer < 3; ++layer) {
				const char* const path = layer == 0 ? first : layer == 1 ? second : third;
				bool cells[7][7] = {};
				for (uint32_t index = 5; index < g_fake.nextIndex; ++index) {
					const FakeEffect& effect = g_fake.effects[index - 5];
					if (table[index] == 0 || effect.fromWeather || strcmp(effect.path, path) != 0) {
						continue;
					}
					const int column = static_cast<int>(std::floor(effect.position[0] / dense)) - static_cast<int>(std::floor(x / dense));
					const int row    = static_cast<int>(std::floor(effect.position[2] / dense)) - static_cast<int>(std::floor(z / dense));
					if (column >= -3 && column <= 3 && row >= -3 && row <= 3) {
						cells[column + 3][row + 3] = true;
					}
				}
				for (const auto& column : cells) {
					for (const bool cell : column) {
						whole = whole && cell;
					}
				}
			}
		}
		Check(whole, "a 7 by 7 field of three effects stays whole around a player running diagonally");
		Check(PatchCount(kPatchFree) > 0, "...with room to spare for the patches kept behind");
	}
	g_config = Still();
	settle(41, 0);

	// the nearest patches come first
	g_config.sandFieldRadius = 2;
	g_config.sandFieldRate   = 1;
	g_fake.MovePlayer(1010.0f, 5.0f, 2030.0f);
	tick(41, 1);
	g_fake.RunJob();
	tick(41, 1);
	g_fake.RunQueue();
	{
		const FakeEffect& nearest = g_fake.effects.back();
		Check(!nearest.fromWeather && std::floor(nearest.position[0] / spacing) == std::floor(1010.0f / spacing) && std::floor(nearest.position[2] / spacing) == std::floor(2030.0f / spacing),
			"a field fills in from where the player stands outwards: the first patch is the player's own cell");
	}
	g_config = Still();
	settle(41, 0);

	// leaning ahead of a moving player
	g_config.sandFieldRadius = 2;
	g_config.sandFieldRate   = 100;
	g_config.sandFieldLead   = 50;
	{
		float x = 1010.0f;
		g_fake.MovePlayer(x, 5.0f, 2030.0f);
		settle(41, 1);
		const auto reach = [&](int& behind, int& ahead) {
			behind = 99;
			ahead  = -99;
			for (uint32_t index = 5; index < g_fake.nextIndex; ++index) {
				const FakeEffect& effect = g_fake.effects[index - 5];
				if (table[index] == 0 || effect.fromWeather) {
					continue;
				}
				const int column = static_cast<int>(std::floor(effect.position[0] / spacing)) - static_cast<int>(std::floor(x / spacing));
				behind = column < behind ? column : behind;
				ahead  = column > ahead ? column : ahead;
			}
		};
		int behind = 0, ahead = 0;
		reach(behind, ahead);
		Check(behind == -2 && ahead == 2, "standing still: the field is centred on the player");
		for (int step = 0; step < 8; ++step) { // one cell a second
			x += spacing;
			g_fake.MovePlayer(x, 5.0f, 2030.0f);
			tick(41, 1);
			g_fake.RunJob();
			g_fake.RunQueue();
		}
		reach(behind, ahead);
		Check(ahead == 3 && behind >= -2, "moving: the field reaches further ahead of the player than it does standing still");
		Check(behind <= -1, "...and still covers where the player is and a little behind");
		g_fake.MovePlayer(x + 2000.0f, 5.0f, 2030.0f); // a waypoint
		x += 2000.0f;
		settle(41, 1);
		reach(behind, ahead);
		Check(behind == -2 && ahead == 2, "a jump across the map is not travel: the field is centred on the player again");
	}
	g_config = Still();

	// what the field costs (printed, not checked): the shipped field, standing, running, and stop-and-go
	g_config.sandFieldRadius  = 5;
	g_config.sandFieldSpacing = 17;
	g_config.sandFieldRate    = 30;
	g_config.sandFieldLead    = 50;
	{
		float x = 1010.0f, z = 2030.0f;
		g_fake.MovePlayer(x, 5.0f, z);
		for (int i = 0; i < 6; ++i) {
			settle(41, 3);
		}
		LARGE_INTEGER frequency {}, from {}, to {};
		QueryPerformanceFrequency(&frequency);
		const auto micros = [&] { return static_cast<double>(to.QuadPart - from.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart); };
		const int  patches = g_patchesUp.load();
		QueryPerformanceCounter(&from);
		for (int i = 0; i < 200; ++i) {
			tick(41, 3);
		}
		QueryPerformanceCounter(&to);
		printf("perf  standing: %d patches up, %.0f microseconds of plugin time a second\n", patches, micros() / 200.0);

		// seconds of play in tenths: dx, dz are world units a tenth (1.7 sub-tiles = 3.4 units is a run)
		const auto play = [&](const char* what, int tenths, const auto& velocity) {
			const size_t created = g_fake.effects.size(), stopped = g_fake.stopped.size();
			long long    alive = 0;
			int          most  = 0;
			QueryPerformanceCounter(&from);
			for (int i = 0; i < tenths; ++i) {
				float dx = 0.0f, dz = 0.0f;
				velocity(i, dx, dz);
				x += dx;
				z += dz;
				g_fake.MovePlayer(x, 5.0f, z);
				now += 100;
				UpdateWeather(now, 41, 3);
				g_fake.RunJob();
				g_fake.RunQueue();
				const int up = kMaxPatches - PatchCount(kPatchFree);
				alive += up;
				most = up > most ? up : most;
			}
			QueryPerformanceCounter(&to);
			const double seconds = tenths / 10.0;
			printf("perf  %s: %.1f patches created and %.1f removed a second, %d alive on average (most %d), %.0f microseconds of plugin time a second\n", what,
				static_cast<double>(g_fake.effects.size() - created) / seconds, static_cast<double>(g_fake.stopped.size() - stopped) / seconds,
				static_cast<int>(alive / tenths), most, micros() / seconds);
		};
		play("running straight for a minute", 600, [](int, float& dx, float&) { dx = 3.4f; });
		play("running diagonally for a minute", 600, [](int, float& dx, float& dz) { dx = 2.4f; dz = 2.4f; });
		play("stop and go (3 s run, 3 s fight) for two minutes", 1200, [](int i, float& dx, float&) { dx = (i / 30) % 2 == 0 ? 3.4f : 0.0f; });
		play("back and forth (4 s each way) for two minutes", 1200, [](int i, float& dx, float&) { dx = (i / 40) % 2 == 0 ? 3.4f : -3.4f; });
		play("standing after all that for 20 s", 200, [](int, float&, float&) {});
	}
	g_config = Still();

	// calm
	settle(41, 0);
	Check(g_patchesUp.load() == 0 && PatchCount(kPatchFree) == kMaxPatches && !g_fake.Alive(marker) && !g_weatherOn, "calm again: every patch removed, and the marker with them");

	// an effect the game cannot create
	g_fake.refuse = second;
	settle(41, 3);
	Check(g_effectFailed[g_config.sandLayer2] && g_fake.AliveCount(first) == 9 && g_fake.AliveCount(third) == 9 && g_fake.AliveCount(second) == 0,
		"an effect the game will not create is left out; the others still come up");
	const size_t queuedAfterFailure = g_fake.queue.size();
	tick(41, 3);
	Check(g_fake.queue.size() == queuedAfterFailure, "...and it is not asked for again");
	g_fake.refuse = nullptr;
	memset(g_effectFailed, 0, sizeof(g_effectFailed));
	settle(41, 3);
	Check(g_fake.AliveCount(second) == 9, "(until the effects are chosen again)");

	// the scene is rebuilt under us: every entity gone
	memset(table, 0, sizeof(table));
	tick(41, 3);
	Check(g_patchesUp.load() == 0 && g_fake.jobPending && strcmp(g_fake.jobPath, first) == 0, "the scene is rebuilt: the patches are forgotten, and the marker is asked for again at once");
	g_fake.MovePlayer(3000.0f, 0.0f, 4000.0f);
	settle(41, 3);
	Check(g_fake.AliveCount(first) == 9 && g_fake.AliveCount(second) == 9 && g_fake.AliveCount(third) == 9, "...and the field is laid out again where the player now is");

	// from a storm straight into the snow
	const uint32_t marker2 = g_fake.Handle();
	settle(110, -1);
	Check(g_fake.AliveCount(first) + g_fake.AliveCount(second) + g_fake.AliveCount(third) == 0 && !g_fake.Alive(marker2), "from a storm into the snow: the sand is removed, the marker too");
	const uint32_t snow = g_fake.Handle();
	Check(g_weatherOn && g_fake.Alive(snow) && strcmp(g_fake.Effect(snow).path, snowPath) == 0 && g_fake.lastIntensity == 1.0f && snowName() == snowPath,
		"...and real snow is started, from the game's own snow name");

	// and back: snow must not be mistaken for the storm's marker
	g_fake.MovePlayer(1010.0f, 5.0f, 2030.0f);
	settle(41, 1);
	Check(!g_fake.Alive(snow) && strcmp(g_fake.Effect(g_fake.Handle()).path, first) == 0 && g_fake.AliveCount(first) == 9, "from the snow into a storm: the snow stops and the storm gets its own marker");
	settle(110, -1);
	const uint32_t snow2 = g_fake.Handle();

	// rain and snow switched off: only the sand is driven
	g_config.weatherEnabled = 0;
	settle(110, -1);
	Check(!g_fake.Alive(snow2) && object[0x7a1] == 0 && !g_weatherHeld, "weather switched off: the snow stops and the weather is handed back");
	settle(41, 1);
	Check(object[0x7a1] == 1 && g_fake.AliveCount(first) == 9, "...but a sandstorm still blows");
	settle(-1, -1);
	Check(g_fake.AliveCount(first) == 0 && object[0x7a1] == 0 && g_fake.Handle() == 0xfffff, "leaving the game: every patch removed, then the weather handed back");
	g_config = Still();

	// leaving while patches are still waiting in the game's queue
	tick(41, 1);
	g_fake.RunJob();
	tick(41, 1);
	Check(g_fake.queue.size() == 6, "(patches waiting in the queue)");
	tick(-1, -1);
	Check(object[0x7a1] == 1, "leaving with patches still queued: the weather is not handed back yet");
	g_fake.RunQueue();
	Check(g_fake.AliveCount(first) == 0 && PatchCount(kPatchFree) == kMaxPatches, "...their jobs create nothing");
	settle(-1, -1);
	Check(object[0x7a1] == 0 && g_fake.Handle() == 0xfffff, "...and then everything is handed back");

	// the plugin going away with patches queued
	tick(41, 1);
	g_fake.RunJob();
	tick(41, 1);
	Check(!g_fake.queue.empty(), "(patches waiting in the queue again)");
	CancelField();
	g_fake.RunQueue();
	Check(g_fake.AliveCount(first) == 0, "unloading with patches queued: their jobs create nothing");
	settle(-1, -1);

	// the first effect cannot be made into a weather effect: the storm takes its next one as marker
	g_fake.refuseWeather = first;
	tick(41, 3);
	g_fake.RunJob();
	for (int i = 0; i < 25; ++i) {
		tick(41, 3);
	}
	// (the game only accepts a new start once its "starting" byte is clear, so a pending job for the next effect shows it was unstuck)
	Check(g_effectFailed[g_config.sandLayer1] && g_fake.jobPending && strcmp(g_fake.jobPath, second) == 0,
		"a marker the game never creates is given up on, the game is not left stuck, and the next effect is tried");
	settle(41, 3);
	Check(snowName() == snowPath && g_fake.AliveCount(second) == 9 && g_fake.AliveCount(third) == 9 && g_fake.AliveCount(first) == 0, "...and the storm goes on with the effects that work");
	g_fake.refuseWeather = nullptr;
	memset(g_effectFailed, 0, sizeof(g_effectFailed));
	settle(-1, -1);

	// a game whose effect names are not where this expects them
	static char odd[] = "something else";
	const uintptr_t oddString[2] = { reinterpret_cast<uintptr_t>(odd), strlen(odd) };
	memcpy(object + 0x70 + 0xb8, oddString, sizeof(oddString));
	const int updatesBeforeOdd = g_fake.updates;
	tick(41, 1);
	tick(41, 1);
	Check(g_fake.updates == updatesBeforeOdd && snowName() == odd && g_fake.queue.empty(), "a snow name that does not look like a game path is left alone, and no storm sand is made");
	memcpy(object + 0x70 + 0xb8, snowString, sizeof(snowString));
	memset(g_effectFailed, 0, sizeof(g_effectFailed));
	settle(-1, -1);

	// a game where the effect code is not what this was written for
	g_fieldReady = false;
	tick(41, 3);
	tick(41, 3);
	Check(!g_fake.jobPending && g_fake.queue.empty() && !g_weatherOn, "effect code not as expected: no marker, no sand (the storm is fog and wind only)");

	memset(g_effectFailed, 0, sizeof(g_effectFailed));
	ForgetWeather();
	g_weatherHeld  = false;
	g_weatherReady = false;
	g_fieldReady   = false;
	g_base         = savedBase;
	g_config       = Config {};
	VirtualFree(image, 0, MEM_RELEASE);
}

} // namespace

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	SeedRandom(12345);
	TestConfig(argc > 1 ? argv[1] : "horror-night.toml");
	TestLevels();
	TestMixer();
	TestSandstorm();
	TestSky();
	TestWeather();
	printf("%s\n", g_failures == 0 ? "all checks passed" : "SOME CHECKS FAILED");
	return g_failures == 0 ? 0 : 1;
}
