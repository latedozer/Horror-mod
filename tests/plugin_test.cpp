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

// The sandstorm under the names its checks were written with (it is one kind of storm now).
int   SandLayers(int level) {
	StormKind* storm = nullptr;
	return StormLayers(level, &storm);
}
float SandBlend() { return StormBlend(); }
void  UpdateSandstorm(uint64_t now, int level) { UpdateStorms(now, level); }
// sandLayers as SandLayers gives it: -1 = not a place for blowing sand
void  UpdateWeather(uint64_t now, int level, int sandLayers) { UpdateWeather(now, level, sandLayers >= 0 ? &g_storms[kStormSand] : nullptr, sandLayers); }

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
	// two files ship: the short one the plugin reads (everyday settings), and the reference with
	// every setting, which sits beside it under the name ...-advanced.toml
	static char shipped[65536];
	static char reference[65536];
	char        referencePath[1024];
	snprintf(referencePath, sizeof(referencePath), "%s", tomlPath);
	if (char* dot = strrchr(referencePath, '.'); dot != nullptr && strlen(referencePath) + 10 < sizeof(referencePath)) {
		snprintf(dot, sizeof(referencePath) - static_cast<size_t>(dot - referencePath), "-advanced.toml");
	}
	const auto read = [](const char* path, char* into, size_t room) {
		FILE*        file = fopen(path, "rb");
		const size_t size = file != nullptr ? fread(into, 1, room - 1, file) : 0;
		if (file != nullptr) {
			fclose(file);
		}
		into[size] = '\0';
		return size;
	};
	Check(read(tomlPath, shipped, sizeof(shipped)) > 0 && read(referencePath, reference, sizeof(reference)) > 0, "the shipped horror-night.toml and horror-night-advanced.toml can be read");

	// rewriting a setting that has its line in a file adds no line to it
	static char result[80000];
	const auto  has = [&](const char* text, const Setting& setting) {
		RewriteConfig(text, setting.section, setting.key, "1", result, sizeof(result));
		return Lines(result) == Lines(text);
	};
	bool allInReference = true;
	bool shortIsRight   = true;
	int  everyday       = 0;
	for (const Setting& setting : kSettings) {
		if (setting.section == nullptr) {
			continue;
		}
		if (!has(reference, setting)) {
			allInReference = false;
			printf("      missing from the reference file: [%s] %s\n", setting.section, setting.key);
		}
		const bool advanced = IsAdvancedKey(setting.key);
		everyday += advanced ? 0 : 1;
		if (has(shipped, setting) == advanced) {
			shortIsRight = false;
			printf("      %s the short file: [%s] %s\n", advanced ? "a fine-tuning setting is in" : "missing from", setting.section, setting.key);
		}
	}
	Check(allInReference, "every setting is in horror-night-advanced.toml");
	Check(shortIsRight && everyday > 80 && everyday < 130, "the short horror-night.toml has every everyday setting and none of the fine-tuning ones");
	Check(Lines(shipped) < Lines(reference) / 2, "...and is less than half as long");

	// ...and the shipped values are the built-in defaults, in both
	const Config defaults {};
	static char  copy[65536];
	for (int which = 0; which < 2; ++which) {
		memcpy(copy, which == 0 ? reference : shipped, sizeof(copy));
		g_config = Config {};
		for (const Setting& setting : kSettings) {
			if (setting.target != nullptr && (which == 0 || !IsAdvancedKey(setting.key))) {
				*setting.target = setting.isBool ? 1 - *setting.target : setting.min; // anything but the default
			}
		}
		ParseConfig(copy);
		Check(memcmp(&g_config, &defaults, sizeof(Config)) == 0, which == 0 ? "the reference file holds the built-in defaults" : "the short file holds the built-in defaults");
	}

	char text[] = "[phantoms]\nenabled = false\nmin_interval = 99999\nmax_interval = 7\nold_setting = 3\n\n[sandstorm]\n  fade=0 # too short\nalways = true\n[nonsense]\nfade = 30\n";
	g_config                 = Config {};
	g_config.phantomsEnabled = 1;
	ParseConfig(text);
	Check(g_config.phantomsEnabled == 0, "false switches a setting off");
	Check(g_config.minInterval == 3600, "a value above its range is brought down to the top of it");
	Check(g_config.maxInterval == 3600, "the longest of a pair is never below the shortest");
	Check(g_config.sand.fade == 1 && g_config.sand.always == 1, "spaces, no spaces and a trailing comment all read; a value below its range is raised");
	Check(g_config.minLife == defaults.minLife && g_config.sand.calmMin == defaults.sand.calmMin, "settings the file does not mention keep their defaults");

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

	// the ash storm's own column, after the six an older plugin reads
	char ashTable[] = "#mod\talwaysNight=1\tskyFlash=0\tsandstorm=1\tashstorm=1\r\nlevel\tnormal\tnightmare_hell\tweather\ttown\tunder\tash\r\n"
		"2\t5\t7\t1\t0\t0\t0\r\n41\t\t\t3\t0\t0\t0\r\n103\t\t\t0\t1\t0\t2\r\n104\t8,9\t\t0\t0\t0\t1\r\n108\t\t\t0\t0\t0\t1\r\n110\t\t\t2\t0\t0\t1\r\n";
	Check(ParseLevels(ashTable) == 6 && g_modAshstorm.load() == 1 && g_modSandstorm.load() == 1, "a levels.tsv with the ash column is read, with both storms announced");
	Check(LevelFall(104) == kFallAsh && LevelFall(108) == kFallAsh && LevelUnder(103) == kFallAsh && IsTown(103), "...ash blows in the marked levels, and the Pandemonium Fortress is sheltered from it");
	Check(LevelFall(2) == kFallRain && LevelFall(41) == kFallSand && LevelFall(110) == kFallSnow && g_levelMonsters[0][104].count == 2, "...the other columns read as before, and a level with weather of its own keeps it");
	Check(StormArea(104) == Area::None, "no lightning in the ash");
	char sandOnly[] = "#mod\talwaysNight=1\tskyFlash=0\tsandstorm=1\r\n41\t\t\t3\t0\t0\r\n104\t\t\t0\t0\t0\r\n";
	Check(ParseLevels(sandOnly) == 2 && g_modAshstorm.load() == 0 && LevelFall(104) == kFallNone && LevelFall(41) == kFallSand, "a levels.tsv from before the ash storm: no ash anywhere");
	char ashOnly[] = "#mod\talwaysNight=1\tskyFlash=0\tsandstorm=0\tashstorm=1\r\n104\t\t\t0\t0\t0\t1\r\n";
	Check(ParseLevels(ashOnly) == 1 && g_modAshstorm.load() == 1 && g_modSandstorm.load() == 0, "the two storms are announced separately");

	// the blizzard's own column, after the ash storm's
	char blizzardTable[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\r\nlevel\tnormal\tnightmare_hell\tweather\ttown\tunder\tash\tblizzard\r\n"
		"2\t\t\t1\t0\t0\t0\t0\r\n104\t\t\t0\t0\t0\t1\t0\r\n109\t\t\t2\t1\t0\t0\t2\r\n110\t\t\t2\t0\t0\t0\t1\r\n113\t\t\t0\t0\t2\t0\t2\r\n120\t\t\t2\t0\t0\t0\t1\r\n";
	Check(ParseLevels(blizzardTable) == 6 && g_modBlizzard.load() == 1 && g_modAshstorm.load() == 1, "a levels.tsv with the blizzard column is read, the blizzard announced");
	Check(LevelFall(110) == kFallBlizzard && LevelFall(120) == kFallBlizzard && LevelUnder(110) == kFallNone, "...the blizzard blows on the marked snowfields");
	Check(LevelFall(109) == kFallSnow && LevelUnder(109) == kFallBlizzard && IsTown(109), "...Harrogath keeps its snow and is sheltered from the blizzard");
	Check(LevelFall(113) == kFallNone && LevelUnder(113) == kFallBlizzard, "...and underground in Act 5 it is overhead instead of the snow");
	Check(LevelFall(2) == kFallRain && LevelFall(104) == kFallAsh, "...the other columns read as before");
	g_config = Config {};
	Check(StormArea(110) == Area::None && StormArea(109) == Area::None && StormArea(113) == Area::None, "no lightning or thunder anywhere in Act 5 with the blizzard: the ice takes its place");
	Check(StormArea(2) == Area::Outdoor, "...the rain of Act 1 still has its lightning");
	char beforeBlizzard[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\r\n109\t\t\t2\t1\t0\t0\r\n110\t\t\t2\t0\t0\t0\r\n113\t\t\t0\t0\t2\t0\r\n";
	Check(ParseLevels(beforeBlizzard) == 3 && g_modBlizzard.load() == 0 && LevelFall(110) == kFallSnow && StormArea(110) == Area::Outdoor && StormArea(113) == Area::Indoor,
		"a levels.tsv from before the blizzard: Act 5 has its snow and its lightning as before");

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
	// the wind bed at this loudness from the start of the buffer to its end, the fire bed silent
	const auto mix = [&](float windFrom, float windTo) {
		const float from[kBeds] = { windFrom, 0.0f }, to[kBeds] = { windTo, 0.0f };
		MixBlock(voices, count, from, to, out);
	};
	voices[count++] = Voice { &shortClip, 0, step(shortClip), 0.5f, kNoBed };
	mix(0.0f, 0.0f);
	Check(out[0] > 8150 && out[0] < 8230 && out[kMixBlock - 1] == out[0] && count == 1, "a sound plays at its gain");
	int blocks = 1;
	while (count > 0 && blocks < 100) {
		mix(0.0f, 0.0f);
		++blocks;
	}
	Check(blocks == 16 && out[kMixBlock - 1] == 0, "...and is dropped when it ends (16 buffers for half a second)");

	// a gust fades in, holds, and follows the wind's loudness
	voices[count++] = Voice { &longClip, 0, step(longClip), 1.0f, kBedWind };
	mix(1.0f, 1.0f);
	Check(out[0] == 0 && out[kMixBlock - 1] > 1200 && out[kMixBlock - 1] < 1400, "a gust fades in from silence");
	for (int i = 0; i < 20; ++i) {
		mix(1.0f, 1.0f);
	}
	Check(out[0] > 16300 && out[kMixBlock - 1] > 16300, "...to full strength");
	mix(1.0f, 0.0f);
	Check(out[0] > 16300 && out[kMixBlock / 2] > 8000 && out[kMixBlock / 2] < 8400 && out[kMixBlock - 1] < 40, "...and gets quieter within a buffer as the wind drops");
	mix(0.0f, 0.0f);
	Check(out[0] == 0 && out[kMixBlock - 1] == 0 && count == 1, "...down to silence, still running");
	count = 0;

	// the bed of gusts
	for (Clip& clip : g_wind) {
		clip = longClip;
	}
	int32_t until = 0;
	voices[count++] = Voice { &shortClip, 0, step(shortClip), 0.5f, kNoBed };
	BedGusts(voices, count, kBedWind, true, false, until);
	Check(count == 2 && voices[1].bed == kBedWind && until >= 35200 && until <= 51200, "wind wanted: a gust starts, the next is due before it ends");
	const int32_t first = until;
	BedGusts(voices, count, kBedWind, true, true, until);
	Check(count == 2 && until == first - static_cast<int32_t>(kMixBlock), "...and nothing more until then");
	until = 1;
	BedGusts(voices, count, kBedWind, true, true, until);
	Check(count == 3 && voices[2].bed == kBedWind, "...then a second gust overlaps the first");
	BedGusts(voices, count, kBedWind, false, true, until);
	Check(count == 3 && until == 0, "wind no longer wanted: the gusts play on while it fades");
	BedGusts(voices, count, kBedWind, false, false, until);
	Check(count == 1 && voices[0].bed == kNoBed, "...and are dropped once it is silent; other sounds are not touched");
	// a bed's low-pass: bass through, the rest gone
	{
		const auto level = [](int cutoff, float hz) { // how much of a tone of that pitch comes out (1 = all)
			LowPass lowPass;
			lowPass.Set(cutoff);
			float peak = 0.0f;
			for (int block = 0; block < 40; ++block) {
				float samples[kMixBlock];
				for (uint32_t i = 0; i < kMixBlock; ++i) {
					samples[i] = std::sin(2.0f * 3.14159265f * hz * static_cast<float>(block * kMixBlock + i) / static_cast<float>(kMixRate));
				}
				lowPass.Run(samples, kMixBlock);
				for (uint32_t i = 0; block >= 20 && i < kMixBlock; ++i) {
					peak = std::fabs(samples[i]) > peak ? std::fabs(samples[i]) : peak;
				}
			}
			return peak;
		};
		Check(level(150, 50.0f) > 0.97f && level(150, 50.0f) < 1.03f, "low-pass at 150 Hz: a 50 Hz tone comes through whole");
		Check(Near(level(150, 150.0f), 0.707f, 0.02f), "...one at 150 Hz at 70% (the cutoff)");
		Check(level(150, 600.0f) < 0.005f && level(150, 3000.0f) < 0.0003f, "...and next to nothing of 600 Hz (under 0.5%) or 3 kHz (70 dB down): no whistle");
		Check(level(0, 3000.0f) > 0.99f, "switched off (0): everything passes");

		// in the mix: the wind bed goes through it, a sound on its own does not
		static int16_t whistle[32000];
		for (int i = 0; i < 32000; ++i) {
			whistle[i] = static_cast<int16_t>(16384.0f * std::sin(2.0f * 3.14159265f * 2000.0f * static_cast<float>(i) / static_cast<float>(kMixRate)));
		}
		const Clip  high { whistle, 32000, kMixRate };
		const float full[kBeds] = { 1.0f, 1.0f };
		LowPass     tone[kBeds];
		tone[kBedWind].Set(150);
		const auto loudest = [&] {
			int most = 0;
			for (int i = 0; i < 15; ++i) { // past the gust's fade-in
				MixBlock(voices, count, full, full, out, tone);
			}
			for (const int16_t sample : out) {
				most = abs(sample) > most ? abs(sample) : most;
			}
			return most;
		};
		count           = 0;
		voices[count++] = Voice { &high, 0, step(high), 1.0f, kBedWind };
		Check(loudest() < 20, "a whistle in the wind bed is gone with the bed's low-pass at 150 Hz");
		count           = 0;
		voices[count++] = Voice { &high, 0, step(high), 1.0f, kBedFire };
		Check(loudest() > 16000, "...the same in the fire bed is untouched (its own low-pass is off)");
		count           = 0;
		voices[count++] = Voice { &high, 0, step(high), 0.5f, kNoBed };
		Check(loudest() > 8000, "...and so is a sound on its own");
		count = 0;
	}

	// a loop played once is faded at both ends
	{
		static int16_t loop[32000];
		for (int16_t& sample : loop) {
			sample = 10000;
		}
		FadeEnds(loop, 32000, 32000, 100, 500);
		Check(loop[0] == 0 && loop[1600] == 5000 && loop[3200] == 10000 && loop[15999] == 10000 && loop[31999 - 8000] == 5000 && loop[31999] == 0,
			"a faded sound starts and ends at silence: 100 ms in, 500 ms out, untouched between");
		static int16_t tiny[10] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
		FadeEnds(tiny, 10, 32000, 100, 500);
		Check(tiny[0] == 0 && tiny[9] == 0, "...and one shorter than its fades is still only faded, half in and half out");
	}

	// the fire is a bed of its own
	for (Clip& clip : g_fire) {
		clip = longClip;
	}
	count = 0;
	int32_t untilWind = 0, untilFire = 0;
	BedGusts(voices, count, kBedWind, true, false, untilWind);
	BedGusts(voices, count, kBedFire, true, false, untilFire);
	Check(count == 2 && voices[0].bed == kBedWind && voices[1].bed == kBedFire && voices[1].clip >= g_fire && voices[1].clip < g_fire + kFireCount,
		"wind and fire wanted: a gust of each, the fire's from the fire sounds");
	Check(untilFire >= 22400 && untilFire <= 35200 && untilFire < untilWind, "...the fire's next one sooner, so more of them overlap");
	for (int i = 0; i < 21; ++i) { // past the fade-in
		const float from[kBeds] = { 1.0f, 0.5f }, to[kBeds] = { 1.0f, 0.5f };
		MixBlock(voices, count, from, to, out);
	}
	Check(out[0] > 24400 && out[0] < 24700, "each is as loud as its own bed: wind at full and fire at half add up to three quarters of full scale");
	{
		const float from[kBeds] = { 0.0f, 0.5f }, to[kBeds] = { 0.0f, 0.5f };
		MixBlock(voices, count, from, to, out);
		Check(out[0] > 8100 && out[0] < 8250, "the wind silent: the fire is heard on its own");
	}
	BedGusts(voices, count, kBedWind, false, false, untilWind);
	Check(count == 1 && voices[0].bed == kBedFire, "the wind faded out: its gusts are dropped, the fire's play on");
	BedGusts(voices, count, kBedFire, false, false, untilFire);
	Check(count == 0, "...until the fire has faded out too");
	g_fire[0] = Clip {}; // one of its two sounds could not be read
	bool always = true;
	for (int i = 0; i < 20; ++i) {
		count     = 0;
		untilFire = 0;
		BedGusts(voices, count, kBedFire, true, false, untilFire);
		always = always && count == 1 && voices[0].clip == &g_fire[1];
	}
	Check(always, "a fire sound that could not be read is passed over for the other");
	for (Clip& clip : g_fire) {
		clip = Clip {};
	}
	for (Clip& clip : g_wind) {
		clip = Clip {};
	}
	count = 0;
	until = 0;
	BedGusts(voices, count, kBedWind, true, false, until);
	Check(count == 0 && until == static_cast<int32_t>(kMixRate), "no wind sounds read: nothing plays, and it looks again later");
}

// ---- sandstorm clock ---------------------------------------------------------------------

// the defaults, with storms that come and go (shipped: the storm never stops)
Config Clocked() {
	Config config {};
	config.sand.always = 0;
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
	Sand().force    = true;
	run(41, 20);
	Check(!Sand().active && Sand().level == 0.0f && SandLayers(41) == -1, "no storm look installed by the mod: nothing happens, even when asked");
	g_modSandstorm = 1;
	Sand().force    = false;

	run(41, 10);
	Check(!Sand().active && Sand().change > now && Sand().change <= now + 240000, "it starts calm, with the first storm on the clock");
	Check(SandLayers(41) == 0, "...and no blowing sand yet");
	Sand().force = true;
	run(41, 40);
	Check(Sand().active && Near(Sand().level, 0.5f, 0.03f), "a storm rolls in: half way after half the roll-in time");
	Check(SandLayers(41) == 2 && Near(SandBlend(), 0.5f, 0.03f), "...with two of the three sand layers up and the storm look in step");
	run(41, 45);
	Check(Sand().level == 1.0f && SandLayers(41) == 3 && SandBlend() == 1.0f && Near(Sand().wind, 1.0f), "...to full strength, all layers up");
	g_config.sand.strength = 50;
	Check(Near(SandBlend(), 0.5f) && SandLayers(41) == 3, "the strength setting scales the look");
	g_config.sand.layer[1] = 0;
	g_config.sand.layer[2] = 0;
	Check(SandLayers(41) == 1, "one layer configured: one layer");
	g_config.sand.layer[0] = 0;
	Check(SandLayers(41) == -1, "no layer configured leaves the weather alone");
	g_config          = Clocked();
	g_config.sand.dust = 0;
	Check(SandLayers(41) == -1, "...and so does blowing sand set to 0");
	g_config = Clocked();

	run(40, 10);
	Check(Sand().active && Sand().level < 0.95f && Sand().level > 0.8f, "walking into Lut Gholein: the storm thins out, it is not cut off");
	Check(Sand().wind < 0.95f && Sand().wind > 0.3f, "...and the wind drops towards muffled");
	run(40, 100);
	Check(Sand().level == 0.0f && Near(Sand().wind, 0.3f), "...until the town is clear, with the wind still heard outside");
	g_config.sand.indoorWind = 0;
	run(40, 30);
	Check(Sand().wind == 0.0f, "...or not, with the muffled wind switched off");
	g_config = Clocked();

	run(41, 100);
	Check(Sand().level == 1.0f, "back out in the desert the same storm rolls in again");
	run(55, 1);
	Check(Sand().level == 0.0f && SandLayers(55) == -1, "in a tomb the storm look is gone at once");
	run(2, 1);
	Check(Sand().level == 0.0f && Sand().active, "in another act too, while the storm goes on by its clock");
	run(-1, 1);
	Check(!Sand().active && Sand().change == 0 && Sand().wind == 0.0f, "leaving the game ends it");

	// the clock itself
	g_config.sand.calmMin = g_config.sand.calmMax = 5;
	g_config.sand.lengthMin = g_config.sand.lengthMax = 6;
	run(41, 45);
	Check(!Sand().active, "calm for the calm time");
	run(41, 10);
	Check(Sand().active, "...then a storm");
	run(41, 55);
	Check(Sand().active, "...for its length");
	run(41, 10);
	Check(!Sand().active, "...then calm again");
	g_config.sand.always = 1;
	run(41, 200);
	Check(Sand().active && Sand().level == 1.0f && Sand().change == 0, "never-ending storm: it stays");
	g_config.sand.always = 0;
	run(41, 1);
	Check(!Sand().active && Sand().change > now, "...and goes back on the clock when that is switched off");
	g_config.sand.enabled = 0;
	run(41, 1);
	Check(Sand().level == 0.0f && SandLayers(41) == -1, "sandstorms switched off: none");
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
	g_modAshstorm  = 0;
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

	// the ash storm look alone (lightning sky and sandstorms both off in the mod)
	g_modSandstorm = 0;
	gameClock();
	UpdateSky(now, true, 0.0f);
	Check(!held() && stage() == 2, "(nothing installed: the clock is the game's)");
	g_modAshstorm = 1;
	UpdateSky(now, true, 0.5f);
	Check(held() && stage() == 1 && Near(blend(), 0.5f), "mod with the ash storm profile only: held, and half a storm shows half way");
	UpdateSky(now, true, 0.0f);
	g_modAshstorm  = 0;
	g_modSandstorm = 1;

	// lightning and sand share the blend: the brighter one shows
	g_modSkyFlash     = 1;
	g_config.skyFlash = 1;
	StartSkyFlash(now);
	UpdateSky(now + 10, true, 0.3f);
	Check(stage() == 1 && blend() == 1.0f, "a lightning flash outshines the storm");
	UpdateSky(now + 60000, true, 0.3f);
	Check(stage() == 1 && Near(blend(), 0.3f), "...and the storm is still there after it");
	Check(Near(g_eventYield.load(), 0.3f), "lighting events give way as much as the storm shows (30%)");
	UpdateSky(now + 60000, true, 0.0f);
	Check(g_eventYield.load() == 0.0f, "...and not at all once it is gone");

	// Act 3 with the mod's monsoons: four looks in a row (flash, night, monsoon, lit monsoon), so
	// that the lightning stays - and everywhere else the blend is the one it always was
	{
		char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\tmonsoon=1\r\n2\t\t\t1\t0\t0\t0\t0\t0\r\n75\t\t\t1\t1\t0\t0\t0\t2\r\n"
			"76\t\t\t1\t0\t0\t0\t0\t1\r\n84\t\t\t0\t0\t1\t0\t0\t2\r\n";
		ParseLevels(table);
		g_config = Config {};
		g_level  = 76;
		UpdateSky(now + 70000, true, 0.0f);
		Check(held() && stage() == 5 && blend() == 0.0f, "Act 3, calm: held on night");
		UpdateSky(now + 70000, true, 0.6f);
		Check(stage() == 1 && Near(blend(), 0.6f) && Near(g_eventYield.load(), 0.6f), "a monsoon rolling in: from the night toward the monsoon look (stage 1)");
		StartSkyFlash(now + 80000);
		UpdateSky(now + 80010, true, 1.0f);
		Check(stage() == 2 && blend() == 1.0f, "lightning inside a monsoon: from the monsoon toward its lit look (stage 2)");
		UpdateSky(now + 80010, true, 0.2f);
		Check(stage() == 0 && blend() == 0.0f, "lightning with hardly any monsoon: the flash look itself (stage 0, nothing of the night blended in)");
		UpdateSky(now + 80010, true, 0.0f);
		Check(stage() == 0 && blend() == 0.0f, "...and with none at all");
		g_config.monsoonFlash = 0;
		UpdateSky(now + 80010, true, 1.0f);
		Check(stage() == 1 && blend() == 1.0f, "sky flash in a monsoon switched off: the monsoon look stays as it is");
		UpdateSky(now + 80010, true, 0.0f);
		Check(stage() == 0, "...the flash outside a monsoon is still there");
		g_config.monsoonFlash = 1;
		g_level               = 75;
		UpdateSky(now + 80010, true, 0.0f);
		Check(stage() == 0 && MonsoonSky(75) && !MonsoonSky(84) && !MonsoonSky(2), "the Kurast Docks have the same lighting: the flash is found there too");
		g_level = 2;
		UpdateSky(now + 80010, true, 0.0f);
		Check(stage() == 1 && blend() == 1.0f, "in Act 1 the flash is where it always was (stage 1)");
		g_level = 76;
		g_skyTestStage = 3;
		g_skyTestBlend = 0.25f;
		g_skyTestUntil = now + 90000;
		UpdateSky(now + 85000, true, 0.0f);
		Check(stage() == 3 && Near(blend(), 0.25f), "horror sky: the asked-for stage and blend are held");
		g_skyTestUntil = 0;
		g_modMonsoon   = 0;
		StartSkyFlash(now + 95000);
		UpdateSky(now + 95010, true, 0.0f);
		Check(stage() == 1 && blend() == 1.0f, "a mod without monsoons: Act 3's flash is where it always was");
		g_level = -1;
		UpdateSky(now + 200000, true, 0.0f);
	}

	gameClock();
	UpdateSky(now, false, 0.0f);
	Check(!held() && stage() == 2 && g_eventYield.load() == 0.0f, "out of the game: the clock is given back");

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
void TestAshStorm() {
	printf("-- ash storm\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\r\n2\t\t\t1\t0\t0\t0\r\n41\t\t\t3\t0\t0\t0\r\n103\t\t\t0\t1\t0\t2\r\n105\t\t\t0\t0\t0\t1\r\n";
	ParseLevels(table);
	g_config            = Clocked();
	g_config.ash.always = 0;
	g_skyReady          = true;
	StormState& ash     = g_storms[kStormAsh].state;
	uint64_t    now     = 3000000;
	const auto  run     = [&](int level, int seconds10) { // tenths of a second
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
		}
	};
	const auto layers = [](int level, StormKind** which) { return StormLayers(level, which); };
	StormKind* which  = nullptr;

	g_modAshstorm = 0;
	ash.force     = true;
	run(105, 20);
	Check(!ash.active && ash.level == 0.0f && layers(105, &which) == -1 && which == nullptr, "no ash storm look installed by the mod: nothing happens, even when asked");
	g_modAshstorm = 1;
	ash.force     = false;

	run(105, 10);
	Check(!ash.active && ash.change > now && layers(105, &which) == 0 && which == &g_storms[kStormAsh], "in Act 4 it starts calm, with the first storm on the clock and no blowing ash yet");
	ash.force = true;
	run(105, 40);
	Check(ash.active && Near(ash.level, 0.5f, 0.03f) && layers(105, &which) == 3 && Near(StormBlend(), 0.5f, 0.03f), "a storm rolls in: half way after half the roll-in time, three of the five layers up, the look in step");
	run(105, 45);
	Check(ash.level == 1.0f && layers(105, &which) == 5 && StormBlend() == 1.0f && Near(ash.wind, 1.0f), "...to full strength, all layers up, the wind with it");
	Check(!Sand().active || Sand().level == 0.0f, "the sandstorm shows nothing here");
	Check(layers(41, &which) != -1 && which == &g_storms[kStormSand], "...and the desert's field is still the sandstorm's");

	run(103, 10);
	Check(ash.active && ash.level < 0.95f && ash.level > 0.8f && ash.wind < 0.95f, "into the Pandemonium Fortress: the storm thins out and the wind drops towards muffled");
	run(103, 100);
	Check(ash.level == 0.0f && Near(ash.wind, 0.3f) && layers(103, &which) == -1, "...until the fortress is clear, the wind still heard outside, and no ash blowing inside");
	run(105, 100);
	Check(ash.level == 1.0f, "back out the same storm rolls in again");

	// six layers for the ash storm, three for the sandstorm
	Check(g_storms[kStormAsh].layers == 6 && g_storms[kStormSand].layers == 3 && kMaxLayers == 6, "the ash storm's field can have six effects, the sandstorm's three");
	const int six[kMaxLayers] = { 1, 6, 2, 9, 14, 16 };
	memcpy(g_config.ash.layer, six, sizeof(six));
	Check(layers(105, &which) == 6, "six configured, at the storm's height: all six up");
	ash.level = 0.5f;
	Check(layers(105, &which) == 4, "...half way in: the first four (they come in one after another, the last near its height)");
	ash.level = 0.05f;
	Check(layers(105, &which) == 0, "...and none before the storm has begun to show");
	g_config.ash.layer[1] = 0;
	g_config.ash.layer[4] = 0;
	ash.level             = 1.0f;
	Check(layers(105, &which) == 4, "empty layers in between are skipped: four configured, four up");
	g_config            = Clocked();
	g_config.ash.always = 0;

	// the two storms keep their own clocks
	Sand().force = true;
	run(105, 1);
	Check(Sand().active && ash.active && Sand().level == 0.0f && ash.level == 1.0f, "a sandstorm starting in Act 2 changes nothing in Act 4");
	run(41, 100);
	Check(Sand().level == 1.0f && ash.level == 0.0f && ash.active && StormBlend() == 1.0f, "in the desert the sandstorm shows and the ash storm does not, while it goes on by its clock");
	run(2, 1);
	Check(Sand().level == 0.0f && ash.level == 0.0f && StormBlend() == 0.0f, "in the rain neither shows");
	g_config.ash.enabled = 0;
	run(105, 1);
	Check(!ash.active && ash.level == 0.0f && layers(105, &which) == -1, "ash storms switched off: none");
	run(-1, 1);
	Check(!Sand().active && !ash.active, "leaving the game ends both");
	g_config   = Clocked();
	g_skyReady = false;
}

// the Act 5 blizzard: a storm kind like the other two, with Harrogath sheltered
void TestBlizzard() {
	printf("-- blizzard\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\r\n2\t\t\t1\t0\t0\t0\t0\r\n105\t\t\t0\t0\t0\t1\t0\r\n"
		"109\t\t\t2\t1\t0\t0\t2\r\n110\t\t\t2\t0\t0\t0\t1\r\n113\t\t\t0\t0\t2\t0\t2\r\n";
	ParseLevels(table);
	g_config                 = Clocked();
	g_config.blizzard.always = 0;
	g_skyReady               = true;
	StormState& blizzard     = g_storms[kStormBlizzard].state;
	uint64_t    now          = 7000000;
	const auto  run          = [&](int level, int seconds10) {
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
		}
	};
	StormKind* which = nullptr;

	g_modBlizzard  = 0;
	blizzard.force = true;
	run(110, 20);
	Check(!blizzard.active && blizzard.level == 0.0f && StormLayers(110, &which) == -1, "no blizzard look installed by the mod: nothing happens, even when asked");
	g_modBlizzard  = 1;
	blizzard.force = false;

	run(110, 10);
	Check(!blizzard.active && StormLayers(110, &which) == 0 && which == &g_storms[kStormBlizzard], "on Act 5's snowfields it starts calm, the first blizzard on the clock");
	Check(g_storms[kStormBlizzard].layers == 6 && kSnowEffectCount > 15 && OnYouOnly(kSnowEffects[1]) && !OnYouOnly(kSnowEffects[2]), "it has six layers, and its first effect is the game's snow, which only falls on you");
	blizzard.force = true;
	run(110, 85);
	Check(blizzard.active && blizzard.level == 1.0f && StormLayers(110, &which) == 6 && StormBlend() == 1.0f && Near(blizzard.wind, 1.0f), "a blizzard rolls in to full strength: all six layers, the look, the wind");
	Check(Sand().level == 0.0f && g_storms[kStormAsh].state.level == 0.0f, "...and neither of the other storms shows in Act 5");
	run(109, 150);
	Check(blizzard.level == 0.0f && Near(blizzard.wind, 0.3f) && StormLayers(109, &which) == -1, "in Harrogath the blizzard clears and its wind is heard muffled");
	run(113, 150);
	Check(blizzard.level == 0.0f && Near(blizzard.wind, 0.3f), "...underground too");
	run(105, 150);
	Check(blizzard.level == 0.0f && blizzard.wind == 0.0f && blizzard.active, "in Act 4 nothing of it is seen or heard, while it goes on by its clock");
	run(110, 150);
	Check(blizzard.level == 1.0f, "back out on the snowfields it blows again");
	g_config.blizzard.enabled = 0;
	run(110, 1);
	Check(!blizzard.active && blizzard.level == 0.0f && StormLayers(110, &which) == -1, "blizzards switched off: none");
	run(-1, 1);
	g_config   = Clocked();
	g_skyReady = false;
}

void TestRainstorm() {
	printf("-- wind-blown rain (Act 1)\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\tmonsoon=1\r\n1\t\t\t1\t1\t0\t0\t0\t0\r\n2\t\t\t1\t0\t0\t0\t0\t0\r\n"
		"8\t\t\t0\t0\t1\t0\t0\t0\r\n76\t\t\t1\t0\t0\t0\t0\t1\r\n110\t\t\t2\t0\t0\t0\t1\t0\r\n";
	ParseLevels(table);
	g_config                  = Clocked();
	g_config.rainstorm.always = 0;
	g_config.monsoon.always   = 0;
	g_skyReady                = true;
	StormState& rain          = g_storms[kStormRain].state;
	uint64_t    now           = 11000000;
	const auto  run           = [&](int level, int seconds10) {
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
		}
	};
	StormKind* which = nullptr;

	run(2, 10);
	Check(!rain.active && StormLayers(2, &which) == 0 && which == &g_storms[kStormRain], "out in the rain of Act 1 it starts calm, on its own clock");
	Check(g_storms[kStormRain].noLook && g_storms[kStormRain].layers == 6 && kRainstormEffectCount == 30 && OnYouOnly(kRainstormEffects[1]) && !OnYouOnly(kRainstormEffects[2]),
		"it has six layers out of twenty-nine effects, the first the game's rain, which only falls on you");
	rain.force = true;
	run(2, 85);
	Check(rain.active && rain.level == 1.0f && StormLayers(2, &which) == 3 && which == &g_storms[kStormRain] && Near(rain.wind, 1.0f), "it builds to its height: all its layers (three as shipped) and its wind");
	Check(StormBlend() == 0.0f, "...and it changes nothing about the lighting: that is the lightning's");
	g_modMonsoon = 0;
	g_modSkyFlash = 0;
	Check(StormLayers(2, &which) == 3, "it needs nothing from the mod");
	g_modMonsoon  = 1;
	g_modSkyFlash = 1;
	run(1, 150);
	Check(rain.level == 0.0f && StormLayers(1, &which) == -1 && rain.wind == 0.0f, "not in the Rogue Encampment, though it rains there: no field, no wind");
	run(8, 150);
	Check(rain.level == 0.0f && rain.wind == 0.0f && StormLayers(8, &which) == -1, "nothing of it underground (muffled wind there is off as shipped)");
	g_config.rainstorm.indoorWind = 1;
	run(8, 150);
	Check(Near(rain.wind, 0.3f), "...switched on, its wind is heard muffled in the caves");
	run(76, 150);
	Check(rain.level == 0.0f && StormLayers(76, &which) != 6 - 100 && which == &g_storms[kStormMonsoon], "in Act 3 with the mod's monsoons the monsoon has the jungle, not this");
	run(110, 50);
	Check(rain.level == 0.0f && which != &g_storms[kStormRain], "and it is nowhere in Act 5");
	run(2, 150);
	Check(rain.level == 1.0f, "back out in the rain of Act 1 it blows again");
	g_config.rainstorm.enabled = 0;
	run(2, 1);
	Check(!rain.active && rain.level == 0.0f && StormLayers(2, &which) == -1, "switched off: none");
	run(-1, 1);
	g_config   = Clocked();
	g_skyReady = false;
}

void TestSettingsMenu() {
	printf("-- settings menu\n");
	const auto rows = [] {
		int count = 0;
		for (const char* at = g_menuText; (at = strstr(at, "[[setting]]")) != nullptr; ++at) {
			++count;
		}
		return count;
	};
	int labelled = 0, everyday = 0;
	for (const Setting& setting : kSettings) {
		labelled += setting.label != nullptr ? 1 : 0;
		everyday += setting.label != nullptr && (setting.key == nullptr || !IsAdvancedKey(setting.key)) ? 1 : 0;
	}
	g_config = Config {};
	bool whole = BuildMenuText();
	Check(whole && rows() == everyday && everyday < labelled - 40, "as shipped the menu lists the everyday settings only");
	Check(strstr(g_menuText, "key = \"plugin_enabled\"") != nullptr && strstr(g_menuText, "key = \"monsoon_effect6\"") != nullptr && strstr(g_menuText, "key = \"blizzard_ice_hits\"") != nullptr
		&& strstr(g_menuText, "key = \"monsoon_layer6\"") == nullptr && strstr(g_menuText, "key = \"blizzard_ice_marker_repeat\"") == nullptr,
		"...the plugin's switch and the effect toggles are in it, the effect numbers and the marker's timings are not");
	g_config.advancedMenu = 1;
	whole                 = BuildMenuText();
	Check(whole && strlen(g_menuText) < sizeof(g_menuText) / 2 && rows() == labelled && rows() > 190, "asked for, it lists every setting, and the list fits its buffer with as much room again");
	Check(strstr(g_menuText, "key = \"monsoon_layer6\"") != nullptr && strstr(g_menuText, "key = \"blizzard_ice_marker_repeat\"") != nullptr, "...the fine-tuning ones too");
	g_config = Config {};
}

void TestMonsoon() {
	printf("-- monsoon\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\tmonsoon=1\r\n2\t\t\t1\t0\t0\t0\t0\t0\r\n75\t\t\t1\t1\t0\t0\t0\t2\r\n"
		"76\t\t\t1\t0\t0\t0\t0\t1\r\n84\t\t\t0\t0\t1\t0\t0\t2\r\n110\t\t\t2\t0\t0\t0\t1\t0\r\n";
	ParseLevels(table);
	g_config                = Clocked();
	g_config.monsoon.always = 0;
	g_skyReady              = true;
	StormState& monsoon     = g_storms[kStormMonsoon].state;
	uint64_t    now         = 9000000;
	const auto  run         = [&](int level, int seconds10) {
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
		}
	};
	StormKind* which = nullptr;

	Check(g_modMonsoon.load() == 1 && LevelFall(76) == kFallMonsoon && LevelFall(75) == kFallRain && LevelUnder(75) == kFallMonsoon && LevelUnder(84) == kFallMonsoon
		&& LevelFall(2) == kFallRain && LevelFall(110) == kFallBlizzard, "a levels.tsv with the monsoon column is read: it blows in the jungle, the docks and the caves shelter");
	Check(StormArea(76) == Area::Outdoor && StormArea(75) == Area::Outdoor && StormArea(84) == Area::Indoor && StormArea(110) == Area::None,
		"lightning and thunder carry on in Act 3: outdoors in the jungle and the docks, muffled underground");
	run(76, 10);
	Check(!monsoon.active && StormLayers(76, &which) == 0 && which == &g_storms[kStormMonsoon], "in the jungle it starts calm, the first monsoon on the clock");
	Check(g_storms[kStormMonsoon].layers == 6 && kMonsoonEffectCount == 36 && OnYouOnly(kMonsoonEffects[1]) && !OnYouOnly(kMonsoonEffects[2]),
		"it has six layers, and its first effect is the game's rain, which only falls on you");
	monsoon.force = true;
	run(76, 85);
	Check(monsoon.active && monsoon.level == 1.0f && StormLayers(76, &which) == 6 && StormBlend() == 1.0f && Near(monsoon.wind, 1.0f), "a monsoon rolls in to full strength: all six layers, the look, the wind");
	Check(Sand().level == 0.0f && g_storms[kStormAsh].state.level == 0.0f && g_storms[kStormBlizzard].state.level == 0.0f, "...and none of the other storms shows in Act 3");
	run(75, 150);
	Check(monsoon.level == 0.0f && Near(monsoon.wind, 0.3f) && StormLayers(75, &which) == -1, "in the Kurast Docks the monsoon clears and its wind is heard muffled");
	run(84, 150);
	Check(monsoon.level == 0.0f && Near(monsoon.wind, 0.3f), "...underground too");
	run(2, 150);
	Check(monsoon.level == 0.0f && monsoon.wind == 0.0f && monsoon.active, "in Act 1 nothing of it is seen or heard, while it goes on by its clock");
	run(76, 150);
	Check(monsoon.level == 1.0f, "back in the jungle it blows again");
	{
		// each effect has a switch of its own: off, its layer is left out, which effect it is stays as it was
		int wanted[kMaxLayers];
		const bool savedReady = g_fieldReady;
		g_fieldReady          = true;
		Check(WantedEffects(&g_storms[kStormMonsoon], 6, wanted) == 6 && wanted[1] == g_config.monsoon.layer[1], "all six effects are wanted at the height of the monsoon");
		g_config.monsoon.layerOn[1] = 0;
		g_config.monsoon.layerOn[4] = 0;
		Check(WantedEffects(&g_storms[kStormMonsoon], 6, wanted) == 4 && wanted[1] == 0 && wanted[4] == 0 && wanted[2] == g_config.monsoon.layer[2] && g_config.monsoon.layer[1] == 12,
			"two effects switched off: four are wanted, and the two keep their numbers for when they are switched back on");
		for (int& on : g_config.monsoon.layerOn) {
			on = 0;
		}
		Check(StormLayers(76, &which) == -1, "every effect switched off: no field at all");
		for (int& on : g_config.monsoon.layerOn) {
			on = 1;
		}
		g_fieldReady = savedReady;
	}
	g_modMonsoon = 0;
	run(76, 20);
	Check(monsoon.level == 0.0f && StormLayers(76, &which) == -1, "no monsoon look installed by the mod: nothing shows");
	g_modMonsoon             = 1;
	g_config.monsoon.enabled = 0;
	run(76, 1);
	Check(!monsoon.active && monsoon.level == 0.0f && StormLayers(76, &which) == -1, "monsoons switched off: none");
	run(-1, 1);
	g_config   = Clocked();
	g_skyReady = false;
}

Config Still() {
	Config config {};
	config.sand.always       = 0;
	config.sand.fieldRadius  = 1;
	config.sand.fieldSpacing = 20;
	config.sand.fieldRate    = 24;
	config.sand.fieldLead    = 0;
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
	const char* const first  = kSandEffects[g_config.sand.layer[0]].path;
	const char* const second = kSandEffects[g_config.sand.layer[1]].path;
	const char* const third  = kSandEffects[g_config.sand.layer[2]].path;
	const float spacing = static_cast<float>(g_config.sand.fieldSpacing) * 2.0f; // world units
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
	g_config.sand.fieldRadius = 0;
	settle(41, 1);
	Check(g_fake.AliveCount(first) <= 9 && g_fake.AliveCount(first) >= 1, "a smaller field: patches beyond it are removed");
	g_config.sand.fieldRadius = 2;
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 25, "a larger field: 5 by 5");
	g_config.sand.layer[0] = 8;
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 0 && g_fake.AliveCount(kSandEffects[8].path) == 25, "another effect chosen: the old patches go, the new ones come");
	g_config = Still();

	// how fast the field fills in
	settle(41, 0);
	g_config.sand.fieldRate = 2;
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
	g_config.sand.fieldRate = 100;
	tick(41, 1);
	Check(g_fake.queue.size() == 9, "a high rate: the rest of the field at once");
	settle(41, 1);
	Check(g_fake.AliveCount(first) == 9, "...and the field is whole either way");
	g_config = Still();
	g_config.sand.layer[0] = g_config.sand.layer[1] = g_config.sand.layer[2] = 4;
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
	g_config.sand.layer[1] = 9;
	settle(41, 3);
	Check(g_fake.AliveCount(kSandEffects[4].path) == 18 && g_fake.AliveCount(kSandEffects[9].path) == 9, "one of three changed: only that field is replaced");
	g_config = Still();

	// a large, dense field while the player runs diagonally (straight up the screen): every cell
	// around the player has its patch after each step, however many are kept behind
	g_config.sand.fieldRadius  = 3;
	g_config.sand.fieldSpacing = 8;
	g_config.sand.fieldRate    = 100;
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
	g_config.sand.fieldRadius = 2;
	g_config.sand.fieldRate   = 1;
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
	g_config.sand.fieldRadius = 2;
	g_config.sand.fieldRate   = 100;
	g_config.sand.fieldLead   = 50;
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
	g_config.sand.fieldRadius  = 5;
	g_config.sand.fieldSpacing = 17;
	g_config.sand.fieldRate    = 30;
	g_config.sand.fieldLead    = 50;
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

	// the same for the ash storm as shipped (five layers, 11 x 11 each)
	settle(41, 0);
	{
		g_config = Still();
		float x = 1010.0f, z = 2030.0f;
		g_fake.MovePlayer(x, 5.0f, z);
		for (int i = 0; i < 80; ++i) {
			now += 1000;
			UpdateWeather(now, 105, &g_storms[kStormAsh], 5);
			g_fake.RunJob();
			g_fake.RunQueue();
		}
		LARGE_INTEGER frequency {}, from {}, to {};
		QueryPerformanceFrequency(&frequency);
		const auto seconds = [&] { return static_cast<double>(to.QuadPart - from.QuadPart) / static_cast<double>(frequency.QuadPart); };
		const auto run     = [&](const char* what, int tenths, float dx) {
			const size_t created = g_fake.effects.size();
			int          most    = 0;
			double       worst   = 0.0;
			QueryPerformanceCounter(&from);
			LARGE_INTEGER before = from;
			for (int i = 0; i < tenths; ++i) {
				x += dx;
				g_fake.MovePlayer(x, 5.0f, z);
				now += 100;
				UpdateWeather(now, 105, &g_storms[kStormAsh], 5);
				g_fake.RunJob();
				g_fake.RunQueue();
				QueryPerformanceCounter(&to);
				const double one = static_cast<double>(to.QuadPart - before.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
				worst            = one > worst ? one : worst;
				before           = to;
				const int up     = kMaxPatches - PatchCount(kPatchFree);
				most             = up > most ? up : most;
			}
			printf("perf  ash storm, %s: %d patches up (most %d), %.1f created a second, %.0f microseconds of plugin time a second, slowest update %.0f microseconds\n", what,
				g_patchesUp.load(), most, static_cast<double>(g_fake.effects.size() - created) / (tenths / 10.0), seconds() * 1e6 / (tenths / 10.0), worst);
		};
		run("standing", 200, 0.0f);
		run("running straight for a minute", 600, 3.4f);
		run("standing after that for 20 s", 200, 0.0f);
		// the whole field going at once is spread over passes
		Check(g_patchesUp.load() == 605, "(the shipped ash field is whole: 605 patches)");
		const size_t before = g_fake.stopped.size();
		now += 1000;
		UpdateWeather(now, 105, &g_storms[kStormAsh], 0);
		const size_t first = g_fake.stopped.size() - before;
		Check(first == 65 && now + 100 == g_weatherNext, "the storm gone: the marker and 64 patches are removed in the first pass, the next pass a tenth of a second on");
		int passes = 1;
		while (PatchCount(kPatchFree) != kMaxPatches && passes < 40) {
			now += 100;
			UpdateWeather(now, 105, &g_storms[kStormAsh], 0);
			++passes;
		}
		Check(g_fake.stopped.size() - before == 606 && passes == 10, "...and all 605 within a second (10 passes)");
		for (int i = 0; i < 12; ++i) {
			now += 1000;
			UpdateWeather(now, 105, &g_storms[kStormAsh], 0);
			g_fake.RunJob();
			g_fake.RunQueue();
		}
		g_fake.MovePlayer(1010.0f, 5.0f, 2030.0f);
	}
	g_config = Still();

	// calm
	settle(41, 0);
	Check(g_patchesUp.load() == 0 && PatchCount(kPatchFree) == kMaxPatches && !g_fake.Alive(marker) && !g_weatherOn, "calm again: every patch removed, and the marker with them");

	// an effect the game cannot create
	g_fake.refuse = second;
	settle(41, 3);
	Check(g_sandFailed[g_config.sand.layer[1]] && g_fake.AliveCount(first) == 9 && g_fake.AliveCount(third) == 9 && g_fake.AliveCount(second) == 0,
		"an effect the game will not create is left out; the others still come up");
	const size_t queuedAfterFailure = g_fake.queue.size();
	tick(41, 3);
	Check(g_fake.queue.size() == queuedAfterFailure, "...and it is not asked for again");
	g_fake.refuse = nullptr;
	memset(g_sandFailed, 0, sizeof(g_sandFailed));
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
	Check(g_sandFailed[g_config.sand.layer[0]] && g_fake.jobPending && strcmp(g_fake.jobPath, second) == 0,
		"a marker the game never creates is given up on, the game is not left stuck, and the next effect is tried");
	settle(41, 3);
	Check(snowName() == snowPath && g_fake.AliveCount(second) == 9 && g_fake.AliveCount(third) == 9 && g_fake.AliveCount(first) == 0, "...and the storm goes on with the effects that work");
	g_fake.refuseWeather = nullptr;
	memset(g_sandFailed, 0, sizeof(g_sandFailed));
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
	memset(g_sandFailed, 0, sizeof(g_sandFailed));
	settle(-1, -1);

	// from one storm to another: the desert's field goes, Act 4's is laid out with its own effects
	{
		g_config.ash.fieldRadius  = 1;
		g_config.ash.fieldSpacing = 20;
		g_config.ash.fieldLead    = 0;
		g_config.ash.layer[0]     = g_config.sand.layer[0]; // the same number in both lists: two different effects
		g_config.ash.layer[1]     = 2;
		const char* const ashFirst  = kAshEffects[g_config.ash.layer[0]].path;
		const char* const ashSecond = kAshEffects[g_config.ash.layer[1]].path;
		const auto        ashSettle = [&](int count) {
			for (int i = 0; i < 12; ++i) {
				now += 1000;
				UpdateWeather(now, 105, &g_storms[kStormAsh], count);
				g_fake.RunJob();
				g_fake.RunQueue();
			}
		};
		settle(41, 1);
		Check(g_fake.AliveCount(first) == 9, "(a sandstorm's field is up in the desert)");
		ashSettle(2);
		Check(g_fake.AliveCount(first) == 0, "walking from the desert's storm into Act 4's: the sand is taken away");
		Check(g_fake.Alive(g_fake.Handle()) && strcmp(g_fake.Effect(g_fake.Handle()).path, ashFirst) == 0, "...the storm's marker is made again from the ash storm's first effect");
		Check(g_fake.AliveCount(ashFirst) == 9 && g_fake.AliveCount(ashSecond) == 9 && snowName() == snowPath, "...and its own effects are laid out around the player");
		// all six of the ash storm's layers
		const int six[kMaxLayers] = { 6, 2, 9, 14, 16, 1 };
		memcpy(g_config.ash.layer, six, sizeof(six));
		ashSettle(6);
		ashSettle(6);
		bool sixFields = true;
		for (const int effect : six) {
			sixFields = sixFields && g_fake.AliveCount(kAshEffects[effect].path) == 9;
		}
		Check(sixFields && g_patchesUp.load() == 54, "six layers configured: six fields, one of each effect, around the player");
		ashSettle(2);
		Check(g_fake.AliveCount(kAshEffects[six[0]].path) == 9 && g_fake.AliveCount(kAshEffects[six[1]].path) == 9 && g_fake.AliveCount(kAshEffects[six[2]].path) == 0
			&& g_fake.AliveCount(kAshEffects[six[5]].path) == 0 && g_patchesUp.load() == 18, "...and as the storm dies down the later layers are taken away first");
		g_sandFailed[g_config.sand.layer[0]] = false;
		settle(41, 1);
		Check(g_fake.AliveCount(ashFirst) == 0 && g_fake.AliveCount(ashSecond) == 0 && g_fake.AliveCount(first) == 9, "and back: the ash goes, the sand returns");
		g_config = Still();
		settle(-1, -1);
	}

	// the blizzard: the game's snow falls between blizzards, and is the marker during one; the
	// snow is never laid out around the player, the blowing effects are
	{
		char blizzardLevels[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\r\n2\t\t\t1\t0\t0\t0\t0\r\n41\t\t\t3\t0\t0\t0\t0\r\n"
			"109\t\t\t2\t1\t0\t0\t2\r\n110\t\t\t2\t0\t0\t0\t1\r\n";
		ParseLevels(blizzardLevels);
		g_config.weatherIntensity       = 50;
		g_config.blizzard.fieldRadius   = 1;
		g_config.blizzard.fieldSpacing  = 20;
		g_config.blizzard.fieldLead     = 0;
		const int lay[kMaxLayers]       = { 1, 5, 2, 0, 0, 0 };
		memcpy(g_config.blizzard.layer, lay, sizeof(lay));
		const auto at = [&](int level, StormKind* storm, int count) {
			for (int i = 0; i < 12; ++i) {
				now += 1000;
				UpdateWeather(now, level, storm, count);
				g_fake.RunJob();
				g_fake.RunQueue();
			}
		};
		const auto snowing = [&] { return g_weatherOn && g_fake.Alive(g_fake.Handle()) && strcmp(g_fake.Effect(g_fake.Handle()).path, snowPath) == 0; };
		StormKind* const storm = &g_storms[kStormBlizzard];
		at(110, storm, 0);
		Check(snowing() && Near(g_fake.lastIntensity, 0.5f) && g_patchesUp.load() == 0, "a calm on Act 5's snowfields: the snow falls as always (at the weather's own strength), nothing laid out");
		at(110, storm, 1);
			Check(snowing() && g_fake.lastIntensity == 1.0f && g_patchesUp.load() == 0 && g_fake.AliveCount(snowPath) == 0,
			"a blizzard begins: its first layer is the snow on you (the marker), at full strength, never laid out around you");
		at(110, storm, 3);
		Check(snowing() && g_fake.AliveCount(kSnowEffects[5].path) == 9 && g_fake.AliveCount(kSnowEffects[2].path) == 9 && g_fake.AliveCount(snowPath) == 0 && g_patchesUp.load() == 18,
			"at its height: the blowing effects laid out, 3 by 3 each, and still only the one snow");
		at(109, nullptr, -1);
		Check(snowing() && g_patchesUp.load() == 0 && Near(g_fake.lastIntensity, 0.5f), "into Harrogath: the field goes, and its snow falls as always");
		g_config.weatherEnabled = 0;
		at(110, storm, 0);
		Check(!g_weatherOn, "the weather switched off: no snow between blizzards");
		g_config = Still();
		settle(-1, -1);
	}

	// "horror probe": one effect of the list made the weather's marker for a few seconds
	{
		const auto named = [](const char* name) {
			for (int i = 1; i < kProbeCount; ++i) {
				if (strcmp(kProbeEffects[i].name, name) == 0) {
					return i;
				}
			}
			return 0;
		};
		const auto run = [&](int ticks, int level) {
			for (int i = 0; i < ticks; ++i) {
				tick(level, -1);
				g_fake.RunJob();
			}
		};
		const auto raining = [&] { return g_weatherOn && g_fake.Alive(g_fake.Handle()) && strcmp(g_fake.Effect(g_fake.Handle()).path, rainPath) == 0; };
		const int  embers  = named("ash embers 20x20");
		const int  wind    = named("Act 4 Mesa wind small 10x10");
		Check(embers > 0 && wind > 0 && kProbeCount > 40, "the probe list holds the Act 4 candidates");
		memset(g_probeOutcome, 0, sizeof(g_probeOutcome));
		run(3, 2);
		const uint32_t rainBefore = g_fake.Handle();
		Check(raining(), "(rain is falling before the probe)");

		g_probeRequest = embers;
		tick(2, -1);
		Check(!g_fake.Alive(rainBefore) && g_fake.jobPending && strcmp(g_fake.jobPath, kProbeEffects[embers].path) == 0,
			"a probe: the rain stops and the game is asked to create the effect as its weather");
		g_fake.RunJob();
		const uint32_t probed = g_fake.Handle();
		tick(2, -1);
		Check(g_probeOutcome[embers] == kProbeCreated && snowName() == snowPath && g_fake.Alive(probed), "...the game created it: said so, and the snow name is the game's own again");
		run(8, 2);
		Check(!g_fake.Alive(probed) && g_probeEffect == 0 && raining(), "after its seconds the effect is taken away and the rain falls again");

		g_fake.refuseWeather = kProbeEffects[wind].path;
		g_probeRequest       = wind;
		run(30, 2);
		Check(g_probeOutcome[wind] == kProbeNotCreated && g_fake.Starting() == 0 && g_probeEffect == 0 && raining(),
			"an effect the game never creates is said to be not created, the game is not left stuck, and the rain comes back");

		memset(g_probeOutcome, 0, sizeof(g_probeOutcome));
		g_probeRequest = kProbeAskAll;
		run(kProbeCount * 9 + 40, 2);
		int created = 0;
		for (int i = 1; i < kProbeCount; ++i) {
			created += g_probeOutcome[i] == kProbeCreated ? 1 : 0;
		}
		Check(created == kProbeCount - 2 && g_probeOutcome[wind] == kProbeNotCreated, "horror probe all: every effect is tried in turn and each has its outcome");
		Check(g_probeEffect == 0 && !g_probeAll && raining(), "...and the weather is the game's again at the end");
		g_fake.refuseWeather = nullptr;

		g_probeRequest = embers;
		run(2, 2);
		Check(g_probeEffect == embers && !raining(), "(a probe is up)");
		g_probeRequest = kProbeAskStop;
		run(1, 2);
		Check(g_probeEffect == 0 && raining(), "horror probe stop ends it at once");
		g_probeRequest = embers;
		run(2, 2);
		tick(-1, -1);
		Check(g_probeEffect == 0 && !g_weatherOn && object[0x7a1] == 0, "leaving the game ends a probe and hands the weather back");
		memset(g_probeOutcome, 0, sizeof(g_probeOutcome));
		settle(-1, -1);
	}

	// a game where the effect code is not what this was written for
	g_fieldReady = false;
	tick(41, 3);
	tick(41, 3);
	Check(!g_fake.jobPending && g_fake.queue.empty() && !g_weatherOn, "effect code not as expected: no marker, no sand (the storm is fog and wind only)");

	memset(g_sandFailed, 0, sizeof(g_sandFailed));
	ForgetWeather();
	g_weatherHeld  = false;
	g_weatherReady = false;
	g_fieldReady   = false;
	g_base         = savedBase;
	g_config       = Config {};
	VirtualFree(image, 0, MEM_RELEASE);
}

} // namespace

// ---- the server tick against stand-ins for the engine functions it calls ----
struct ServerShot {
	int      row;
	int      skill;
	int32_t  x;
	int32_t  y;
	uint32_t frame;
};
struct ServerFakes {
	int      iterations = 0; // player walks
	int      missiles   = 0;
	int      sent       = 0;
	int      lastRow    = -1;
	int      level      = 2;
	int32_t  position[2] = { 5000, 5000 };
	int32_t  life        = 100 * 256;
	int32_t  maxLife     = 100 * 256;
	int      lifeSets    = 0;
	uint32_t player[8]   = { 0, 0, 1, 1 }; // type 0 (player), id 1, mode 1 (neutral)
	uint8_t  room[64]    = {};
	uint32_t missile[8]  = { 3, 0, 99, 1 };
	std::vector<ServerShot> shots;
} g_server;

void FakeIterPlayers(Game* game, PlayerCallback callback, void* user) {
	++g_server.iterations;
	callback(game, g_server.player, user);
}
Room*    FakeUnitRoom(Unit*) { return g_server.room; }
void     FakeUnitPos(Unit*, int32_t* xy) {
	xy[0] = g_server.position[0];
	xy[1] = g_server.position[1];
}
int32_t  FakeRoomLevel(Room*) { return g_server.level; }
int32_t  FakeGetStat(Unit*, uint32_t stat, uint16_t) { return stat == 6 ? g_server.life : stat == 7 ? g_server.maxLife : 0; }
void     FakeSetStat(Unit*, uint32_t stat, int32_t value, uint16_t) {
	if (stat == 6) {
		g_server.life = value;
		++g_server.lifeSets;
	}
}
uint32_t FakeCastId(Game*) { return 1; }
void*    FakePlayerClient(Unit*) { return g_server.room; }
void     FakeSendMissile(void*, Game*, Unit*, int32_t) { ++g_server.sent; }
Unit*    FakeMissileCreate(Game* game, void* params) {
	++g_server.missiles;
	ServerShot shot {};
	memcpy(&shot.row, static_cast<uint8_t*>(params) + 0x20, 4);
	memcpy(&shot.x, static_cast<uint8_t*>(params) + 0x24, 4);
	memcpy(&shot.y, static_cast<uint8_t*>(params) + 0x28, 4);
	memcpy(&shot.skill, static_cast<uint8_t*>(params) + 0x3c, 4);
	shot.frame        = GameFrame(game);
	g_server.lastRow  = shot.row;
	g_server.shots.push_back(shot);
	return g_server.missile;
}

// A stand-in game image with only what the bolts and meteors use: anything else the tick called
// would land in empty memory.
uint8_t* ServerImage() {
	auto* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, kFunctions[kCastId].rva + 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (image == nullptr) {
		return nullptr;
	}
	PlaceJump(image + kFunctions[kIterPlayers].rva, reinterpret_cast<void*>(&FakeIterPlayers));
	PlaceJump(image + kFunctions[kUnitRoom].rva, reinterpret_cast<void*>(&FakeUnitRoom));
	PlaceJump(image + kFunctions[kUnitPos].rva, reinterpret_cast<void*>(&FakeUnitPos));
	PlaceJump(image + kFunctions[kRoomLevel].rva, reinterpret_cast<void*>(&FakeRoomLevel));
	PlaceJump(image + kFunctions[kGetStat].rva, reinterpret_cast<void*>(&FakeGetStat));
	PlaceJump(image + kFunctions[kSetStat].rva, reinterpret_cast<void*>(&FakeSetStat));
	PlaceJump(image + kFunctions[kMissileCreate].rva, reinterpret_cast<void*>(&FakeMissileCreate));
	PlaceJump(image + kFunctions[kPlayerClient].rva, reinterpret_cast<void*>(&FakePlayerClient));
	PlaceJump(image + kFunctions[kSendMissile].rva, reinterpret_cast<void*>(&FakeSendMissile));
	PlaceJump(image + kFunctions[kCastId].rva, reinterpret_cast<void*>(&FakeCastId));
	return image;
}

void TestServerBolts() {
	printf("-- server tick: bolts without phantom rows\n");
	uint8_t* image = ServerImage();
	if (image == nullptr) {
		Check(false, "memory for the stand-in game");
		return;
	}
	const uintptr_t savedBase = g_base;
	g_base                    = reinterpret_cast<uintptr_t>(image);
	g_server                  = ServerFakes {};
	g_config                  = Config {};
	g_config.directHits       = 0;
	DefaultLevelInfo();
	for (ClassList& classes : g_classes) {
		classes.count = 0; // the D2RMM mod was installed with its phantoms off
	}
	g_lightningRow[3]   = 742;
	g_lightningSkill[3] = 57;
	static uint8_t game[0x200];
	memset(game, 0, sizeof(game));
	game[0x106]    = 3; // data bank
	uint32_t frame = 100;
	const auto run = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			memcpy(game + 0x170, &frame, 4);
			ServerTick(game);
			++frame;
		}
	};
	g_game = nullptr;
	run(1); // a new game
	const int walks = g_server.iterations;
	run(50);
	Check(g_server.iterations == walks && g_server.missiles == 0, "no phantom rows and no strike: the tick does nothing at all");
	g_forceSpawn = true;
	++g_flashRequests; // the client's storm asks for a strike
	run(60);
	Check(g_server.missiles == 3 && g_server.sent == 3 && g_server.lastRow == 742, "a strike still drops its bolts (3 flickers, 1 bolt each) with the phantoms off in the mod");
	Check(!g_forceSpawn.load() && g_aliveCount.load() == 0, "...and a phantom asked for meanwhile is dropped instead of spawned");
	g_server.level = 1; // the Rogue Encampment
	++g_flashRequests;
	run(60);
	Check(g_server.missiles == 3, "no bolts in town");

	// phantom rows installed, but nothing for them to do: the tick stays on its short path
	g_classes[3].count       = 1;
	g_config.phantomsEnabled = 0;
	g_game                   = nullptr;
	run(1); // a new game: the first spawn is 20-60 s away
	const int idle = g_server.iterations;
	run(60 * 25 + 50);
	Check(g_server.iterations == idle, "phantoms switched off in the plugin: once the spawn time has passed the tick still does nothing");
	// the whole plugin switched off (its setting, its key): phantoms switched on are not spawned either
	g_config.phantomsEnabled = 1;
	g_config.pluginEnabled   = 0;
	g_game                   = nullptr;
	run(1);
	const int off = g_server.iterations;
	run(60 * 25 + 50);
	Check(g_server.iterations == off && g_aliveCount.load() == 0, "the plugin switched off: no phantoms, and the tick stays on its short path");
	g_config.pluginEnabled = 1;
	TogglePlugin();
	const bool wentOff = g_config.pluginEnabled == 0;
	TogglePlugin();
	Check(wentOff && g_config.pluginEnabled == 1, "the on/off key switches it off and on again");
	g_config.phantomsEnabled = 0;
	g_config.phantomsEnabled = 1;
	g_diabloDead             = true;
	run(50);
	Check(g_server.iterations == idle, "Diablo dead (no more phantoms this game): the same");
	g_diabloDead       = false;
	g_classes[3].count = 0;
	g_base = savedBase;
	g_game = nullptr;
	VirtualFree(image, 0, MEM_RELEASE);
}

