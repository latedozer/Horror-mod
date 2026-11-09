# Horror Night

A horror overhaul for **Diablo II: Resurrected** (Reign of the Warlock, D2R 3.3/3.4) running through
[D2RLoader](https://github.com/D2RLoader/PluginSDK). It has two parts:

| Part | What it is | Needs |
|---|---|---|
| `d2rmm-mod/HorrorNight` | A [D2RMM](https://github.com/olegbl/d2rmm) mod that changes game data: lighting, fog, sound volumes, phantom monster rows, the lightning bolt | D2RMM 1.8+ |
| `plugin/` | A D2RLoader plugin (`d2rl-horror-night.dll`) that drives everything that happens at run time | D2RLoader with plugin ABI 4 |

The mod works on its own (darker world, fog, sound mix). Phantoms, hauntings, lightning, real weather
and the whispers need the plugin as well.

## What it does

**The world**
- Always night. Every time-of-day lighting profile is replaced by night.
- Much darker, so your light radius matters. Your own light reach is lowered to 65%.
- Monsters' self-lighting is cut, so they stay hidden until they enter your light.
- Thick dark distance fog and low ground fog everywhere.
- Music turned down, the game's own ambience turned up.

**Weather and storms** (plugin)
- The game's real rain (Acts 1 and 3) and snow (Act 5) always falls outdoors.
- Lightning storms: the whole scene flashes, a bolt strikes nearby and flickers several times on the
  same spot, then thunder rolls in. Now and then a strike lands on you for 5–10% of your maximum life
  (it never kills).

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
3. Build the plugin (below) and put `d2rl-horror-night.dll` in
   `<game>/mods/<your D2RMM mod name>/d2rloader/plugins/`.
4. Start the game twice the first time (D2RLoader builds its table cache on the first start).

Settings:
- Data-side (darkness, fog, light radius, volumes, flash look): D2RMM's settings for the mod, then
  **Install** again.
- Plugin (timings, chances, damage, weather, storms): in game with
  [I Hate TOMLs](https://d2rloader.net) if you have it, or
  `<game>/mods/<mod>/d2rloader/config/horror-night.toml`.

Console (Ctrl + `): `horror status`, `horror spawn`, `horror strike`, `horror haunt`.

If you remove the plugin, turn off the mod option **Lightning lights the whole sky** and re-install;
otherwise the stormy areas' unused daylight profile shows as a blinding daytime.

## Build the plugin

The plugin embeds thunder and ambient sounds from the game. Those files belong to Blizzard and are not
in this repository: `tools/get-sounds.sh` pulls them from your own installed copy.

Linux (what it was developed on):

```sh
# 1. tools: mingw-w64 (pacman -S mingw-w64-gcc), wine with winegcc, ffmpeg
# 2. the D2RLoader Plugin SDK headers
git clone https://github.com/D2RLoader/PluginSDK ../PluginSDK
# 3. the sounds, from your game install (CascLib.dll ships with D2RMM in its tools folder)
tools/get-sounds.sh "/path/to/Diablo II Resurrected" "/path/to/D2RMM/tools/CascLib.dll"
# 4. build
SDK=../PluginSDK/include plugin/build.sh
```

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

Engine addresses and structures were found with help from
[d2rAI-toolkit](https://github.com/Nugwa628/d2rAI-toolkit)'s notes and by reading the game's code.

## Logs

`<game>/mods/<mod>/d2rloader/logs/horror-night.log` says which parts are on, and why any are off.

## Credits

Made by Latedozer.

Diablo II: Resurrected is a trademark of Blizzard Entertainment. This project is not affiliated with or
endorsed by Blizzard. It contains no game files.
