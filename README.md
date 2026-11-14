# Horrors in the Night

A horror overhaul for **Diablo II: Resurrected** (Reign of the Warlock, D2R 3.3/3.4) running through
[D2RLoader](https://github.com/D2RLoader/PluginSDK). It has two parts:

| Part | What it is | Needs |
|---|---|---|
| `d2rmm-mod/HorrorNight` | A [D2RMM](https://github.com/olegbl/d2rmm) mod that changes game data: lighting, fog, phantom monster rows, the lightning bolt, and a table of levels for the plugin | D2RMM 1.8+ |
| `plugin/` | A D2RLoader plugin (`d2rl-horror-night.dll`) that drives everything that happens at run time | D2RLoader with plugin ABI 4 |

The mod works on its own (darker world, fog). Phantoms, hauntings, lightning, real weather,
sandstorms and the whispers need the plugin as well.

## What it does

**The world**
- Always night. Every time-of-day lighting profile is replaced by night.
- Much darker, so your light radius matters. Your own light reach is lowered to 65%.
- Monsters' self-lighting is cut, so they stay hidden until they enter your light.
- Thick dark distance fog and low ground fog everywhere.

**Weather and storms** (plugin)
- The game's real rain (Acts 1 and 3) and snow (Act 5) always falls outdoors.
- Lightning storms: the whole scene flashes, a bolt strikes nearby and flickers several times on the
  same spot, then thunder rolls in. Now and then a strike lands on you for 5–10% of your maximum life
  (it never kills).
- A sandstorm in the Act 2 desert: the air fills with sand-coloured fog that closes in around you,
  sand blows past, your light reaches less far and the wind howls. By default it blows the whole
  time you are out there; it can be set to come and go every few minutes instead. In Lut Gholein and underground you only hear the wind. The blowing sand is up to three of
  the game's own Act 2 sand effects, coming in one by one as the storm builds. Each effect is a
  patch a few tiles across, so the storm lays them out on a grid around you and moves the grid as
  you walk. Which effects, how far out and how dense are settings; choosing the same effect more than
  once stacks it for a thicker storm.

**Phantoms** (plugin)
- False monsters appear near you: a twin of a melee monster that belongs to the area you are in.
- They cannot be killed, their hits drain a share of your current life but never kill, and they
  vanish within seconds. Your mercenary ignores them and they ignore it.
- After Diablo dies, phantoms stop for the rest of that game.

**Hauntings** (plugin)
- Rarely, the phantom that appears is one of the dead wearing a townsperson's name.
- While it is there, corpses around you get back up and your life orb reads 1 (display only).

**Whispers** (plugin)
- Breathing, moans, distant screams, ravens and a rare laugh at random moments outside town.

## Install (players)

1. Install [D2RLoader](https://github.com/D2RLoader/PluginSDK) and D2RMM.
2. Copy `d2rmm-mod/HorrorNight` into D2RMM's `mods` folder, enable it, and click **Install**.
3. Download `d2rl-horror-night.dll` from the
   [latest release](https://github.com/latedozer/Horror-mod/releases/latest) (or build it, below) and put it in
   `<game>/mods/<your D2RMM mod name>/d2rloader/plugins/`.
4. Start the game twice the first time (D2RLoader builds its table cache on the first start).

Settings:
- Data-side (darkness, fog, light radius, flash look, sandstorm thickness): D2RMM's settings for the
  mod, then **Install** again.
- Plugin (timings, chances, damage, weather, storms, sandstorms): in game with
  [I Hate TOMLs](https://d2rloader.net) if you have it, or
  `<game>/mods/<mod>/d2rloader/config/horror-night.toml`.

Console (Ctrl + `): `horror status`, `horror spawn`, `horror strike`, `horror haunt`, `horror sand`
(starts or stops a sandstorm when they are set to come and go), `horror fx` (lists the sand effects by
number; `horror fx 6 5 3` sets the storm's three effects while you watch) and `horror field 2 20 24 50`
(how many patches out from you, the sub-tiles between them, how many are created a second, and how
far the field leans ahead of you when you move, in percent). Both are saved.

The mod tells the plugin which of its options it was installed with, so the plugin only holds the
game on night, flashes the sky or raises a sandstorm when the lighting for it is there. With
**Always night** off the game keeps its day and night cycle, and there are no sky flashes or sandstorms.

If you remove the plugin, turn off the mod options **Lightning lights the whole sky** and
**Sandstorms in Act 2** and re-install; otherwise those areas' unused daylight profile shows at
daytime, as a blinding flash or a standing sandstorm.

## Build the plugin

The plugin plays thunder and ambient sounds from the game. Those files belong to Blizzard, so they are
neither in this repository nor in the DLL: at start-up the plugin reads them straight out of the
installed game's own data (Battle.net and Steam editions) with [CascLib](https://github.com/ladislav-zezula/CascLib),
decodes them with [dr_flac](https://github.com/mackron/dr_libs) and processes them (bass lift, gain,
limiter, mono) in `plugin/game_sounds.cpp`. Both libraries are in `plugin/third_party/`.

Linux (what it was developed on):

```sh
# 1. tools: mingw-w64 (pacman -S mingw-w64-gcc)
# 2. the D2RLoader Plugin SDK headers
git clone https://github.com/D2RLoader/PluginSDK ../PluginSDK
# 3. build
SDK=../PluginSDK/include plugin/build.sh
```

## Tests

```sh
SDK=../PluginSDK/include tests/run.sh [extracted game data folder]
```

`tests/plugin_test.cpp` checks the parts of the plugin that do not need the game (config file, level
table, sound mixer, sandstorm clock, the hold on the time of day); it is built with mingw-w64 and run
under Wine. `tests/mod.test.js` runs the D2RMM mod's script with Node against game data you extracted
yourself (the `data` folder with `global/excel` and `hd/env/vis`) and checks what it would install.

## Compatibility

- The plugin calls and hooks D2R engine code at fixed addresses for the game image with PE timestamp
  `0x6AB3782C` (D2R 3.3 build 93847; 3.4.95041 ships the same image). It checks the timestamp and the
  bytes at every address before use, and switches the affected feature off with a log line on any
  mismatch. A future game patch will most likely switch features off until the addresses are updated.
- Single player is what it was built and tested for. The plugin is marked as shared, so both TCP/IP
  players need the same version.
- Other plugins that hook the same functions (the game frame, monster target search) may conflict.
- Phantom and townsfolk rows are appended to `monstats.txt`; Diablo is recognised by his vanilla row
  number (243).
- Where it rains, snows or storms comes from `levels.txt` and `lvltypes.txt` when the mod is installed,
  so levels added by other mods are covered. An older copy of the mod falls back to the vanilla level
  numbers.
- The sandstorm look sits in the Act 2 lighting profiles `act2_town` and `act2_ruin`; another mod
  that rewrites those after this one will change or remove it.
- The game's weather only knows rain and snow. The blowing sand is its own Act 2 sand effects, placed
  around the player with the game's own "particle effect at a place" call from a job in its entity
  world (the way it places rain splashes). Where the player is comes from the game's weather effect,
  which it keeps on the player: while a storm blows that is the storm's first sand effect, created
  by pointing the weather's snow effect name at it for that moment. Like the rest of the plugin this depends on the exact
  game version, and is switched off with a log line when the code does not look as expected.

Engine addresses and structures were found with help from
[d2rAI-toolkit](https://github.com/Nugwa628/d2rAI-toolkit)'s notes and by reading the game's code.

## Logs

`<game>/mods/<mod>/d2rloader/logs/horror-night.log` says which parts are on, and why any are off.

## License

MIT (see `LICENSE`). This covers the code and data in this repository only.
`plugin/third_party/` holds CascLib (MIT, see its `LICENSE`) and dr_flac (public domain / MIT-0).

## Credits

Made by Latedozer.

Diablo II: Resurrected is a trademark of Blizzard Entertainment. This project is not affiliated with or
endorsed by Blizzard. It contains no game files.