// ---- what the storms ask of the mixer ----
void TestStormSounds() {
	printf("-- storm sounds\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\r\n2\t\t\t1\t0\t0\t0\r\n41\t\t\t3\t0\t0\t0\r\n103\t\t\t0\t1\t0\t2\r\n105\t\t\t0\t0\t0\t1\r\n";
	ParseLevels(table);
	g_config       = Config {}; // as shipped: both storms always blowing
	g_skyReady     = true;
	g_modSandstorm = 1;
	g_modAshstorm  = 1;
	// a mixer that is "running": requests queue up, nothing plays
	static int16_t one[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
	for (Clip& clip : g_burst) {
		clip = Clip { one, 8, kMixRate };
	}
	g_mixEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_mixThread = GetCurrentThread();
	g_soundsReady.store(true);
	uint64_t   now  = 9000000;
	const auto run  = [&](int level, int seconds10) {
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
			UpdateBursts(now, level);
		}
	};
	const auto wind = [] { return g_beds[kBedWind].gain.load(); };
	const auto fire = [] { return g_beds[kBedFire].gain.load(); };

	g_config.rainstorm.enabled = 0; // (level 2 is out in the rain: its own wind is another test's)
	run(2, 10);
	Check(wind() == 0.0f && fire() == 0.0f, "no storm here: no wind, no fire");
	run(105, 120);
	Check(Near(wind(), 1.9f) && Near(fire(), 0.7f), "out in the ash storm: wind and fire, each at its own volume (190% and 70% as shipped)");
	g_config.fireVolume = 30;
	run(105, 1);
	Check(Near(wind(), 1.9f) && Near(fire(), 0.3f), "the fire's volume is its own setting");
	g_config.fireVolume = 0;
	run(105, 1);
	Check(Near(wind(), 1.9f) && fire() == 0.0f, "...and 0 is no fire");
	g_config.fireVolume = 70;
	run(103, 150);
	Check(Near(wind(), 0.57f) && Near(fire(), 0.21f), "in the Pandemonium Fortress both are muffled (to three tenths)");
	g_config.ash.indoorWind = 0;
	run(103, 100);
	Check(wind() == 0.0f && fire() == 0.0f, "...or silent, with the muffled wind switched off");
	g_config.ash.indoorWind = 1;
	run(41, 150);
	Check(Near(wind(), 0.7f) && fire() == 0.0f, "the sandstorm has wind and no fire");
	Check(g_beds[kBedWind].lowPass.load() == 0, "...and the whole wind");
	run(105, 100);
	Check(g_beds[kBedWind].lowPass.load() == 100, "the ash storm's wind is all bass: only what it has below 100 Hz");
	g_config.ash.windLowPass = 300;
	run(105, 1);
	Check(g_beds[kBedWind].lowPass.load() == 300, "...its own setting, followed at once");
	run(0, 1);
	Check(g_beds[kBedWind].lowPass.load() == 300, "no wind: it stays as it was while the last of the wind fades");
	g_config.ash.windLowPass = 100;
	run(41, 100);

	// steam and rockfalls
	g_requestCount = 0;
	g_totalBursts  = 0;
	run(41, 3000);
	Check(g_totalBursts.load() == 0 && g_requestCount == 0, "none in the sandstorm (five minutes)");
	g_config.burstMinInterval = g_config.burstMaxInterval = 20;
	run(105, 150); // the storm builds: 8 seconds to full
	g_requestCount = 0;
	g_totalBursts  = 0;
	g_storms[kStormAsh].burstNext    = 0;
	run(105, 1);
	Check(g_totalBursts.load() == 0 && g_storms[kStormAsh].burstNext == now + 20000, "out in the ash: the first is put on the clock, not played at once");
	run(105, 199);
	Check(g_totalBursts.load() == 0, "...nothing before its time");
	run(105, 1);
	Check(g_totalBursts.load() == 1 && g_requestCount == 1 && g_requests[0].clip >= g_burst && g_requests[0].clip < g_burst + kBurstCount && g_requests[0].gain >= 0.349f
			&& g_requests[0].gain <= 0.701f,
		"then one of the six is played, at half to all of its volume");
	g_requestCount = 0;
	run(105, 600);
	Check(g_totalBursts.load() == 4 && g_requestCount == 3, "...and one every interval after that (3 more in a minute at 20 s)");
	g_requestCount = 0;
	run(103, 600);
	Check(g_totalBursts.load() == 4 && g_storms[kStormAsh].burstNext == 0, "none in the Pandemonium Fortress");
	g_config.burstVolume = 0;
	run(105, 1200);
	Check(g_totalBursts.load() == 4 && g_requestCount == 0, "volume 0: none at all");

	// the blizzard: the whole wind, the mountain's howl under it, hard gusts now and then
	{
		char blizzardTable[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\r\n105\t\t\t0\t0\t0\t1\t0\r\n109\t\t\t2\t1\t0\t0\t2\r\n110\t\t\t2\t0\t0\t0\t1\r\n";
		ParseLevels(blizzardTable);
		g_config      = Config {};
		g_modBlizzard = 1;
		for (Clip& clip : g_gust) {
			clip = Clip { one, 8, kMixRate };
		}
		const auto howl = [] { return g_beds[kBedHowl].gain.load(); };
		run(110, 150);
		Check(Near(wind(), 1.2f) && Near(howl(), 0.7f) && fire() == 0.0f, "out in the blizzard: wind and the mountain's howl at their own volumes (120% and 70% as shipped), no fire");
		Check(g_beds[kBedWind].lowPass.load() == 0, "...the whole wind, whistle and all");
		run(105, 150);
		Check(Near(fire(), 0.7f) && howl() == 0.0f && g_beds[kBedWind].lowPass.load() == 100, "in the ash storm: fire and no howl, and the ash storm's bass wind");
		run(109, 150);
		Check(Near(wind(), 0.36f) && Near(howl(), 0.21f), "in Harrogath both are muffled");
		g_config.gustMinInterval = g_config.gustMaxInterval = 10;
		run(110, 150);
		g_requestCount = 0;
		g_totalBursts  = 0;
		run(110, 1);
		run(110, 300);
		Check(g_totalBursts.load() == 3 && g_requestCount == 3 && g_requests[0].clip >= g_gust && g_requests[0].clip < g_gust + kGustCount, "hard gusts: one every interval, out in the blizzard (3 in 30 s at 10 s)");
		g_requestCount = 0;
		run(109, 300);
		Check(g_totalBursts.load() == 3 && g_storms[kStormBlizzard].burstNext == 0, "none in Harrogath");
		g_config.howlVolume = 0;
		run(110, 150);
		Check(howl() == 0.0f, "howl volume 0: no howl");
		for (Clip& clip : g_gust) {
			clip = Clip {};
		}
		g_modBlizzard = 0;
	}

	run(0, 1); // out of the game
	Check(wind() == 0.0f && fire() == 0.0f && g_beds[kBedHowl].gain.load() == 0.0f, "leaving the game silences them all");
	CloseHandle(g_mixEvent);
	g_mixEvent  = nullptr;
	g_mixThread = nullptr;
	g_soundsReady.store(false);
	g_requestCount = 0;
	for (Clip& clip : g_burst) {
		clip = Clip {};
	}
	g_config = Config {};
}

// ---- the ash storm's meteors ----
void TestServerMeteors() {
	printf("-- server tick: meteors\n");
	uint8_t* image = ServerImage();
	if (image == nullptr) {
		Check(false, "memory for the stand-in game");
		return;
	}
	const uintptr_t savedBase = g_base;
	g_base                    = reinterpret_cast<uintptr_t>(image);
	g_server                  = ServerFakes {};
	g_config                  = Config {};
	g_config.meteor.countMin = g_config.meteor.countMax = 3;
	g_config.meteor.hits = 0;
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\r\n2\t\t\t1\t0\t0\t0\r\n103\t\t\t0\t1\t0\t2\r\n105\t\t\t0\t0\t0\t1\r\n";
	ParseLevels(table);
	for (ClassList& classes : g_classes) {
		classes.count = 0; // no phantom rows: the meteors do not need them either
	}
	g_showers[kShowerMeteor].row[3]    = 750;
	g_showers[kShowerMeteor].hitRow[3] = 751;
	g_showers[kShowerMeteor].skill[3]  = 56;
	static uint8_t game[0x200];
	memset(game, 0, sizeof(game));
	game[0x106]    = 3;
	uint32_t frame = 100;
	const auto run = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			memcpy(game + 0x170, &frame, 4);
			ServerTick(game);
			++frame;
		}
	};
	const auto distance = [](const ServerShot& shot) {
		const double dx = shot.x - g_server.position[0], dy = shot.y - g_server.position[1];
		return std::sqrt(dx * dx + dy * dy);
	};
	g_game = nullptr;
	run(1);

	// a volley, out in the ash
	g_server.level = 105;
	++g_showers[kShowerMeteor].requests;
	run(80);
	bool plain = g_server.shots.size() == 3, ring = true, apart = true;
	for (size_t i = 0; i < g_server.shots.size(); ++i) {
		const ServerShot& shot = g_server.shots[i];
		plain = plain && shot.row == 750 && shot.skill == 56;
		ring  = ring && distance(shot) >= g_config.meteor.minDistance - 1.5 && distance(shot) <= g_config.meteor.maxDistance + 0.5;
		apart = apart && (i == 0 || (shot.frame - g_server.shots[i - 1].frame >= 1 && shot.frame - g_server.shots[i - 1].frame <= 6));
	}
	Check(plain && g_server.sent == 3, "a volley drops its meteors (3) as the meteor row, with Meteor as their skill");
	Check(ring, "...each landing in the ring around the player that the settings give");
	Check(apart, "...in quick succession: 40 to 240 ms apart (1 to 6 game frames) as shipped");
	Check(g_server.lifeSets == 0, "...and none of them touches the player's life");

	// the gap inside a volley is a setting
	g_server.shots.clear();
	g_config.meteor.gapMin = g_config.meteor.gapMax = 400;
	++g_showers[kShowerMeteor].requests;
	run(80);
	Check(g_server.shots.size() == 3 && g_server.shots[1].frame - g_server.shots[0].frame == 10 && g_server.shots[2].frame - g_server.shots[1].frame == 10,
		"a gap of 400 ms: the meteors are 10 game frames apart");
	g_server.shots.clear();
	g_config.meteor.gapMin = g_config.meteor.gapMax = 0;
	++g_showers[kShowerMeteor].requests;
	run(80);
	Check(g_server.shots.size() == 3 && g_server.shots[2].frame == g_server.shots[0].frame, "a gap of 0: the whole volley falls together");
	g_config.meteor.gapMin = 40;
	g_config.meteor.gapMax = 240;

	// where they fall
	g_server.shots.clear();
	g_server.level = 103;
	++g_showers[kShowerMeteor].requests;
	run(80);
	g_server.level = 2;
	++g_showers[kShowerMeteor].requests;
	run(80);
	Check(g_server.shots.empty(), "no meteors in the Pandemonium Fortress, or outside the ash");
	g_showers[kShowerMeteor].force = true;
	run(80);
	Check(g_server.shots.size() == 3, "horror meteor drops a volley anywhere outside town");

	// an aimed one: its marker under the player, a burn if they are still there when it lands
	g_server.shots.clear();
	g_server.level             = 105;
	g_config.meteor.hits        = 1;
	g_config.meteor.hitChance   = 100;
	g_config.meteor.damageMin   = 5;
	g_config.meteor.damageMax = 10;
	++g_showers[kShowerMeteor].requests;
	run(30);
	Check(!g_server.shots.empty() && g_server.shots[0].row == 751 && distance(g_server.shots[0]) <= 1.5, "an aimed meteor: the volley's first is the marker row, on the player's spot");
	Check(g_server.lifeSets == 0, "...and nothing happens to the player while it falls");
	run(60);
	Check(g_server.shots.size() == 3 && g_server.shots[1].row == 750 && g_server.shots[2].row == 750, "...the rest of the volley are plain meteors");
	Check(g_server.lifeSets == 1 && g_server.life >= 90 * 256 && g_server.life <= 95 * 256, "a player still standing there when it lands is burned for 5-10% of maximum life");

	g_server.shots.clear();
	g_server.life     = 100 * 256;
	g_server.lifeSets = 0;
	++g_showers[kShowerMeteor].requests;
	run(10);
	g_server.position[0] += 20; // stepped out of the marker
	run(90);
	Check(g_server.shots.size() == 3 && g_server.lifeSets == 0 && g_server.life == 100 * 256, "a player who steps out of the marker in time is not burned");

	g_server.position[0]        = 5000;
	g_server.life               = 300; // just over 1 life
	g_config.meteor.damageMin    = 100;
	g_config.meteor.damageMax = 100;
	++g_showers[kShowerMeteor].requests;
	run(100);
	Check(g_server.life == 256, "an aimed meteor never kills: 1 life is left");

	g_config.meteor.hits = 0;
	g_server.shots.clear();
	g_server.lifeSets = 0;
	++g_showers[kShowerMeteor].requests;
	run(100);
	bool anyMarker = false;
	for (const ServerShot& shot : g_server.shots) {
		anyMarker = anyMarker || shot.row == 751;
	}
	Check(g_server.shots.size() == 3 && !anyMarker && g_server.lifeSets == 0, "aimed meteors switched off: no markers, no burns");

	g_showers[kShowerMeteor].row[3] = -1;
	g_server.shots.clear();
	++g_showers[kShowerMeteor].requests;
	run(60);
	Check(g_server.shots.empty(), "a mod without the meteor rows: none fall");

	g_showers[kShowerMeteor].row[3]    = -1;
	g_showers[kShowerMeteor].hitRow[3] = -1;
	g_config          = Config {};
	g_base            = savedBase;
	g_game            = nullptr;
	VirtualFree(image, 0, MEM_RELEASE);
}

