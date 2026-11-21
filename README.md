# A Darker Atmosphere

*Formerly "Horrors in the Night". The file names, the settings file `horror-night.toml`, the D2RMM folder `HorrorNight` and the `horror` console command keep their names, so an existing install updates in place.*

A horror overhaul for **Diablo II: Resurrected** (Reign of the Warlock, D2R 3.3/3.4) running through
[D2RLoader](https://github.com/D2RLoader/PluginSDK). It has two parts:

| Part | What it is | Needs |
|---|---|---|
| `d2rmm-mod/HorrorNight` | A [D2RMM](https://github.com/olegbl/d2rmm) mod that changes game data: lighting, fog, phantom monster rows, the lightning bolt, the ash storm's meteors, the blizzard's ice, and a table of levels for the plugin | D2RMM 1.8+ |
| `plugin/` | A D2RLoader plugin (`d2rl-horror-night.dll`) that drives everything that happens at run time | D2RLoader with plugin ABI 4 |

The mod works on its own (darker world, fog). Phantoms, hauntings, lightning, real weather,
sandstorms, ash storms, blizzards and the whispers need the plugin as well.

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
- Dust devils in the sandstorm: every so often a hard gust brings four to eight whirlwinds up
  around you, and each wanders off slowly on a path of its own. Keep out of them: being inside one
  drains 5% of your maximum life a second (it never kills). They stand for six seconds. How often,
  how many, how hard, how fast and which of the game's whirlwinds are settings.
- An ash storm in Act 4: grey smoke lit red-orange from below closes in around you, ash, embers,
  fog, haze, lava glow and columns of smoke blow past, and under a deep roar of wind you hear fire
  burning, with a vent of steam or a rockfall now and then. It blows in the Outer Steppes, the
  Plains of Despair, the City of the Damned, the River of Flame and the Chaos Sanctuary (the last
  two can each be left calm), the whole time by default, or coming and going like the sandstorm.
  In the Pandemonium Fortress you only hear it, muffled. The blowing field works like the
  sandstorm's, with up to six of the game's own effects at once out of eighteen to choose from,
  and the storm's look is one of three (grey lit from below, plain grey ash, ember red).
- Meteors fall in the ash storm: every 40 to 80 seconds a volley of them comes down around you.
  They are harmless copies of the Sorceress's Meteor. Now and then the first of a volley is aimed
  at the spot you stand on, with the Meteor's red marker on the ground: step out of it in the two
  and a half seconds before it lands, or lose 5–10% of your maximum life (it never kills).
- A blizzard in Act 5: snow closes in until the distance is a pale grey wall, drifting snow, fog
  and wind blow past, the light turns cold and blue, and the wind howls off the mountain with a
  hard gust now and then. The game's own snow falls on you at full strength. It blows on the Bloody
  Foothills, the Frigid Highlands, the Arreat Plateau, the Frozen Tundra, the Arreat Summit and
  outside Nihlathak's Temple, the whole time by default, or coming and going like the other storms.
  In Harrogath and underground you only hear it, muffled. The blowing field works like the ash
  storm's: up to six of the game's own Act 5 effects at once, out of eighteen to choose from.
- Ice falls in the blizzard, in place of the lightning (Act 5 has no lightning or thunder while the
  mod's blizzards are on): every 20 to 60 seconds a volley of sixty ice shards comes down around you. They
  fall the way the Sorceress's Meteor does, but what falls is a spike of ice like her Glacial Spike, and it
  bursts into ice where it lands. Now and then the first of a volley is aimed at you, with a marker on the
  ground: step out of it before it lands, or lose 5–10% of your maximum life (it never kills).
- Wind-blown rain in Act 1: out in the rain, fog hangs in the tree tops and the wind drives past,
  with a quiet wind to hear. It is a blowing field of up to six effects out of twenty-nine (Act 1's own fog, mist,
  ground fog, leaves, the Monastery's and the camp's, and some wind from other acts), and nothing
  else: Act 1's lightning and thunder are untouched, it needs nothing from the mod, and it is never
  in a town. It also blows wherever else it rains and there is no storm of its own.
- A monsoon in Act 3: the rain closes the jungle in behind a dark green-grey wall, mist lies along
  the ground, fog, leaves and debris blow past, and the downpour and the jungle at night are heard
  under the wind, with something calling out now and then. It blows in the three jungles, the four
  parts of Kurast and Travincal, the whole time by default, or coming and going like the other
  storms; in the Kurast Docks and underground you only hear it, muffled. Rain, lightning and
  thunder carry on in it. Its blowing field is up to six effects at once out of thirty-five: Act 3's
  own, and wind, leaves and splashes from other acts' maps.
- More snakes in Act 3 (mod only, no plugin needed): the little snakes of the jungle appear far more
  often in Act 3's outdoor areas. They are the game's own harmless critters; the mod raises their
  chance and lists them again in the level's free critter slots, the way the game itself fills
  Kurast's sewers with rats. "Snakes in Act 3" in D2RMM's settings: as in the game, more, or many.
- Storms show in terror zones too: while lightning flashes or a sandstorm, ash storm or blizzard blows, the
  zone's own lighting gives way to it as much as the storm shows, and comes back as it passes.

**Phantoms** (plugin; off as shipped, switch them on with `enabled` under `[phantoms]`)
- False monsters appear near you: a twin of a melee monster that belongs to the area you are in.
- They cannot be killed, their hits drain a share of your current life but never kill, and they
  vanish within seconds. Your mercenary ignores them and they ignore it.
- After Diablo dies, phantoms stop for the rest of that game.

**Hauntings** (plugin; they come with the phantoms)
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
- Data-side (darkness, fog, light radius, flash look, sandstorm thickness, the ash storm's look,
  thickness and areas, the blizzard's thickness, its ice and the aimed shard's marker): D2RMM's
  settings for the mod, then **Install** again.
- Plugin: in game with [I Hate TOMLs](https://d2rloader.net) if you have it, or
  `<game>/mods/<mod>/d2rloader/config/horror-night.toml`. That file holds the everyday settings:
  what is on, how often, how loud, how much a hit takes, and a switch for each part of a storm's
  blowing field. The fine-tuning settings (timings within a volley, distances, which effect each
  part of a field is, the ice's impact and marker) are listed with their explanations in
  `horror-night-advanced.toml`, which comes in the download as a reference (keep it out of the
  game's config folder): copy a line from it
  into `horror-night.toml` under the same `[section]` to use it. "List the fine-tuning settings
  here too" puts them in the in-game menu as well.
- The whole plugin has one switch: `enabled` under `[plugin]`, `horror toggle` in the console, and
  a key of the game's own - **Options, Controls, Extensions: "A Darker Atmosphere: on / off"**
  (Ctrl + H as shipped; bind it like any other key). Off, storms clear and lightning, phantoms and
  whispers stop; the mod's darkness and fog are game data and stay.

Console (Ctrl + `): `horror status`, `horror spawn`, `horror strike`, `horror haunt`, `horror sand`
(starts or stops a sandstorm when they are set to come and go), `horror fx` (lists the sand effects by
number; `horror fx 6 5 3` sets the storm's three effects while you watch) and `horror field 2 20 24 50`
(how many patches out from you, the sub-tiles between them, how many are created a second, and how
far the field leans ahead of you when you move, in percent). Both are saved. The ash storm has the
same three as `horror ash`, `horror ashfx` (up to six numbers, 0 for none: `horror ashfx 6 9 11 17 14`)
and `horror ashfield`; `horror meteor` drops a volley where you stand. Act 1's wind-blown rain has `horror rainstorm`, `horror stormfx` (`horror stormfx 1 29 24`) and
`horror stormfield`. The monsoon's are `horror monsoon`, `horror rainfx` (`horror rainfx 1 12 7 27 12 28`) and
`horror rainfield`. The blizzard's are
`horror blizzard`, `horror snowfx` (`horror snowfx 1 10 5 9 17 11`) and `horror snowfield`, and
`horror ice` drops a volley of ice. What shows where the ice lands is the plugin's to play, from the game's own ice effects: `horror impact` lists them, `horror impact 1 7` sets one or two, `horror impact show 3` plays one in front of you, and `horror impact marker 11 200 800` sets the marker of an aimed shard, its size in percent and its head start in milliseconds. The sandstorm's dust devils have `horror devil` (lists the effects), `horror devil now` (a group stands up around you), `horror devil wind 1 15` (which whirlwind, and how fast it drifts in tenths of a tile a second) and `horror devil show 12` (any of the listed effects in front of you for five seconds). `horror probe all` goes
through some fifty of the game's effects, puts each on you for a few seconds and says in the log
whether the game could create it there, for anyone picking effects for a storm.

The mod tells the plugin which of its options it was installed with, so the plugin only holds the
game on night, flashes the sky or raises a sandstorm, ash storm, blizzard or monsoon when the
lighting for it is there. With **Always night** off the game keeps its day and night cycle, and
there are no sky flashes, sandstorms, ash storms, blizzards or monsoons (and Act 5 has its
lightning again). Act 1's wind-blown rain has no lighting of its own and blows either way.

If you remove the plugin, turn off the mod options **Lightning lights the whole sky**,
**Sandstorms in Act 2**, **Ash storms in Act 4**, **Blizzards in Act 5** and **Monsoons in Act 3**
and re-install;
otherwise those areas' unused daylight profile shows at daytime, as a blinding flash or a standing
storm.

### If the ash storm is heavy on your machine, or hard to hear

- As shipped the ash storm's field is five effects on an 11 by 11 grid each, 605 patches of
  particles around you, and a volley is 50 meteors. If Act 4 runs slowly, lower **Blowing ash: how
  far out** first (each step down removes a ring of patches from every effect), then switch some
  of its effects off, then lower the meteors in a volley.
- The ash storm's wind is all bass: only what the wind has below 100 Hz is played, at 190% to make
  up for how quiet that sounds. On small speakers it may hardly be heard: raise **Wind: only tones
  below (Hz)** (0 is the whole wind, as in the desert). With a subwoofer it may be too much: lower
  **Wind volume**.

### If the blizzard is heavy on your machine

- As shipped the blizzard's field is five blowing effects on an 11 by 11 grid each (the sixth, the
  snow, only falls on you), 605 patches of particles around you. If Act 5 runs slowly, lower
  **Blowing snow: how far out** (`field_radius`) first, then switch some of its effects off. The
  same goes for the monsoon in Act 3 and the wind-blown rain in Act 1.
- Leaving out effect 1 (the snow) means no snow falls on you during a blizzard: the game's weather
  carries one effect at a time, and in a blizzard that is the blizzard's first layer.

## Build the plugin

The plugin plays thunder, wind, fire and ambient sounds from the game. Those files belong to Blizzard, so they are
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

`tests/plugin_test.cpp` checks the parts of the plugin that do not need the game, against stand-ins
for the game where it calls into it (config file, level table, sound mixer, the storms' clocks,
fields and sounds, bolts and meteors on the server side, the hold on the time of day); it is built
with mingw-w64 and run under Wine. `tests/mod.test.js` runs the D2RMM mod's script with Node against game data you extracted
yourself (the `data` folder with `global/excel` and `hd/env/vis`) and checks what it would install.

## Compatibility

- The plugin calls and hooks D2R engine code at fixed addresses for the game image with PE timestamp
  `0x6AB3782C` (D2R 3.3 build 93847; 3.4.95041 ships the same image). It checks the timestamp and the
  bytes at every address before use, and switches the affected feature off with a log line on any
  mismatch. A future game patch will most likely switch features off until the addresses are updated.
- Single player is what it was built and tested for. The plugin is marked as shared, so both TCP/IP
  players need the same version.
- Other plugins that hook the same functions (the game frame, monster target search, the lighting
  blend) may conflict.
- Phantom and townsfolk rows are appended to `monstats.txt`; Diablo is recognised by his vanilla row
  number (243).
- Where it rains, snows or storms comes from `levels.txt` and `lvltypes.txt` when the mod is installed,
  so levels added by other mods are covered. An older copy of the mod falls back to the vanilla level
  numbers.
- The sandstorm look sits in the Act 2 lighting profiles `act2_town` and `act2_ruin`, the ash storm
  look in the Act 4 profiles `act4_mesa`, `act4_lava` and `act4_diab`, the blizzard look in the
  "day" profiles of Act 5's snowy `expansion_*` families; another mod that rewrites those after this
  one will change or remove it.
- The lightning bolt, the two meteors and the three ice rows are rows appended to `missiles.txt`
  (copies of the game's own, with their damage taken out), with three names added to the HD
  `missiles.json`. The plugin finds them by a mark in their parameters, not by
  row number, so other mods' missiles do not get in the way.
- The game's weather only knows rain and snow. The blowing sand, ash and snow are its own particle effects, placed
  around the player with the game's own "particle effect at a place" call from a job in its entity
  world (the way it places rain splashes). Where the player is comes from the game's weather effect,
  which it keeps on the player: while a storm blows that is the storm's first effect, created
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
