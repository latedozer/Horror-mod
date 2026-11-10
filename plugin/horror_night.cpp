// Horror Night - D2RLoader plugin
//   * phantoms: false monsters that cannot be killed, drain life without killing,
//     and vanish after a random time (server side, one game-frame hook)
//   * storm: lightning flash + thunder in rainy areas (client side, overlay + winmm)
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

void Log(const char* format, ...) {
	if (g_ctx == nullptr) {
		return;
	}
	char    text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	g_ctx->LogInfo(text);
}

uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

uint32_t Rand() {
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return static_cast<uint32_t>(g_rng >> 16);
}

int RandRange(int low, int high) {
	if (high <= low) {
		return low;
	}
	return low + static_cast<int>(Rand() % static_cast<uint32_t>(high - low + 1));
}

// ---------------------------------------------------------------- config

struct Config {
	int phantomsEnabled = 1;
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

	int ambientEnabled   = 1;
	int ambientMin       = 70;
	int ambientMax       = 200;
	int ambientVolume    = 60;
	int heartbeat        = 0;  // off: removed at the user's request (setting kept for anyone who wants it)
	int heartbeatBelow   = 35; // percent of life
	int heartbeatVolume  = 130;

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

void ApplyConfigValue(const char* section, const char* key, const char* value) {
	int number = 0;
	if (strncmp(value, "true", 4) == 0) {
		number = 1;
	} else if (strncmp(value, "false", 5) == 0) {
		number = 0;
	} else {
		number = atoi(value);
	}

	struct Entry {
		const char* section;
		const char* key;
		int*        target;
	};
	const Entry entries[] = {
		{ "phantoms", "enabled", &g_config.phantomsEnabled },
		{ "phantoms", "min_interval", &g_config.minInterval },
		{ "phantoms", "max_interval", &g_config.maxInterval },
		{ "phantoms", "min_life", &g_config.minLife },
		{ "phantoms", "max_life", &g_config.maxLife },
		{ "phantoms", "max_alive", &g_config.maxAlive },
		{ "phantoms", "max_group", &g_config.maxGroup },
		{ "phantoms", "drain_percent", &g_config.drainPercent },
		{ "phantoms", "min_distance", &g_config.minDistance },
		{ "phantoms", "max_distance", &g_config.maxDistance },
		{ "haunting", "chance", &g_config.hauntChance },
		{ "haunting", "corpses", &g_config.riseMax },
		{ "haunting", "false_health", &g_config.falseHealth },
		{ "weather", "enabled", &g_config.weatherEnabled },
		{ "weather", "intensity", &g_config.weatherIntensity },
		{ "sounds", "enabled", &g_config.ambientEnabled },
		{ "sounds", "min_interval", &g_config.ambientMin },
		{ "sounds", "max_interval", &g_config.ambientMax },
		{ "sounds", "volume", &g_config.ambientVolume },
		{ "sounds", "heartbeat", &g_config.heartbeat },
		{ "sounds", "heartbeat_below", &g_config.heartbeatBelow },
		{ "sounds", "heartbeat_volume", &g_config.heartbeatVolume },
		{ "storm", "enabled", &g_config.stormEnabled },
		{ "storm", "min_interval", &g_config.stormMinInterval },
		{ "storm", "max_interval", &g_config.stormMaxInterval },
		{ "storm", "lightning_bolts", &g_config.flashStrength },
		{ "storm", "screen_flash", &g_config.screenFlash },
		{ "storm", "bolt_row", &g_config.boltRow },
		{ "storm", "hits_min", &g_config.hitsMin },
		{ "storm", "hits_max", &g_config.hitsMax },
		{ "storm", "bolts_per_hit", &g_config.boltsPerHit },
		{ "storm", "bolt_spread", &g_config.boltSpread },
		{ "storm", "bolt_min_distance", &g_config.boltMinDistance },
		{ "storm", "direct_hit_chance", &g_config.directHitChance },
		{ "storm", "direct_hit_damage_min", &g_config.directHitDamage },
		{ "storm", "direct_hit_damage_max", &g_config.directHitDamageMax },
		{ "storm", "sky_flash", &g_config.skyFlash },
		{ "storm", "sky_flash_strength", &g_config.skyStrength },
		{ "storm", "volume", &g_config.volume },
		{ "storm", "snow_areas", &g_config.snowAreas },
		{ "storm", "indoor_thunder", &g_config.indoorThunder },
	};
	for (const Entry& entry : entries) {
		if (strcmp(entry.section, section) == 0 && strcmp(entry.key, key) == 0) {
			*entry.target = number;
			return;
		}
	}
}

void NormalizeConfig();

void LoadConfig() {
	static char text[16384];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		Log("config: could not read the config file, using defaults");
		return;
	}

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

void NormalizeConfig() {
	g_config.minInterval      = Clamp(g_config.minInterval, 5, 3600);
	g_config.maxInterval      = Clamp(g_config.maxInterval, g_config.minInterval, 7200);
	g_config.minLife          = Clamp(g_config.minLife, 2, 600);
	g_config.maxLife          = Clamp(g_config.maxLife, g_config.minLife, 900);
	g_config.maxAlive         = Clamp(g_config.maxAlive, 1, 12);
	g_config.maxGroup         = Clamp(g_config.maxGroup, 1, g_config.maxAlive);
	g_config.drainPercent     = Clamp(g_config.drainPercent, 1, 50);
	g_config.minDistance      = Clamp(g_config.minDistance, 4, 30);
	g_config.maxDistance      = Clamp(g_config.maxDistance, g_config.minDistance, 34);
	g_config.hauntChance      = Clamp(g_config.hauntChance, 0, 100);
	g_config.riseMax          = Clamp(g_config.riseMax, 0, 8);
	g_config.weatherIntensity = Clamp(g_config.weatherIntensity, 5, 100);
	g_config.ambientMin       = Clamp(g_config.ambientMin, 3, 3600);
	g_config.ambientMax       = Clamp(g_config.ambientMax, g_config.ambientMin, 7200);
	g_config.ambientVolume    = Clamp(g_config.ambientVolume, 0, 300);
	g_config.heartbeatBelow   = Clamp(g_config.heartbeatBelow, 1, 100);
	g_config.heartbeatVolume  = Clamp(g_config.heartbeatVolume, 0, 300);
	g_config.stormMinInterval = Clamp(g_config.stormMinInterval, 3, 3600);
	g_config.stormMaxInterval = Clamp(g_config.stormMaxInterval, g_config.stormMinInterval, 7200);
	g_config.flashStrength    = Clamp(g_config.flashStrength, 0, 1);
	g_config.volume           = Clamp(g_config.volume, 0, 300);
	g_config.hitsMin          = Clamp(g_config.hitsMin, 1, 12);
	g_config.hitsMax          = Clamp(g_config.hitsMax, g_config.hitsMin, 12);
	g_config.boltsPerHit      = Clamp(g_config.boltsPerHit, 1, 8);
	g_config.boltSpread       = Clamp(g_config.boltSpread, 4, 30);
	g_config.boltMinDistance  = Clamp(g_config.boltMinDistance, 2, g_config.boltSpread);
	g_config.directHitChance  = Clamp(g_config.directHitChance, 0, 100);
	g_config.directHitDamage  = Clamp(g_config.directHitDamage, 0, 100);
	g_config.directHitDamageMax = Clamp(g_config.directHitDamageMax, g_config.directHitDamage, 100);
	g_config.skyStrength      = Clamp(g_config.skyStrength, 0, 100);
}

// ---------------------------------------------------------------- I Hate TOMLs
// The "I Hate TOMLs" plugin offers a "settings-menu" service: a plugin hands it a small
// TOML description of its settings and two callbacks (read a value as text, write a value),
// and gets a page in the in-game settings window. Layout read from plugins that use it:
//   table:        { u32 size; u32 version; handle (*add)(context, const Registration*); ... }
//   Registration: { u32 size = 0x28; u32 flags; const char* description; get; set; reserved }
//   get(key, out, outSize) -> found      set(key, value, error, errorSize) -> accepted
// Every change is applied at once and written back to horror-night.toml.

struct MenuRow {
	const char* section; // nullptr = a header line
	const char* key;
	int*        target;
	bool        isBool;
	const char* label;
	const char* note;
	int         min;
	int         max;
	int         step;
};

const MenuRow kMenu[] = {
	{ nullptr, nullptr, nullptr, false, "Phantoms", nullptr, 0, 0, 0 },
	{ "phantoms", "enabled", &g_config.phantomsEnabled, true, "Phantoms appear", "False monsters: they cannot be killed, their hits drain life but never kill, and they vanish.", 0, 0, 0 },
	{ "phantoms", "min_interval", &g_config.minInterval, false, "Shortest wait (seconds)", "Time between appearances is random between the two.", 5, 3600, 5 },
	{ "phantoms", "max_interval", &g_config.maxInterval, false, "Longest wait (seconds)", nullptr, 5, 7200, 5 },
	{ "phantoms", "min_life", &g_config.minLife, false, "Shortest stay (seconds)", "How long a phantom stays before it vanishes.", 2, 600, 1 },
	{ "phantoms", "max_life", &g_config.maxLife, false, "Longest stay (seconds)", nullptr, 2, 900, 1 },
	{ "phantoms", "max_alive", &g_config.maxAlive, false, "Most at once", nullptr, 1, 12, 1 },
	{ "phantoms", "max_group", &g_config.maxGroup, false, "Most appearing together", nullptr, 1, 12, 1 },
	{ "phantoms", "drain_percent", &g_config.drainPercent, false, "Life drained per hit (%)", "Percent of your current life. It can never kill.", 1, 50, 1 },
	{ "phantoms", "min_distance", &g_config.minDistance, false, "Nearest they appear (tiles)", nullptr, 4, 30, 1 },
	{ "phantoms", "max_distance", &g_config.maxDistance, false, "Farthest they appear (tiles)", nullptr, 4, 34, 1 },

	{ nullptr, nullptr, nullptr, false, "Hauntings", nullptr, 0, 0, 0 },
	{ "haunting", "chance", &g_config.hauntChance, false, "Haunting chance (%)", "Share of phantom appearances where one of the dead wears a townsperson's name.", 0, 100, 1 },
	{ "haunting", "corpses", &g_config.riseMax, false, "Corpses that get back up", nullptr, 0, 8, 1 },
	{ "haunting", "false_health", &g_config.falseHealth, true, "Life orb reads 1", "Only what the orb shows; your real life is untouched.", 0, 0, 0 },

	{ nullptr, nullptr, nullptr, false, "Weather", nullptr, 0, 0, 0 },
	{ "weather", "enabled", &g_config.weatherEnabled, true, "Always rain or snow outdoors", "Rain in Acts 1 and 3, snow in Act 5.", 0, 0, 0 },
	{ "weather", "intensity", &g_config.weatherIntensity, false, "How heavy (%)", nullptr, 5, 100, 5 },

	{ nullptr, nullptr, nullptr, false, "Whispers", nullptr, 0, 0, 0 },
	{ "sounds", "enabled", &g_config.ambientEnabled, true, "Whispers and distant sounds", "Breathing, moans, screams and ravens outside town.", 0, 0, 0 },
	{ "sounds", "min_interval", &g_config.ambientMin, false, "Shortest wait (seconds)", nullptr, 3, 3600, 5 },
	{ "sounds", "max_interval", &g_config.ambientMax, false, "Longest wait (seconds)", nullptr, 3, 7200, 5 },
	{ "sounds", "volume", &g_config.ambientVolume, false, "Volume (%)", "100 is as recorded.", 0, 300, 5 },

	{ nullptr, nullptr, nullptr, false, "Storm", nullptr, 0, 0, 0 },
	{ "storm", "enabled", &g_config.stormEnabled, true, "Lightning and thunder", "In the rainy outdoor areas of Acts 1 and 3.", 0, 0, 0 },
	{ "storm", "min_interval", &g_config.stormMinInterval, false, "Shortest wait (seconds)", "Time between strikes is random between the two.", 3, 3600, 5 },
	{ "storm", "max_interval", &g_config.stormMaxInterval, false, "Longest wait (seconds)", nullptr, 3, 7200, 5 },
	{ "storm", "volume", &g_config.volume, false, "Thunder volume (%)", nullptr, 0, 300, 5 },
	{ "storm", "sky_flash", &g_config.skyFlash, true, "Lightning lights the whole scene", "Also holds the game on night.", 0, 0, 0 },
	{ "storm", "sky_flash_strength", &g_config.skyStrength, false, "Flash brightness (%)", nullptr, 0, 100, 5 },
	{ "storm", "lightning_bolts", &g_config.flashStrength, true, "Lightning bolts", "A harmless bolt lands near you on each strike.", 0, 0, 0 },
	{ "storm", "hits_min", &g_config.hitsMin, false, "Fewest flickers per strike", nullptr, 1, 12, 1 },
	{ "storm", "hits_max", &g_config.hitsMax, false, "Most flickers per strike", nullptr, 1, 12, 1 },
	{ "storm", "bolts_per_hit", &g_config.boltsPerHit, false, "Bolts per flicker", nullptr, 1, 8, 1 },
	{ "storm", "bolt_min_distance", &g_config.boltMinDistance, false, "Nearest a bolt lands (tiles)", nullptr, 2, 30, 1 },
	{ "storm", "bolt_spread", &g_config.boltSpread, false, "Farthest a bolt lands (tiles)", nullptr, 4, 30, 1 },
	{ "storm", "direct_hit_chance", &g_config.directHitChance, false, "Strikes that hit you (%)", nullptr, 0, 100, 1 },
	{ "storm", "direct_hit_damage_min", &g_config.directHitDamage, false, "Direct hit: least damage (% of max life)", "A direct hit always leaves at least 1 life.", 0, 100, 1 },
	{ "storm", "direct_hit_damage_max", &g_config.directHitDamageMax, false, "Direct hit: most damage (% of max life)", nullptr, 0, 100, 1 },
	{ "storm", "snow_areas", &g_config.snowAreas, true, "Storms in the snow of Act 5 too", nullptr, 0, 0, 0 },
	{ "storm", "indoor_thunder", &g_config.indoorThunder, true, "Muffled thunder underground", nullptr, 0, 0, 0 },
};

struct MenuRegistration {
	uint32_t    structSize;
	uint32_t    flags;
	const char* description;
	bool (*get)(const char* key, char* out, uint32_t outSize);
	bool (*set)(const char* key, const char* value, char* error, uint32_t errorSize);
	void* reserved;
};

char             g_menuText[12288];
MenuRegistration g_menuRegistration {};
bool             g_menuJoined = false;

const MenuRow* MenuFind(const char* key) {
	if (key == nullptr) {
		return nullptr;
	}
	for (const MenuRow& row : kMenu) {
		if (row.section == nullptr) {
			continue;
		}
		const size_t sectionLength = strlen(row.section);
		if (strncmp(key, row.section, sectionLength) == 0 && key[sectionLength] == '_' && strcmp(key + sectionLength + 1, row.key) == 0) {
			return &row;
		}
	}
	return nullptr;
}

bool MenuGet(const char* key, char* out, uint32_t outSize) {
	const MenuRow* row = MenuFind(key);
	if (row == nullptr || out == nullptr || outSize == 0) {
		return false;
	}
	if (row->isBool) {
		snprintf(out, outSize, "%s", *row->target != 0 ? "true" : "false");
	} else {
		snprintf(out, outSize, "%d", *row->target);
	}
	return true;
}

// Rewrites one "key = value" line of horror-night.toml (inside its [section]); adds the
// line, or the section, when an older file does not have it.
void SaveConfigValue(const char* section, const char* key, const char* value) {
	static char text[20000];
	static char result[24000];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		return;
	}
	const size_t keyLength = strlen(key);
	size_t       used      = 0;
	bool         inSection = false;
	bool         done      = false;
	bool         seen      = false;
	const char*  line      = text;
	auto         append    = [&](const char* data, size_t length) {
		if (used + length < sizeof(result) - 1) {
			memcpy(result + used, data, length);
			used += length;
		}
	};
	while (*line != '\0') {
		const char* end = strchr(line, '\n');
		const size_t length = end != nullptr ? static_cast<size_t>(end - line) + 1 : strlen(line);
		const char* c = line;
		while (*c == ' ' || *c == '\t') {
			++c;
		}
		if (*c == '[') {
			if (inSection && !done) {
				char added[160];
				const int n = snprintf(added, sizeof(added), "%s = %s\r\n", key, value);
				append(added, static_cast<size_t>(n));
				done = true;
			}
			const size_t sectionLength = strlen(section);
			inSection = strncmp(c + 1, section, sectionLength) == 0 && c[1 + sectionLength] == ']';
			seen      = seen || inSection;
		} else if (inSection && !done && strncmp(c, key, keyLength) == 0) {
			const char* after = c + keyLength;
			while (*after == ' ' || *after == '\t') {
				++after;
			}
			if (*after == '=') {
				char replaced[160];
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
		char added[200];
		const int n = seen ? snprintf(added, sizeof(added), "%s = %s\r\n", key, value) : snprintf(added, sizeof(added), "\r\n[%s]\r\n%s = %s\r\n", section, key, value);
		append(added, static_cast<size_t>(n));
	}
	result[used] = '\0';
	static_cast<void>(g_ctx->WriteConfig(result));
}

bool MenuSet(const char* key, const char* value, char* error, uint32_t errorSize) {
	const MenuRow* row = MenuFind(key);
	if (row == nullptr || value == nullptr) {
		if (error != nullptr && errorSize > 0) {
			snprintf(error, errorSize, "unknown setting");
		}
		return false;
	}
	while (*value == ' ' || *value == '"') {
		++value;
	}
	int number = 0;
	if (row->isBool) {
		number = (strncmp(value, "true", 4) == 0 || value[0] == '1') ? 1 : 0;
	} else {
		number = Clamp(atoi(value), row->min, row->max);
	}
	*row->target = number;
	NormalizeConfig();
	char text[32];
	if (row->isBool) {
		snprintf(text, sizeof(text), "%s", *row->target != 0 ? "true" : "false");
	} else {
		snprintf(text, sizeof(text), "%d", *row->target);
	}
	SaveConfigValue(row->section, row->key, text);
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

	size_t used = static_cast<size_t>(snprintf(g_menuText, sizeof(g_menuText), "title = \"Horror Night\"\n"));
	for (const MenuRow& row : kMenu) {
		if (used > sizeof(g_menuText) - 600) {
			break;
		}
		used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "[[setting]]\n"));
		if (row.section == nullptr) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "type = \"header\"\nlabel = \"%s\"\n", row.label));
			continue;
		}
		used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "key = \"%s_%s\"\ntype = \"%s\"\nlabel = \"%s\"\n", row.section, row.key,
			row.isBool ? "bool" : "int", row.label));
		if (row.note != nullptr) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "note = \"%s\"\n", row.note));
		}
		if (!row.isBool) {
			used += static_cast<size_t>(snprintf(g_menuText + used, sizeof(g_menuText) - used, "min = %d\nmax = %d\nstep = %d\nshift_step = %d\nctrl_step = %d\n", row.min, row.max,
				row.step, row.step * 5, row.step * 10));
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
	kMissileCreate, kPlayerClient, kSendMissile, kCastId, kUnitOwner, kUnitDistance, kUnitDirection, kFunctionCount
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
int                   g_flashLogs    = 0;
int                   g_lightningRow[4] = { -1, -1, -1, -1 }; // missiles row by data bank
int                   g_lightningSkill[4] = { 57, 57, 57, 57 }; // the row's Skill (Thunder Storm)
std::atomic<int>      g_testRow { -1 };        // "horror test <row>": fire this missiles row once



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
	return level == 1 || level == 40 || level == 75 || level == 103 || level == 109;
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
std::atomic<int>      g_lifePercent { 100 };
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
bool StartHaunting(Game* game, const ClassList* classes, Unit* player, uint32_t frame, int level) {
	const int townsfolk = PickPhantomClass(game, classes, level, ActOfLevel(level));
	Room*     playerRoom = Engine<UnitRoomFn>(kUnitRoom)(player);
	if (townsfolk < 0 || playerRoom == nullptr) {
		return false;
	}
	int32_t origin[2] = { 0, 0 };
	Engine<UnitPosFn>(kUnitPos)(player, origin);
	const uint32_t life = static_cast<uint32_t>(RandRange(g_config.maxLife > 8 ? 8 : g_config.minLife, g_config.maxLife)) * 25;

	bool placed = false;
	for (int attempt = 0; attempt < 8 && !placed; ++attempt) {
		const double angle    = (Rand() % 6283) / 1000.0;
		const int    distance = RandRange(g_config.minDistance, g_config.maxDistance);
		placed = SpawnPhantomAt(game, playerRoom, origin[0] + static_cast<int32_t>(std::cos(angle) * distance), origin[1] + static_cast<int32_t>(std::sin(angle) * distance),
			townsfolk, frame, life, "townsfolk");
	}
	if (!placed) {
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
	}
	g_lastFrame = frame;

	const ClassList* classes = ClassesFor(game);
	if (classes == nullptr) {
		return;
	}

	// Most frames have nothing to do: no phantom alive, no strike in progress, no spawn due.
	if (g_phantomCount == 0 && g_flashHitsLeft <= 0 && g_flashRequests.load(std::memory_order_relaxed) == g_flashSeen && frame < g_nextSpawn
		&& g_testRow.load(std::memory_order_relaxed) < 0 && !g_forceSpawn.load(std::memory_order_relaxed) && !g_forceHaunt.load(std::memory_order_relaxed)) {
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
					Engine<KillFn>(kKill)(game, unit, nullptr, 0);
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
	if (players.count == 0 || g_phantomCount >= g_config.maxAlive) {
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
		Log("storm: bolt row %d skill %d at %d,%d (player %d,%d) -> %s, server unit id %u mode %u", missileRow, skill, x, y, origin[0], origin[1],
			missile != nullptr ? "created" : "FAILED", missile != nullptr ? UnitId(missile) : 0, missile != nullptr ? UnitMode(missile) : 0);
	}
}

// A direct hit: a share of MAXIMUM life, but it always leaves at least 1 life.
void LightningDamage(Unit* player) {
	const int percent = RandRange(g_config.directHitDamage, g_config.directHitDamageMax);
	if (percent <= 0) {
		return;
	}
	const int32_t life    = Engine<GetStatFn>(kGetStat)(player, 6, 0);
	const int32_t maxLife = Engine<GetStatFn>(kGetStat)(player, 7, 0);
	if (life <= 256 || maxLife <= 0) {
		return;
	}
	int64_t left = life - static_cast<int64_t>(maxLife) * percent / 100;
	if (left < 256) {
		left = 256;
	}
	Engine<SetStatFn>(kSetStat)(player, 6, static_cast<int32_t>(left), 0);
	static int logs = 0;
	if (++logs <= 10) {
		Log("storm: DIRECT HIT %d%% - life %d -> %d of max %d (1/256 units)", percent, life, static_cast<int32_t>(left), maxLife);
	}
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
	int&            hitsLeft = g_flashHitsLeft;
	static uint32_t nextHit  = 0;
	static bool     directHit = false; // this strike lands on the players
	static bool     firstFlicker = false;
	static int32_t  spots[kMaxPlayers][8][2]; // where this strike's bolts land: only the first flicker is random
	const uint32_t  frame    = GameFrame(game);
	const uint32_t  requests = g_flashRequests.load();
	if (requests != g_flashSeen) {
		g_flashSeen = requests;
		hitsLeft    = RandRange(g_config.hitsMin, g_config.hitsMax);
		nextHit     = frame;
		directHit   = static_cast<int>(Rand() % 100) < g_config.directHitChance;
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

void FindLightningRow(const D2RL::PluginContext* context, uint32_t bank) {
	g_lightningRow[bank] = -1;
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
		if (param4 == 7777 && param5 == 4242) {
			g_lightningRow[bank] = static_cast<int>(row);
			int16_t skill = -1;
			if (view.rowSize >= 0x1b8) {
				memcpy(&skill, rows + static_cast<size_t>(row) * view.rowSize + 0x1b6, 2);
			}
			g_lightningSkill[bank] = skill > 0 ? skill : 57;
			break;
		}
	}
	Log("storm: missiles bank %u has %u rows of 0x%x bytes, lightning row %d, skill %d", bank, view.rowCount, view.rowSize, g_lightningRow[bank], g_lightningSkill[bank]);
}

// Reads <mod>\<mod>.mpq\data\global\excel\d2rloader\horror-night\levels.tsv, written by the D2RMM mod:
// level id, monsters on Normal, monsters on Nightmare/Hell (monstats row numbers).
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
		Log("phantoms: could not open %ls (run Install in D2RMM with the Horror Night mod's phantoms on)", file);
		return;
	}
	static char text[262144];
	DWORD       read = 0;
	const BOOL  ok   = ReadFile(handle, text, sizeof(text) - 1, &read, nullptr);
	CloseHandle(handle);
	if (!ok) {
		return;
	}
	text[read] = '\0';

	memset(g_levelMonsters, 0, sizeof(g_levelMonsters));
	g_levelsLoaded = 0;
	char* line = text;
	while (*line != '\0') {
		char* end = line;
		while (*end != '\0' && *end != '\n') {
			++end;
		}
		const bool last = *end == '\0';
		*end            = '\0';
		if (*line >= '0' && *line <= '9') {
			const int level = atoi(line);
			char*     field = line;
			for (int column = 0; column < 2; ++column) {
				field = strchr(field, '\t');
				if (field == nullptr) {
					break;
				}
				++field;
				if (level <= 0 || level >= kMaxLevels) {
					continue;
				}
				LevelMonsters& monsters = g_levelMonsters[column][level];
				const char*    c        = field;
				while (*c != '\0' && *c != '\t' && *c != '\r' && monsters.count < kMaxPerLevel) {
					if (*c >= '0' && *c <= '9') {
						monsters.bases[monsters.count++] = static_cast<int16_t>(atoi(c));
						while (*c >= '0' && *c <= '9') {
							++c;
						}
					} else {
						++c;
					}
				}
			}
			++g_levelsLoaded;
		}
		if (last) {
			break;
		}
		line = end + 1;
	}
	Log("phantoms: %d levels with their monsters loaded (level 2 on Normal: %d monsters)", g_levelsLoaded, g_levelMonsters[0][2].count);
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
		const auto* rows = static_cast<const uint8_t*>(view.rows);
		for (uint32_t row = 0; row < view.rowCount; ++row) {
			int16_t aip8[3];
			memcpy(aip8, rows + static_cast<size_t>(row) * kMonstatsSize + kAip8Offset, sizeof(aip8));
			if (aip8[0] == kPhantomMark && aip8[2] >= kPhantomMark && aip8[2] <= kPhantomMark + 5 && aip8[1] >= 0 && g_classes[bank].count < kMaxClasses) {
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
struct Clip {
	const int16_t* samples = nullptr;
	uint32_t       count   = 0;
	uint32_t       rate    = 22050;
};
Clip              g_thunder[kThunderCount];
Clip              g_ambient[kAmbientCount];
Clip              g_heartbeatClip;
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
	const bool rainOutdoor = (level >= 1 && level <= 7) || level == 17 || level == 38 || level == 39 || (level >= 75 && level <= 83);
	const bool snowOutdoor = (level >= 109 && level <= 112) || level == 117 || level == 120 || level == 121;
	if (rainOutdoor || (snowOutdoor && g_config.snowAreas != 0)) {
		return Area::Outdoor;
	}
	if (g_config.indoorThunder != 0) {
		const bool rainIndoor = (level >= 8 && level <= 37) || (level >= 84 && level <= 102);
		const bool snowIndoor = level >= 113 && level <= 135 && !snowOutdoor;
		if (rainIndoor || (snowIndoor && g_config.snowAreas != 0)) {
			return Area::Indoor;
		}
	}
	return Area::None;
}

// lub-dub: two thumps with enough overtone to be heard on small speakers
void BuildHeartbeat() {
	constexpr uint32_t kRate  = 22050;
	constexpr uint32_t kCount = kRate * 55 / 100;
	static int16_t     buffer[kCount];
	for (uint32_t i = 0; i < kCount; ++i) {
		const double t     = static_cast<double>(i) / kRate;
		double       value = 0.0;
		for (const double start : { 0.0, 0.24 }) {
			if (t >= start) {
				const double local    = t - start;
				const double strength = start == 0.0 ? 1.0 : 0.8;
				const double pitch    = start == 0.0 ? 95.0 : 115.0;
				const double envelope = std::exp(-local * 16.0) * (1.0 - std::exp(-local * 400.0));
				value += strength * envelope
					* (std::sin(local * 2.0 * 3.14159265 * pitch) + 0.55 * std::sin(local * 2.0 * 3.14159265 * pitch * 2.0) + 0.25 * std::sin(local * 2.0 * 3.14159265 * pitch * 3.0));
			}
		}
		value     = std::tanh(value * 1.6);
		buffer[i] = static_cast<int16_t>(value * 30000.0);
	}
	g_heartbeatClip.samples = buffer;
	g_heartbeatClip.count   = kCount;
	g_heartbeatClip.rate    = kRate;
}

// The sounds are the game's own (Blizzard's), so the DLL does not carry them: they are read out of
// the installed game's data at start-up (see game_sounds.cpp).
const char* const kAmbientNames[kAmbientCount] = { "breath1", "breath3", "breath6", "breath8", "moan1", "moan2", "moan3", "moan4", "moan5", "moan6",
	"moan7", "moan8", "scream1", "scream3", "scream5", "scream7", "raven1", "raven3", nullptr /* Diablo's laugh */ };

bool LoadGameClip(void* storage, const char* name, const SoundProcess& process, Clip* clip) {
	uint32_t       count   = 0;
	const int16_t* samples = LoadGameSound(storage, name, process, &count);
	if (samples == nullptr) {
		Log("sounds: could not read %s from the game data", name);
		return false;
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
		CloseGameStorage(storage);
		Log("sounds: %d of %d read from the game data in %llu ms", loaded, kThunderCount + kAmbientCount,
			static_cast<unsigned long long>(GetTickCount64() - started));
	}
	BuildHeartbeat();
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

struct PlayRequest {
	const Clip* clip;
	float       gain;
};
SRWLOCK           g_playLock = SRWLOCK_INIT;
PlayRequest       g_requests[16];
int               g_requestCount = 0;
HANDLE            g_mixEvent     = nullptr; // auto-reset: a request came in, or the device finished a buffer
HANDLE            g_mixThread    = nullptr;
std::atomic<bool> g_mixStop { false };

struct Voice {
	const Clip* clip;
	uint64_t    position; // in clip samples, 32.32 fixed point
	uint64_t    step;
	float       gain;
};

DWORD WINAPI MixerThread(void*) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // little work, but it must not starve
	HWAVEOUT     device = nullptr;
	WAVEHDR      headers[kMixBuffers] {};
	static int16_t buffers[kMixBuffers][kMixBlock];
	float        mix[kMixBlock];
	Voice        voices[kMaxVoices];
	int          voiceCount = 0;
	uint32_t     idle       = 0;
	bool         failedLogged = false;

	while (!g_mixStop.load(std::memory_order_relaxed)) {
		WaitForSingleObject(g_mixEvent, INFINITE);
		if (g_mixStop.load(std::memory_order_relaxed)) {
			break;
		}
		AcquireSRWLockExclusive(&g_playLock);
		for (int i = 0; i < g_requestCount && voiceCount < kMaxVoices; ++i) {
			const Clip* clip = g_requests[i].clip;
			voices[voiceCount++] = Voice { clip, 0, (static_cast<uint64_t>(clip->rate) << 32) / kMixRate, g_requests[i].gain };
		}
		g_requestCount = 0;
		ReleaseSRWLockExclusive(&g_playLock);

		if (device == nullptr) {
			if (voiceCount == 0) {
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
		}

		// refill every buffer the device has finished with
		for (int b = 0; b < kMixBuffers; ++b) {
			if ((headers[b].dwFlags & WHDR_DONE) == 0) {
				continue;
			}
			memset(mix, 0, sizeof(mix));
			for (int v = 0; v < voiceCount;) {
				Voice&         voice   = voices[v];
				const int16_t* samples = voice.clip->samples;
				const uint32_t count   = voice.clip->count;
				const bool     soft    = voice.gain > 1.0f; // above full strength the peaks are rounded off, not clipped
				const float    scale   = voice.gain / 32768.0f;
				uint32_t       i       = 0;
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
				if (i < kMixBlock) {
					voices[v] = voices[--voiceCount]; // finished
				} else {
					++v;
				}
			}
			for (uint32_t i = 0; i < kMixBlock; ++i) {
				const float value = mix[i] > 1.0f ? 1.0f : mix[i] < -1.0f ? -1.0f : mix[i];
				buffers[b][i]     = static_cast<int16_t>(value * 32767.0f);
			}
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
	PlayClip(g_thunder[Rand() % kThunderCount], gain * static_cast<float>(g_config.volume) / 100.0f);
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
	g_peakCount = RandRange(g_config.hitsMin, g_config.hitsMax);
	uint32_t at = 0;
	for (int i = 0; i < g_peakCount; ++i) {
		g_peaks[i].startMs  = at;
		g_peaks[i].strength = i == 0 ? 1.0f : static_cast<float>(RandRange(55, 100)) / 100.0f;
		at += static_cast<uint32_t>(RandRange(90, 300));
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

void UpdateSky(uint64_t now, bool inGame) {
	if (!g_skyReady || g_config.skyFlash == 0) {
		return;
	}
	if (!inGame) {
		if (g_skyHolding) {
			SkyWrite(5, 0.0f, 0); // give the clock back to the game
			g_skyHolding = false;
		}
		g_peakCount = 0;
		return;
	}
	g_skyHolding = true;
	const float intensity = SkyIntensity(now) * static_cast<float>(g_config.skyStrength) / 100.0f;
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

// ---- real weather ----------------------------------------------------------------
// The HD weather object (WeatherTranslation) listens to the old 2D game's rain events.
// It is found through its "start precipitation" handler in the event hub (static object
// at 0x2a9aef0, handler list at +0x388, 0x30-byte entries {vtable, function, flag,
// object}). Its debug switch at +0x7a1 makes it ignore the 2D
// game, and update(object, {type, intensity}, debug = 1) starts or adjusts the effect.
using WeatherFn = void (*)(void* self, void* params, uint8_t fromDebug);
constexpr uint32_t kWeatherHub    = 0x2a9aef0;
constexpr uint32_t kWeatherStart  = 0x7cb830;
constexpr uint32_t kWeatherUpdate = 0x7cb6b0;
constexpr uint32_t kWeatherStop   = 0x7cba70;

bool      g_weatherReady = false;
bool      g_weatherHeld  = false; // we switched the override on
bool      g_weatherOn    = false; // we started precipitation
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

// 0 rain, 1 snow, -1 nothing falls here
int WeatherFor(int level) {
	if ((level >= 1 && level <= 7) || level == 17 || level == 38 || level == 39 || (level >= 75 && level <= 83)) {
		return 0;
	}
	if ((level >= 109 && level <= 112) || level == 117 || level == 120 || level == 121) {
		return 1;
	}
	return -1;
}

uintptr_t g_weatherObject    = 0;  // found once, then only checked
int       g_weatherSetType   = -2; // what the last update call asked for...
int       g_weatherSetLevel  = -2; // ...in which level...
int       g_weatherSetAmount = -1; // ...at which intensity
uint64_t  g_weatherReassert  = 0;

// Once a second at most, and normally one memory check: the weather object is remembered, and
// the game's update is only called when the wanted weather changes (and every 10 s, in case
// the game stopped it on its own).
void UpdateWeather(uint64_t now, int level) {
	if (!g_weatherReady || g_config.weatherEnabled == 0 || now < g_weatherNext) {
		return;
	}
	g_weatherNext     = now + 1000;
	const bool inGame = level > 0;
	if (!inGame && !g_weatherHeld) {
		return;
	}
	uint8_t flags[2] = { 2, 2 }; // +0x7a0 effects loaded, +0x7a1 override
	if (g_weatherObject != 0 && (!SafeRead(g_weatherObject + 0x7a0, &flags) || flags[0] > 1 || flags[1] > 1)) {
		g_weatherObject = 0; // gone or reused: look again
	}
	if (g_weatherObject == 0) {
		g_weatherObject  = FindWeather();
		g_weatherSetType = -2;
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
	if (!inGame) {
		if (g_weatherOn) {
			uint8_t flag = 1;
			reinterpret_cast<WeatherFn>(g_base + kWeatherStop)(weather, &flag, 1);
			g_weatherOn = false;
		}
		*reinterpret_cast<uint8_t*>(g_weatherObject + 0x7a1) = 0; // hand the weather back to the game
		g_weatherHeld    = false;
		g_weatherSetType = -2;
		return;
	}
	if (flags[1] != 1) {
		*reinterpret_cast<uint8_t*>(g_weatherObject + 0x7a1) = 1;
	}
	g_weatherHeld    = true;
	const int wanted = WeatherFor(level);
	if (wanted >= 0) {
		if (wanted != g_weatherSetType || level != g_weatherSetLevel || g_config.weatherIntensity != g_weatherSetAmount || now >= g_weatherReassert) {
			struct {
				int32_t type;
				float   intensity;
			} params { wanted, static_cast<float>(g_config.weatherIntensity) / 100.0f };
			reinterpret_cast<WeatherFn>(g_base + kWeatherUpdate)(weather, &params, 1);
			if (!g_weatherOn && g_weatherLogs < 6) {
				++g_weatherLogs;
				Log("weather: %s started in level %d at %d%%", wanted == 0 ? "rain" : "snow", level, g_config.weatherIntensity);
			}
			g_weatherOn        = true;
			g_weatherSetType   = wanted;
			g_weatherSetLevel  = level;
			g_weatherSetAmount = g_config.weatherIntensity;
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

// ---- whispers, stingers, heartbeat --------------------------------------------------
uint64_t g_ambientNext   = 0;
int      g_ravenLeft     = 0;
uint64_t g_ravenNext     = 0;
float    g_ravenGain     = 0.0f;
uint64_t g_heartbeatNext = 0;
uint32_t g_hauntSeen     = 0;

const Clip& AmbientClip(int first, int last) {
	return g_ambient[RandRange(first, last)];
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

	if (g_config.ambientEnabled != 0 && inGame && !(level == 1 || level == 40 || level == 75 || level == 103 || level == 109)) {
		if (g_ambientNext == 0) {
			g_ambientNext = now + static_cast<uint64_t>(RandRange(g_config.ambientMin, g_config.ambientMax)) * 1000;
		} else if (now >= g_ambientNext) {
			g_ambientNext = now + static_cast<uint64_t>(RandRange(g_config.ambientMin, g_config.ambientMax)) * 1000;
			const int   roll = static_cast<int>(Rand() % 100);
			const float gain = volume * static_cast<float>(RandRange(35, 100)) / 100.0f;
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
		g_ravenNext = now + static_cast<uint64_t>(RandRange(650, 1150));
		if (inGame) {
			PlayClip(AmbientClip(16, 17), g_ravenGain);
		} else {
			g_ravenLeft = 0;
		}
	}

	if (g_config.heartbeat != 0 && inGame) {
		int percent = g_lifePercent.load();
		if (g_haunting.load(std::memory_order_relaxed) && g_config.falseHealth != 0) {
			percent = 3; // the orb says 1: the heart agrees
		}
		if (percent > 0 && percent < g_config.heartbeatBelow && now >= g_heartbeatNext) {
			const float danger = 1.0f - static_cast<float>(percent) / static_cast<float>(g_config.heartbeatBelow);
			g_heartbeatNext    = now + 1150 - static_cast<uint64_t>(danger * 600.0f);
			PlayClip(g_heartbeatClip, static_cast<float>(g_config.heartbeatVolume) / 100.0f * (0.7f + 0.3f * danger));
			static int logs = 0;
			if (++logs <= 3) {
				Log("sounds: heartbeat at %d%% life", percent);
			}
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
	Log("sounds: whispers %s every %d-%d s, heartbeat %s below %d%% life", g_config.ambientEnabled != 0 ? "on" : "off", g_config.ambientMin, g_config.ambientMax,
		g_config.heartbeat != 0 ? "on" : "off", g_config.heartbeatBelow);
}

void ReleaseClientExtras() {
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

	if (area == Area::None || (g_config.stormEnabled == 0 && !forced)) {
		g_nextStrike = 0;
	} else {
		if (g_nextStrike == 0) {
			g_nextStrike = now + static_cast<uint64_t>(RandRange(g_config.stormMinInterval, g_config.stormMaxInterval)) * 1000;
		}
		if (forced || now >= g_nextStrike) {
			g_nextStrike = now + static_cast<uint64_t>(RandRange(g_config.stormMinInterval, g_config.stormMaxInterval)) * 1000;
			// a close strike: bright flash, thunder right behind it, loud; far: the opposite
			const int delay = RandRange(150, 2600);
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

	UpdateSky(now, g_level.load(std::memory_order_relaxed) > 0);
	UpdateWeather(now, g_level.load(std::memory_order_relaxed));
	UpdateFalseHealth(g_level.load(std::memory_order_relaxed) > 0);
	UpdateSounds(now, g_level.load(std::memory_order_relaxed));

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

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	const char* args = command != nullptr && command->args != nullptr ? command->args : "";
	while (*args == ' ') {
		++args;
	}
	char text[400];
	if (strncmp(args, "spawn", 5) == 0) {
		g_forceSpawn = true;
		g_ctx->WriteConsoleMessage("Horror Night: a phantom appears on the next game frame (not in town).");
	} else if (strncmp(args, "test", 4) == 0) {
		const int row = atoi(args + 4);
		g_testRow     = row;
		snprintf(text, sizeof(text), "Horror Night: firing missiles row %d near you (not in town).", row);
		g_ctx->WriteConsoleMessage(text);
	} else if (strncmp(args, "haunt", 5) == 0) {
		g_forceHaunt = true;
		g_ctx->WriteConsoleMessage("Horror Night: a haunting starts on the next game frame (needs undead in this area, not in town).");
	} else if (strncmp(args, "strike", 6) == 0) {
		g_forceStrike = true;
		g_ctx->WriteConsoleMessage("Horror Night: lightning strike.");
	} else {
		snprintf(text, sizeof(text), "Horror Night: phantoms %s%s (alive %d, spawned %d, hits %d, rows %d/%d/%d, lightning row %d); storm %s, level %d. Commands: horror spawn | strike | status",
			g_serverReady.load() ? "on" : "off", g_diabloDead.load() ? ", stopped: Diablo is dead" : "", g_aliveCount.load(), g_totalSpawned.load(), g_totalDrains.load(), g_classes[1].count, g_classes[2].count,
			g_classes[3].count, g_lightningRow[3], g_overlay != nullptr ? "on" : "off", g_level.load());
		g_ctx->WriteConsoleMessage(text);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "horror-night",
	.name        = "Horror Night",
	.version     = "1.0.0",
	.author      = "Latedozer",
	.description = "Phantom monsters that cannot be killed and vanish on a timer, plus lightning and thunder.",
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

	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	g_rng ^= static_cast<uint64_t>(counter.QuadPart) * 0x2545F4914F6CDD1DULL;
	if (g_rng == 0) {
		g_rng = 1;
	}

	Log("Horror Night %s loading (D2R %s, SDK %s)", kPluginInfo.version, context->buildVersion != nullptr ? context->buildVersion : "?", D2RL_SDK_VERSION);
	LoadConfig();
	SetUpServer();
	SetUpStorm();
	static_cast<void>(context->RegisterConsoleCommand("horror", OnConsole, "Horror Night: status, spawn, strike"));
	JoinSettingsMenu();
	if (!g_menuJoined) {
		Log("settings menu: I Hate TOMLs not available at load (will try again when a game starts)");
	}
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_serverReady = false;
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
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_module = instance;
	}
	return TRUE;
}