// the client's side: when volleys are asked for
void TestMeteorClock() {
	printf("-- meteor clock\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\r\n2\t\t\t1\t0\t0\t0\r\n103\t\t\t0\t1\t0\t2\r\n105\t\t\t0\t0\t0\t1\r\n";
	ParseLevels(table);
	g_config                   = Config {};
	g_config.meteor.minInterval = g_config.meteor.maxInterval = 10;
	g_skyReady                 = true;
	g_modAshstorm              = 1;
	uint64_t   now             = 9000000;
	const auto run             = [&](int level, int seconds10) {
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
			UpdateShowers(now, level);
		}
	};
	const uint32_t before = g_showers[kShowerMeteor].requests.load();
	run(105, 30); // 3 s: the storm (never-ending by default) is still rolling in
	Check(g_showers[kShowerMeteor].requests.load() == before, "no meteors while the storm is still building");
	run(105, 60);
	Check(g_storms[kStormAsh].state.level == 1.0f && g_showers[kShowerMeteor].requests.load() == before, "at its height the first volley is put on the clock, not dropped at once");
	run(105, 320);
	Check(g_showers[kShowerMeteor].requests.load() == before + 3, "then a volley is asked for every interval (3 in 32 seconds at 10 s apart)");
	run(103, 150);
	run(2, 150);
	Check(g_showers[kShowerMeteor].requests.load() == before + 3, "none in the fortress or in another act");
	g_config.meteor.on = 0;
	run(105, 400);
	Check(g_showers[kShowerMeteor].requests.load() == before + 3, "meteors switched off: none");
	run(-1, 1);
	g_config   = Config {};
	g_skyReady = false;
}

// ---- the sandstorm's dust devils ----
void TestServerDevils() {
	printf("-- server tick: dust devils\n");
	uint8_t* image = ServerImage();
	if (image == nullptr) {
		Check(false, "memory for the stand-in game");
		return;
	}
	const uintptr_t savedBase = g_base;
	g_base                    = reinterpret_cast<uintptr_t>(image);
	g_server                  = ServerFakes {};
	g_config                  = Config {};
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\r\n2\t\t\t1\t0\t0\r\n40\t\t\t0\t1\t3\r\n41\t\t\t3\t0\t0\r\n";
	ParseLevels(table);
	for (ClassList& classes : g_classes) {
		classes.count = 0;
	}
	Check(g_config.devil.countMin == 4 && g_config.devil.countMax == 8, "as shipped, four to eight dust devils stand up together");
	g_config.devil.damage = 3;
	g_config.devil.speed    = 0; // standing ones first
	g_config.devil.countMin = g_config.devil.countMax = 4;
	static uint8_t game[0x200];
	memset(game, 0, sizeof(game));
	game[0x106]    = 3;
	uint32_t frame = 100;
	const auto run = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			memcpy(game + 0x170, &frame, 4);
			ServerTick(game);
			++frame;
		}
	};
	// what was asked of the client side since the last look (and forgets it)
	struct Asked {
		float    drift[2];
		int      effect;
		int32_t  x, y;
		uint32_t holdMs;
		float    scale;
		uint64_t at;
	};
	const auto asked = [] {
		std::vector<Asked> list;
		for (Impact& impact : g_impacts) {
			if (impact.state.load() == kImpactAsked) {
				if (impact.list == kDevilEffects) {
					list.push_back({ { impact.drift[0], impact.drift[1] }, impact.effect, impact.spot[0], impact.spot[1], impact.holdMs, impact.scale, impact.at });
				}
				impact.state = kImpactFree;
				--g_impactsBusy;
			}
		}
		return list;
	};
	const auto home = [] {
		g_server.position[0] = 5000;
		g_server.position[1] = 5000;
		g_server.life        = 100 * 256;
		g_server.lifeSets    = 0;
	};
	g_game = nullptr;
	run(1);
	asked();

	// a group of them
	g_server.level = 41;
	++g_devilRequests;
	uint64_t clock = GetTickCount64();
	run(2);
	std::vector<Asked> list = asked();
	bool right = list.size() == 4;
	bool apart = false;
	for (const Asked& one : list) {
		const double dx = one.x - 5000, dy = one.y - 5000, away = std::sqrt(dx * dx + dy * dy);
		right = right && one.effect == 1 && one.holdMs == 6000 && one.scale == 1.0f && one.at < clock + 500 && away > 4.1 && away < 18.5 && one.drift[0] == 0.0f && one.drift[1] == 0.0f;
		apart = apart || one.x != list[0].x || one.y != list[0].y;
	}
	Check(right && apart && g_devilCount == 4, "dust devils out in the sandstorm: four Tornados at once, each somewhere of its own 6-18 tiles from the player, up for six seconds");
	run(300);
	Check(g_server.shots.empty() && g_server.lifeSets == 0 && g_devilCount == 0 && asked().empty(), "none stands up on the player: one who stays put is not drained, and after six seconds they are gone");

	// any of them drains a player who is in it
	g_config.devil.countMin = g_config.devil.countMax = 1;
	++g_devilRequests;
	run(1);
	list = asked();
	run(10);
	Check(list.size() == 1 && g_server.lifeSets == 0, "one alone, and nothing happens to a player who keeps away");
	if (list.size() == 1) {
		g_server.position[0] = list[0].x;
		g_server.position[1] = list[0].y;
	}
	run(14);
	Check(g_server.lifeSets == 0, "...nor in its first second to one who walks into it");
	run(1);
	Check(g_server.lifeSets == 1 && g_server.life == 97 * 256, "a player inside a dust devil is drained 3% of maximum life");
	run(300);
	Check(g_server.lifeSets == 5 && g_server.life == 85 * 256 && g_devilCount == 0, "...and again every second while it stands, then it is over");

	home();
	g_server.life = 300; // just over 1 life
	++g_devilRequests;
	run(1);
	list = asked();
	if (list.size() == 1) {
		g_server.position[0] = list[0].x;
		g_server.position[1] = list[0].y;
	}
	run(300);
	Check(g_server.life == 256, "a dust devil never kills: 1 life is left");

	home();
	g_config.devil.damage = 0;
	++g_devilRequests;
	run(1);
	list = asked();
	if (list.size() == 1) {
		g_server.position[0] = list[0].x;
		g_server.position[1] = list[0].y;
	}
	run(300);
	Check(list.size() == 1 && g_server.lifeSets == 0, "damage 0: they stand up all the same and drain nothing");
	g_config.devil.damage = 3;

	// they drift: the whirlwind wanders off at a tile and a half a second, and the drain goes with it
	home();
	g_config.devil.speed = 15;
	++g_devilRequests;
	run(1);
	list = asked();
	float drift[2] = { 0.0f, 0.0f };
	if (list.size() == 1) {
		drift[0]             = list[0].drift[0];
		drift[1]             = list[0].drift[1];
		g_server.position[0] = list[0].x;
		g_server.position[1] = list[0].y;
	}
	// (as the renderer counts: two units to one of the game's places, so 3 a second)
	Check(list.size() == 1 && Near(std::sqrt(drift[0] * drift[0] + drift[1] * drift[1]), 3.0f), "a drifting dust devil is asked for with a drift of 1.5 tiles a second");
	{
		Impact up {};
		up.position[0] = 100.0f;
		up.position[1] = 1.0f;
		up.position[2] = 200.0f;
		up.drift[0]    = drift[0];
		up.drift[1]    = drift[1];
		up.upAt        = 5000;
		float xyz[3]   = {};
		ImpactPlace(up, 7000, xyz);
		Check(Near(xyz[0], 100.0f + 2.0f * drift[0]) && xyz[1] == 1.0f && Near(xyz[2], 200.0f + 2.0f * drift[1]), "...which after two seconds has carried it 3 tiles, at the same height");
	}
	run(300);
	Check(g_server.lifeSets == 2 && g_server.life == 94 * 256, "a player who stays where it stood up is drained for two seconds (reach 4 tiles), then it has drifted off them");
	home();
	++g_devilRequests;
	run(1);
	list = asked();
	run(60);
	if (list.size() == 1) { // into its path: where it is three seconds after it stood up
		g_server.position[0] = list[0].x + static_cast<int32_t>(std::lround(list[0].drift[0] / 2.0f * 3.0f));
		g_server.position[1] = list[0].y + static_cast<int32_t>(std::lround(list[0].drift[1] / 2.0f * 3.0f));
	}
	run(20);
	Check(g_server.lifeSets == 1, "...and one who walks into where it has drifted to is drained there");
	run(300);
	asked();
	g_config.devil.speed = 0;

	// none stands up nearer than its reach
	g_config.devil.minDistance = 2;
	g_config.devil.maxDistance = 4;
	g_config.devil.reach       = 10;
	NormalizeConfig();
	Check(g_config.devil.minDistance == 11 && g_config.devil.maxDistance == 11, "the nearest a dust devil stands up is kept outside its reach");
	g_config.devil.minDistance = 6;
	g_config.devil.maxDistance = 18;
	g_config.devil.reach       = 4;

	// only out in the sand
	home();
	for (const int level : { 40, 2 }) {
		g_server.level = level;
		++g_devilRequests;
		run(300);
	}
	Check(asked().empty() && g_server.lifeSets == 0 && g_devilCount == 0, "none in Lut Gholein, or in another act");
	g_config.devil.countMin = g_config.devil.countMax = 3;
	g_devilForce = 1; // "horror devil now"
	run(1);
	Check(asked().size() == 3 && g_devilCount == 3, "horror devil now: a group stands up around the player wherever they are outside town");
	g_server.level = 40;
	g_devilForce   = 1;
	run(300);
	Check(asked().empty(), "...but not in town");

	// a new game: the old one's are gone with it
	g_server.level = 41;
	++g_devilRequests;
	run(1);
	asked();
	frame = 10;
	run(1);
	Check(g_devilCount == 0, "the dust devils of the last game do not carry on in a new one");

	g_config = Config {};
	g_base   = savedBase;
	g_game   = nullptr;
	g_server.shots.clear();
	VirtualFree(image, 0, MEM_RELEASE);
}

void TestDevilClock() {
	printf("-- dust devil clock\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\r\n2\t\t\t1\t0\t0\r\n40\t\t\t0\t1\t3\r\n41\t\t\t3\t0\t0\r\n";
	ParseLevels(table);
	g_config                   = Config {};
	g_config.devil.minInterval = g_config.devil.maxInterval = 10;
	g_skyReady                 = true;
	g_modSandstorm             = 1;
	uint64_t   now             = 12000000;
	const auto run             = [&](int level, int seconds10) {
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
			UpdateDevils(now, level);
		}
	};
	const uint32_t before = g_devilRequests.load();
	run(41, 30); // 3 s: the storm (never-ending by default) is still rolling in
	Check(g_devilRequests.load() == before, "no dust devils while the sandstorm is still building");
	run(41, 60);
	Check(g_storms[kStormSand].state.level == 1.0f && g_devilRequests.load() == before, "at its height the first is put on the clock, not raised at once");
	run(41, 320);
	Check(g_devilRequests.load() == before + 3, "then a group is asked for every interval (3 in 32 seconds at 10 s apart)");
	run(2, 300);
	Check(g_devilRequests.load() == before + 3, "none in another act");
	g_config.devil.on = 0;
	run(41, 400);
	Check(g_devilRequests.load() == before + 3, "dust devils switched off: none");
	run(-1, 1);
	g_config       = Config {};
	g_skyReady     = false;
	g_modSandstorm = 0;
}

// ---- the blizzard's ice ----
void TestServerIce() {
	printf("-- server tick: ice\n");
	uint8_t* image = ServerImage();
	if (image == nullptr) {
		Check(false, "memory for the stand-in game");
		return;
	}
	const uintptr_t savedBase = g_base;
	g_base                    = reinterpret_cast<uintptr_t>(image);
	g_server                  = ServerFakes {};
	g_config                  = Config {};
	g_config.ice.countMin = g_config.ice.countMax = 4;
	g_config.ice.hits     = 0;
	g_config.meteor.countMin = g_config.meteor.countMax = 4;
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\r\n105\t\t\t0\t0\t0\t1\t0\r\n109\t\t\t2\t1\t0\t0\t2\r\n110\t\t\t2\t0\t0\t0\t1\r\n";
	ParseLevels(table);
	for (ClassList& classes : g_classes) {
		classes.count = 0;
	}
	Shower& ice    = g_showers[kShowerIce];
	Shower& meteor = g_showers[kShowerMeteor];
	ice.row[3]       = 760;
	ice.hitRow[3]    = 761;
	ice.skill[3]     = 56;
	meteor.row[3]    = 750;
	meteor.hitRow[3] = 751;
	static uint8_t game[0x200];
	memset(game, 0, sizeof(game));
	game[0x106]    = 3;
	uint32_t frame = 100;
	const auto run = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			memcpy(game + 0x170, &frame, 4);
			ServerTick(game);
			++frame;
		}
	};
	g_game = nullptr;
	run(1);

	g_server.level = 110;
	++ice.requests;
	run(80);
	bool rows = g_server.shots.size() == 4;
	for (const ServerShot& shot : g_server.shots) {
		rows = rows && shot.row == 760;
	}
	Check(rows && g_server.lifeSets == 0, "a volley of ice on Act 5's snowfields: its shards (4) as the ice row, and nobody is hurt");
	// what shows where each lands is asked of the client side: the Blizzard's impact, at the shard's place, when it lands
	{
		int  asked = 0;
		bool right = true;
		const uint64_t clock = GetTickCount64();
		for (const Impact& impact : g_impacts) {
			if (impact.state.load() != kImpactAsked) {
				continue;
			}
			bool placed = false;
			for (const ServerShot& shot : g_server.shots) {
				placed = placed || (shot.x == impact.spot[0] && shot.y == impact.spot[1]);
			}
			right = right && placed && impact.effect == 1 && impact.player[0] == 5000 && impact.player[1] == 5000 && impact.at > clock && impact.at <= clock + kShowerLandMs;
			++asked;
		}
		Check(asked == 4 && right && g_impactsBusy.load() == 4, "an impact is asked for where each shard lands, for when it lands: the Blizzard's own");
		// the client side: the place in the renderer's world (two units to one of the game's), then played when the time comes
		const uintptr_t savedWeather = g_weatherObject;
		const bool      savedReady   = g_fieldReady;
		g_weatherObject = 0;
		UpdateImpacts(clock, 110);
		bool placed = true;
		int  waiting = 0;
		for (const Impact& impact : g_impacts) {
			if (impact.state.load() == kImpactWaiting) {
				++waiting;
				placed = placed && impact.position[0] == 2.0f * static_cast<float>(impact.spot[0]) && impact.position[2] == 2.0f * static_cast<float>(impact.spot[1]);
			}
		}
		Check(waiting == 4 && placed, "each waits for its time, placed in the renderer's world");
		g_fieldReady = false;
		UpdateImpacts(clock + kShowerLandMs + 1, 110);
		Check(g_impactsBusy.load() == 0, "with the game's effect code not as expected nothing is played and nothing is left waiting");
		g_config.ice.impact  = 0;
		g_config.ice.impact2 = 0;
		ice.force            = true;
		g_server.shots.clear();
		run(80);
		Check(g_server.shots.size() == 4 && g_impactsBusy.load() == 0, "impacts switched off: the ice still falls, nothing is asked for");
		g_impactTest = 3;
		run(1);
		int shown = 0;
		for (Impact& impact : g_impacts) {
			if (impact.state.load() == kImpactAsked) {
				shown += impact.effect == 3 && impact.spot[0] == 5002 && impact.spot[1] == 5002 ? 1 : 100;
				impact.state = kImpactFree;
				--g_impactsBusy;
			}
		}
		Check(shown == 1, "horror impact show 3: that effect, a step in front of the player, now");
		// the dust devil preview: show = put up once and removed after five seconds, again = played every 600 ms
		const auto devilAsked = [&](int effect, int* held, float* scale) {
			int count = 0;
			*held     = 0;
			for (Impact& impact : g_impacts) {
				if (impact.state.load() == kImpactAsked) {
					count += impact.list == kDevilEffects && impact.effect == effect && impact.spot[0] == 5005 && impact.spot[1] == 5005 ? 1 : 100;
					*held += impact.holdMs == 5000 ? 1 : 0;
					*scale = impact.scale;
					impact.state = kImpactFree;
					--g_impactsBusy;
				}
			}
			return count;
		};
		int   held  = 0;
		float scale = 0.0f;
		g_devilShow = 1;
		run(1);
		Check(devilAsked(1, &held, &scale) == 1 && held == 1 && scale == 1.0f, "horror devil show 1: the Tornado once, to be removed after five seconds");
		g_devilAgain = 5;
		run(1);
		Check(devilAsked(5, &held, &scale) == 9 && held == 0 && scale == 1.0f && kDevilEffectCount == 28, "horror devil again 5: the crack played nine times over five seconds");
		g_config.ice.impact = 1;
		g_weatherObject     = savedWeather;
		g_fieldReady        = savedReady;
		g_server.shots.clear();
	}
	g_server.shots.clear();
	++meteor.requests;
	run(80);
	Check(g_server.shots.empty(), "no meteors on the snowfields");
	g_server.level = 109;
	++ice.requests;
	run(80);
	g_server.level = 105;
	++ice.requests;
	run(80);
	Check(g_server.shots.empty(), "no ice in Harrogath, or in Act 4");
	ice.force = true;
	run(80);
	Check(g_server.shots.size() == 4 && g_server.shots[0].row == 760, "horror ice drops a volley anywhere outside town");

	g_server.shots.clear();
	const int meteorHits   = meteor.totalHits.load();
	g_server.level        = 110;
	g_config.ice.hits      = 1;
	g_config.ice.hitChance = 100;
	g_config.ice.markerLead = 0;
	++ice.requests;
	run(30);
	Check(!g_server.shots.empty() && g_server.shots[0].row == 761, "an aimed shard: the volley's first is the marker row");
	{
		int markers = 0;
		for (const Impact& impact : g_impacts) {
			if (impact.state.load() == kImpactAsked && impact.effect == 11) {
				markers += impact.holdMs == 0 && impact.scale == 2.0f && impact.spot[0] == g_server.shots[0].x && impact.spot[1] == g_server.shots[0].y ? 1 : 100;
			}
		}
		Check(markers == 5, "...and its marker is asked for there: cracking ice, at once and again every half second until it lands");
	}
	run(60);
	Check(g_server.lifeSets == 1 && g_server.life >= 90 * 256 && g_server.life <= 95 * 256 && ice.totalHits.load() == 1, "a player still standing there when it lands loses 5-10% of maximum life");
	Check(meteor.totalHits.load() == meteorHits, "...counted as the ice's, not the meteors'");
	{
		// the marker's head start: marked at once where the player stands, the shard dropped there 800 ms (20 frames) later
		for (Impact& impact : g_impacts) {
			impact.state = kImpactFree;
		}
		g_impactsBusy = 0;
		g_server.shots.clear();
		g_server.lifeSets       = 0;
		g_server.life           = 100 * 256;
		g_config.ice.markerLead = 800;
		g_config.ice.countMin = g_config.ice.countMax = 1;
		++ice.requests;
		run(10);
		int markers = 0;
		for (const Impact& impact : g_impacts) {
			markers += impact.state.load() == kImpactAsked && impact.effect == 11 && impact.spot[0] == 5000 && impact.spot[1] == 5000 ? 1 : 0;
		}
		Check(markers == 7 && g_server.shots.empty(), "a head start for the marker: it is asked for at once on the player's spot, no shard yet");
		run(15);
		Check(g_server.shots.size() == 1 && g_server.shots[0].row == 761 && g_server.shots[0].x == 5000 && g_server.shots[0].y == 5000 && g_server.lifeSets == 0,
			"...the shard is dropped on the marked spot when the head start is over");
		run(70);
		Check(g_server.lifeSets == 1, "...and hurts a player still there when it lands");
	}

	ice.row[3] = ice.hitRow[3] = meteor.row[3] = meteor.hitRow[3] = -1;
	g_config = Config {};
	g_base   = savedBase;
	g_game   = nullptr;
	VirtualFree(image, 0, MEM_RELEASE);
}

void TestIceClock() {
	printf("-- ice clock\n");
	char table[] = "#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\r\n105\t\t\t0\t0\t0\t1\t0\r\n109\t\t\t2\t1\t0\t0\t2\r\n110\t\t\t2\t0\t0\t0\t1\r\n";
	ParseLevels(table);
	g_config                 = Config {};
	g_config.ice.minInterval = g_config.ice.maxInterval = 10;
	g_skyReady               = true;
	g_modBlizzard            = 1;
	g_modAshstorm            = 1;
	uint64_t   now           = 11000000;
	const auto run           = [&](int level, int seconds10) {
		for (int i = 0; i < seconds10; ++i) {
			now += 100;
			UpdateStorms(now, level);
			UpdateShowers(now, level);
		}
	};
	Shower&        ice     = g_showers[kShowerIce];
	Shower&        meteor  = g_showers[kShowerMeteor];
	const uint32_t before  = ice.requests.load();
	const uint32_t meteors = meteor.requests.load();
	run(110, 30);
	Check(ice.requests.load() == before, "no ice while the blizzard is still building");
	run(110, 380);
	Check(g_storms[kStormBlizzard].state.level == 1.0f && ice.requests.load() == before + 3, "then a volley every interval (3 in about 32 s at 10 s apart)");
	Check(meteor.requests.load() == meteors, "...and no meteors asked for in the blizzard");
	run(109, 300);
	Check(ice.requests.load() == before + 3, "none in Harrogath");
	g_config.ice.on = 0;
	run(110, 400);
	Check(ice.requests.load() == before + 3, "ice switched off: none");
	run(-1, 1);
	g_config      = Config {};
	g_skyReady    = false;
	g_modBlizzard = 0;
}

void TestHauntingLength() {
	printf("-- how long a haunting lasts\n");
	const Config saved = g_config;
	const auto range   = [](int minLife, int maxLife, int* low, int* high) {
		g_config.minLife = minLife;
		g_config.maxLife = maxLife;
		*low             = 1 << 30;
		*high            = 0;
		for (int i = 0; i < 400; ++i) {
			const int seconds = static_cast<int>(HauntingFrames() / 25);
			*low              = seconds < *low ? seconds : *low;
			*high             = seconds > *high ? seconds : *high;
		}
	};
	int low = 0, high = 0;
	range(4, 10, &low, &high);
	Check(low == 8 && high == 10, "short stays: a haunting lasts at least 8 seconds (8-10 s)");
	range(20, 60, &low, &high);
	Check(low >= 20 && high <= 60 && low < 30, "long stays: never under the shortest stay (20-60 s)");
	range(4, 6, &low, &high);
	Check(low == 4 && high == 6, "stays of 8 seconds or less: the configured range (4-6 s)");
	g_config = saved;
}

// ---- storms over terror zones: the lighting blend ----
std::vector<std::pair<float, uint8_t>> g_blends;
void FakeBlend(void*, void*, float weight, uint8_t isEvent) { g_blends.push_back({ weight, isEvent }); }

void TestEventYield() {
	printf("-- storms over lighting events\n");
	g_originalBlend = FakeBlend;
	g_eventYield    = 0.0f;
	BlendHook(nullptr, nullptr, 1.0f, 0); // the time of day
	BlendHook(nullptr, nullptr, 1.0f, 1); // a terror zone's look
	Check(g_blends.size() == 2 && g_blends[0].first == 1.0f && g_blends[1].first == 1.0f && g_blends[1].second == 1, "no storm: every profile goes in as the game asks");
	g_eventYield = 0.6f;
	BlendHook(nullptr, nullptr, 0.7f, 0);
	BlendHook(nullptr, nullptr, 1.0f, 1);
	Check(g_blends[2].first == 0.7f && Near(g_blends[3].first, 0.4f), "a storm at 60%: the event's weight goes down to 40%, the time of day's is left alone");
	g_eventYield = 1.0f;
	BlendHook(nullptr, nullptr, 0.5f, 1);
	Check(g_blends[4].first == 0.0f, "at full strength the event gives way entirely");
	g_eventYield    = 0.0f;
	g_originalBlend = nullptr;
	g_blends.clear();
}

// ---- log lines from a thread of their own ----
struct LoggedLine {
	char  text[64];
	DWORD thread;
};
std::vector<LoggedLine> g_logged;
void __cdecl CaptureLog(const D2RL::PluginContext*, const char* message) noexcept {
	LoggedLine line {};
	snprintf(line.text, sizeof(line.text), "%s", message);
	line.thread = GetCurrentThreadId();
	g_logged.push_back(line);
}

void TestLogThread() {
	printf("-- log lines\n");
	static D2RL::PluginApi api {};
	api.apiSize = D2RL::PluginApiSize;
	api.logInfo = api.logWarn = api.logError = CaptureLog;
	static D2RL::PluginContext context {};
	context.contextSize = static_cast<uint32_t>(sizeof(context));
	context.api         = &api;
	const D2RL::PluginContext* saved = g_ctx;
	g_ctx                            = &context;
	const DWORD self                 = GetCurrentThreadId();

	StartLogThread();
	Log("one");
	Log("two %d", 2);
	Log("three");
	StopLogThread();
	const bool inOrder = g_logged.size() == 3 && strcmp(g_logged[0].text, "one") == 0 && strcmp(g_logged[1].text, "two 2") == 0 && strcmp(g_logged[2].text, "three") == 0;
	bool otherThread = !g_logged.empty();
	for (const LoggedLine& line : g_logged) {
		otherThread = otherThread && line.thread != self;
	}
	Check(inOrder, "every line reaches the loader, in order");
	Check(otherThread, "...from the log thread, not from the thread that logged");
	Log("four");
	Check(g_logged.size() == 4 && g_logged[3].thread == self, "once the thread has stopped, lines are written directly");
	g_ctx = saved;
	g_logged.clear();
}

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	SeedRandom(12345);
	TestConfig(argc > 1 ? argv[1] : "horror-night.toml");
	TestLevels();
	TestMixer();
	TestSandstorm();
	TestAshStorm();
	TestBlizzard();
	TestMonsoon();
	TestRainstorm();
	TestSettingsMenu();
	TestStormSounds();
	TestSky();
	TestWeather();
	TestServerBolts();
	TestServerMeteors();
	TestMeteorClock();
	TestServerDevils();
	TestDevilClock();
	TestServerIce();
	TestIceClock();
	TestHauntingLength();
	TestEventYield();
	TestLogThread();
	printf("%s\n", g_failures == 0 ? "all checks passed" : "SOME CHECKS FAILED");
	return g_failures == 0 ? 0 : 1;
}
