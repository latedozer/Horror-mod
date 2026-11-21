// A Darker Atmosphere - always night, darker world, light radius matters.
// The look is all in the HD lighting profiles (data/hd/env/vis). The rest is data for the
// A Darker Atmosphere plugin: phantom monster rows, the lightning bolt, and a table of levels.
const FAMILIES = ["act1_barracks", "act1_campfire", "act1_catacombs", "act1_cathedral", "act1_caves", "act1_court", "act1_crypt", "act1_outdoors", "act1_tristram", "act2_arcane", "act2_bigcliff", "act2_maggot", "act2_outdoors", "act2_palace", "act2_palace_cells", "act2_palace_clean", "act2_ruin", "act2_sewer", "act2_tomb", "act2_town", "act3_docktown", "act3_jungle", "act3_jungle_dungeon", "act3_kurast", "act3_kurast_stone_tile", "act3_sewer", "act3_spider", "act3_temple", "act3_travincal", "act4_diab", "act4_fort", "act4_lava", "act4_mesa", "expansion_baallair", "expansion_baallair_throneroom", "expansion_icecave", "expansion_mountaintop", "expansion_ruins", "expansion_ruins_snow", "expansion_siege", "expansion_siege_town", "expansion_town", "expansion_wildtemple", "expansion_wildtemple_tempenter", "uberarreatsummit"];
const SINGLES = ["act1_barracks_desecrated", "act1_catacombs_desecrated", "act1_caves_desecrated", "act1_court_desecrated", "act1_crypt_desecrated", "act1_outdoors_desecrated", "act1_outdoors_interior01_vis", "act2_arcane_desecrated", "act2_maggot_desecrated", "act2_palace_cells_desecrated", "act2_palace_desecrated", "act2_sewer_desecrated", "act2_tainted_sun", "act2_tomb_desecrated", "act2_town_desecrated", "act2_town_interior_vis", "act3_docktown_desecrated", "act3_jungle_dungeon_desecrated", "act3_sewer_desecrated", "act3_spider_desecrated", "act3_temple_desecrated", "act3_travincal_desecrated", "act4_fort_interior_vis", "act4_lava_desecrated", "act4_mesa_desecrated", "denofevil_vis", "diabloappears_vis", "expansion_baallair_throneroom_desecrated", "expansion_icecave_desecrated", "expansion_mountaintop_desecrated", "expansion_siege_desecrated", "expansion_wildtemple_desecrated", "expansion_wildtemple_tempenter_desecrated", "uberarreatsummit_desecrated", "visbox_act1_cathedral_vis", "visbox_docktown_interior01_vis", "visbox_harrograth_vis", "visbox_kurast_hut_ambient_vis", "visbox_kurast_temple_ambient_vis", "visbox_monastry_vis", "visbox_tempenter_darkness_vis", "visbox_tempenter_roof_vis", "visbox_tower_vis"];
const TOWNS = ["act2_town", "act3_docktown", "act4_fort", "expansion_town", "expansion_siege_town"];
// outdoor lighting families that get permanent weather
const RAIN = ['act1_outdoors', 'act1_court', 'act1_tristram', 'act3_jungle', 'act3_kurast', 'act3_kurast_stone_tile', 'act3_docktown', 'act3_travincal'];
const SNOW = ['expansion_siege', 'expansion_siege_town', 'expansion_town', 'expansion_mountaintop', 'expansion_ruins', 'expansion_ruins_snow', 'expansion_wildtemple_tempenter', 'uberarreatsummit'];
const RAIN_VFX = 'data/hd/vfx/particles/env/common/weather/Rain.particles';
const SNOW_VFX = 'data/hd/vfx/particles/env/common/weather/Snow.particles';
// lighting families of the Act 2 desert: Lut Gholein's (the open desert uses it too) and the Lost City's
const SAND = ['act2_town', 'act2_ruin'];
// lighting families of Act 4's outdoor areas: the Outer Steppes to the City of the Damned, the
// River of Flame, the Chaos Sanctuary (the Pandemonium Fortress, act4_fort, keeps its own)
const ASH = ['act4_mesa', 'act4_lava', 'act4_diab'];
// Act 5's blizzard takes the "day" lighting of the snowy families (the SNOW list above), which the
// lightning flash would otherwise use: in Act 5 the falling ice replaces the lightning.
// Act 3's monsoon: the outdoor families of Kurast (the docks, the jungles, the city, Travincal).
// Here the lightning stays, so these families carry four looks side by side for the plugin to
// blend between: dawn1 the lightning flash, dawn2 the night, day the monsoon, dusk1 the monsoon
// lit by lightning (everywhere else the flash is the "day" look and nothing else is).
const MONSOON = ['act3_jungle', 'act3_kurast', 'act3_kurast_stone_tile', 'act3_docktown', 'act3_travincal'];
const STAGES = ['dawn1', 'dawn2', 'day', 'dusk1', 'dusk2', 'night'];

// Every feature sits in a config section with its own on/off checkbox.
const darknessOn = config.darknessEnabled !== false;
const keep = darknessOn ? 1 - Math.min(Math.max(config.darkness, 0), 95) / 100 : 1; // fraction of ambient light kept
const keyCap = darknessOn ? config.keyLightCap : Infinity;
const monsterKeep = darknessOn ? Math.min(Math.max(config.monsterVisibility, 0), 100) / 100 : 1;
const radiusScale = config.lightRadiusEnabled !== false ? config.lightRadiusScale / 100 : 1;
const alwaysNight = darknessOn && config.alwaysNight;
const weatherOn = config.weatherEnabled !== false;
const lightningOn = weatherOn && config.lightning;
// both of these turn an area's "day" profile into something else, so they need Always night
const flashOn = lightningOn && config.skyFlash && alwaysNight;
const sandOn = config.sandstormEnabled !== false && alwaysNight;
const sandThick = Math.min(Math.max(Number.isFinite(config.sandThickness) ? config.sandThickness : 60, 10), 100) / 100;
const ashOn = config.ashstormEnabled !== false && alwaysNight;
const ashThick = Math.min(Math.max(Number.isFinite(config.ashThickness) ? config.ashThickness : 60, 10), 100) / 100;
const blizzardOn = config.blizzardEnabled !== false && alwaysNight;
const blizzardThick = Math.min(Math.max(Number.isFinite(config.blizzardThickness) ? config.blizzardThickness : 60, 10), 100) / 100;
const iceOn = blizzardOn && config.blizzardIce !== false;
const monsoonOn = config.monsoonEnabled !== false && alwaysNight;
const monsoonThick = Math.min(Math.max(Number.isFinite(config.monsoonThickness) ? config.monsoonThickness : 60, 10), 100) / 100;

function visPath(name) {
  return 'hd\\env\\vis\\' + name + '.json';
}

function entity(def, name) {
  return (def.entities || []).find((e) => e.name === name);
}

function light(def, name) {
  const e = entity(def, name);
  return e && e.components.find((c) => c.type === 'DirectionalLightDefinitionComponent');
}

// Permanent rain or snow: the lighting profile's own screen-space effect slot
// (vanilla uses it for the Arcane Sanctuary stars). Only fills an empty slot.
function addWeather(def, family) {
  if (!weatherOn || !config.alwaysWeather) return;
  const vfx = RAIN.includes(family) ? RAIN_VFX : SNOW.includes(family) ? SNOW_VFX : null;
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (!vfx || !data || data.screenspaceVfx) return;
  data.screenspaceVfx = vfx;
  if (def.dependencies && Array.isArray(def.dependencies.particles) && !def.dependencies.particles.some((p) => p.path === vfx)) {
    def.dependencies.particles.push({ path: vfx });
  }
}

// Thicker fog: distance fog and a low ground fog, in a dark blue-grey so it swallows
// light instead of glowing. Never thins fog an area already has.
function addFog(def) {
  const amount = weatherOn && config.fogEnabled !== false ? Math.min(Math.max(config.fogAmount, 0), 100) / 100 : 0;
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (!data || amount <= 0) return;
  const dark = { x: 0.16, y: 0.19, z: 0.25 };
  if (typeof data.depthFogDensity === 'number') {
    if (data.depthFogDensity <= 0) data.depthFogColor = { ...dark };
    data.depthFogDensity = Math.max(data.depthFogDensity, 0.9 * amount);
  }
  if (typeof data.heightFogDensity === 'number') {
    if (data.heightFogDensity <= 0) {
      data.heightFogColor = { ...dark };
      if (!(data.heightFogPlane > 0)) data.heightFogPlane = 5;
    }
    data.heightFogDensity = Math.max(data.heightFogDensity, 1.0 * amount);
  }
}

function darken(def, amount) {
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (data) {
    if (typeof data.iblIntensity === 'number') data.iblIntensity *= amount;
    // the light every unit carries; its reach follows the unit's light radius
    if (typeof data.playerUnitLightDistanceScaler === 'number') data.playerUnitLightDistanceScaler *= radiusScale;
  }
  const key = light(def, 'global_key_light');
  if (key) key.power = Math.min(key.power * amount, keyCap);
  for (const name of ['global_fill_light', 'global_back_light']) {
    const l = light(def, name);
    if (l) l.power = Math.min(l.power * amount, keyCap);
  }
  // these two light monsters no matter how dark the area is
  for (const name of ['monster_fill_light', 'monster_back_light']) {
    const l = light(def, name);
    if (l) l.power *= monsterKeep;
  }
}

// Sky flash: A Darker Atmosphere's plugin holds the renderer on "night" and, for a lightning
// strike, blends for a split second toward the "day" profile. So in stormy outdoor areas
// the day profile becomes the lit-by-lightning look: cold, bright, and monsters fully lit.
// Without the plugin the game's own clock would show this profile as daytime.
function makeFlash(def, night) {
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (data) data.iblIntensity = config.flashAmbient / 100;
  const key = light(def, 'global_key_light');
  if (key) {
    key.power = config.flashBrightness;
    key.color = { x: 0.78, y: 0.86, z: 1.0 };
  }
  for (const name of ['monster_fill_light', 'monster_back_light']) {
    const l = light(def, name);
    const original = light(night, name);
    if (l && original) l.power = original.power;
  }
}

// Sandstorm: like the sky flash, this takes over the "day" profile, which nobody sees under
// Always night. For the Act 2 desert it becomes the height of a storm: the air turns to
// sand-coloured fog that closes in, the wind picks up and your light reaches less far. The
// plugin fades it in and out. Without the plugin the game's own clock would show it at midday.
function makeSandstorm(def) {
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (!data) return;
  if (typeof data.depthFogDensity === 'number') {
    data.depthFogColor = { x: 0.3, y: 0.22, z: 0.12 }; // the wall of dust further off
    data.depthFogDensity = Math.max(data.depthFogDensity, 1 + 2.5 * sandThick);
  }
  if (typeof data.heightFogDensity === 'number') {
    data.heightFogColor = { x: 0.36, y: 0.27, z: 0.15 }; // dust along the ground, caught by your light
    data.heightFogDensity = Math.max(data.heightFogDensity, 0.5 + 1.5 * sandThick);
    if (!(data.heightFogPlane > 0)) data.heightFogPlane = 5;
  }
  if (typeof data.windScale === 'number') data.windScale = 1.5; // the strongest any vanilla area uses
  if (typeof data.playerUnitLightDistanceScaler === 'number') data.playerUnitLightDistanceScaler *= 1 - 0.3 * sandThick;
  const key = light(def, 'global_key_light');
  if (key) key.color = { x: 0.85, y: 0.62, z: 0.36 }; // moonlight through dust
  // The renderer also has a volumetric fog that no vanilla area switches on. Off unless asked for.
  if (config.sandVolumetric && typeof data.fogDensity === 'number') {
    data.fogDensity = sandThick;
    data.fogDiffuse = { x: 0.6, y: 0.45, z: 0.25 };
    data.fogWind = 0.5;
    data.fogChurnStrength = 0.5;
    data.fogWispiness = 0.4;
    data.fogPlayerRadius = 6;
    data.fogPlayerFade = 0.5;
  }
}

// Ash storm: the same for Act 4. Its "day" profile becomes the height of a storm of ash and
// embers: the distance fills with ash, smoke lies along the ground, the wind picks up and your
// light reaches less far. The plugin fades it in and out, and only in the areas the level table
// below marks for it. Three looks:
//   lit    grey ash closing in, lit red-orange from below as if by the lava
//   ash    grey ash and grey light, like near a volcano
//   ember  red smoke and red light, close to the red the game gives Act 4's terror zones
const ASH_LOOKS = {
  lit: { far: { x: 0.2, y: 0.12, z: 0.09 }, low: { x: 0.42, y: 0.2, z: 0.1 }, key: { x: 1.0, y: 0.5, z: 0.25 }, smoke: { x: 0.6, y: 0.32, z: 0.2 } },
  ash: { far: { x: 0.17, y: 0.16, z: 0.16 }, low: { x: 0.3, y: 0.29, z: 0.28 }, key: { x: 0.75, y: 0.73, z: 0.7 }, smoke: { x: 0.5, y: 0.48, z: 0.46 } },
  ember: { far: { x: 0.28, y: 0.06, z: 0.04 }, low: { x: 0.45, y: 0.1, z: 0.06 }, key: { x: 1.0, y: 0.2, z: 0.15 }, smoke: { x: 0.65, y: 0.18, z: 0.1 } },
};
const ashLook = ASH_LOOKS[config.ashLook] || ASH_LOOKS.lit;
function makeAshstorm(def) {
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (!data) return;
  if (typeof data.depthFogDensity === 'number') {
    data.depthFogColor = { ...ashLook.far }; // the ash further off
    data.depthFogDensity = Math.max(data.depthFogDensity, 1 + 2.5 * ashThick);
  }
  if (typeof data.heightFogDensity === 'number') {
    data.heightFogColor = { ...ashLook.low }; // ash and smoke along the ground, caught by your light
    data.heightFogDensity = Math.max(data.heightFogDensity, 0.5 + 1.5 * ashThick);
    if (!(data.heightFogPlane > 0)) data.heightFogPlane = 5;
  }
  if (typeof data.windScale === 'number') data.windScale = 1.5; // the strongest any vanilla area uses
  if (typeof data.playerUnitLightDistanceScaler === 'number') data.playerUnitLightDistanceScaler *= 1 - 0.3 * ashThick;
  const key = light(def, 'global_key_light');
  if (key) key.color = { ...ashLook.key };
  // The renderer's volumetric fog, which no vanilla area switches on. Off unless asked for.
  if (config.ashVolumetric && typeof data.fogDensity === 'number') {
    data.fogDensity = ashThick;
    data.fogDiffuse = { ...ashLook.smoke };
    data.fogWind = 0.5;
    data.fogChurnStrength = 0.5;
    data.fogWispiness = 0.4;
    data.fogPlayerRadius = 6;
    data.fogPlayerFade = 0.5;
  }
}

// Blizzard: the same for Act 5. Its "day" lighting becomes the height of a blizzard: snow fills
// the air until the distance is a pale grey wall, drifts lie along the ground and catch your light,
// the light turns cold and blue, the wind is at its hardest and your light reaches less far. The
// plugin fades it in and out where the level table below says it blows.
function makeBlizzard(def) {
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (!data) return;
  if (typeof data.depthFogDensity === 'number') {
    data.depthFogColor = { x: 0.3, y: 0.33, z: 0.38 }; // the wall of snow further off
    data.depthFogDensity = Math.max(data.depthFogDensity, 1 + 2.5 * blizzardThick);
  }
  if (typeof data.heightFogDensity === 'number') {
    data.heightFogColor = { x: 0.44, y: 0.48, z: 0.55 }; // drifting snow along the ground, caught by your light
    data.heightFogDensity = Math.max(data.heightFogDensity, 0.5 + 1.5 * blizzardThick);
    if (!(data.heightFogPlane > 0)) data.heightFogPlane = 5;
  }
  if (typeof data.windScale === 'number') data.windScale = 1.5; // the strongest any vanilla area uses
  if (typeof data.playerUnitLightDistanceScaler === 'number') data.playerUnitLightDistanceScaler *= 1 - 0.3 * blizzardThick;
  const key = light(def, 'global_key_light');
  if (key) key.color = { x: 0.62, y: 0.74, z: 1.0 }; // cold moonlight through the snow
  // The renderer's volumetric fog, which no vanilla area switches on. Off unless asked for.
  if (config.blizzardVolumetric && typeof data.fogDensity === 'number') {
    data.fogDensity = blizzardThick;
    data.fogDiffuse = { x: 0.7, y: 0.75, z: 0.85 };
    data.fogWind = 0.8;
    data.fogChurnStrength = 0.6;
    data.fogWispiness = 0.5;
    data.fogPlayerRadius = 6;
    data.fogPlayerFade = 0.5;
  }
}

// Monsoon: the same for Act 3. Its look is the height of a tropical storm: the rain closes the
// jungle in behind a dark green-grey wall, mist lies along the ground and catches your light,
// the wind is at its hardest and your light reaches less far. The plugin fades it in and out
// where the level table below says it blows.
function makeMonsoon(def) {
  const vis = entity(def, 'vis_data');
  const data = vis && vis.components[0];
  if (!data) return;
  if (typeof data.depthFogDensity === 'number') {
    data.depthFogColor = { x: 0.1, y: 0.15, z: 0.13 }; // the wall of rain further off
    data.depthFogDensity = Math.max(data.depthFogDensity, 1 + 2.5 * monsoonThick);
  }
  if (typeof data.heightFogDensity === 'number') {
    data.heightFogColor = { x: 0.2, y: 0.28, z: 0.24 }; // mist and spray along the ground, caught by your light
    data.heightFogDensity = Math.max(data.heightFogDensity, 0.5 + 1.5 * monsoonThick);
    if (!(data.heightFogPlane > 0)) data.heightFogPlane = 5;
  }
  if (typeof data.windScale === 'number') data.windScale = 1.5; // the strongest any vanilla area uses
  if (typeof data.playerUnitLightDistanceScaler === 'number') data.playerUnitLightDistanceScaler *= 1 - 0.3 * monsoonThick;
  const key = light(def, 'global_key_light');
  if (key) key.color = { x: 0.55, y: 0.75, z: 0.68 }; // what light there is comes through wet leaves
  // The renderer's volumetric fog, which no vanilla area switches on. Off unless asked for.
  if (config.monsoonVolumetric && typeof data.fogDensity === 'number') {
    data.fogDensity = monsoonThick;
    data.fogDiffuse = { x: 0.35, y: 0.45, z: 0.4 };
    data.fogWind = 0.8;
    data.fogChurnStrength = 0.6;
    data.fogWispiness = 0.5;
    data.fogPlayerRadius = 6;
    data.fogPlayerFade = 0.5;
  }
}

FAMILIES.forEach((family) => {
  const isTown = TOWNS.includes(family);
  const amount = isTown && darknessOn && config.gentleTowns ? Math.max(keep, 0.8) : keep;
  const night = D2RMM.readJson(visPath(family + '_night'));
  if (!night || !night.entities) return;
  STAGES.forEach((stage) => {
    let out;
    if (alwaysNight || stage === 'night') {
      out = JSON.parse(JSON.stringify(night));
      if (stage !== 'night') {
        // keep the original entity ids of the file being replaced
        const original = D2RMM.readJson(visPath(family + '_' + stage));
        (original.entities || []).forEach((e, i) => {
          if (out.entities[i] && out.entities[i].name === e.name) out.entities[i].id = e.id;
        });
        if (original.name) out.name = original.name;
      }
    } else {
      out = D2RMM.readJson(visPath(family + '_' + stage));
    }
    darken(out, amount);
    addFog(out);
    addWeather(out, family);
    const monsoon = monsoonOn && MONSOON.includes(family);
    if (monsoon) {
      // four looks in a row (see MONSOON): flash, night, monsoon, monsoon lit by lightning
      if (stage === 'day' || stage === 'dusk1') makeMonsoon(out);
      if (flashOn && (stage === 'dawn1' || stage === 'dusk1')) makeFlash(out, night);
    } else if (stage === 'day' && flashOn && (RAIN.includes(family) || (SNOW.includes(family) && !blizzardOn))) {
      makeFlash(out, night);
    }
    if (stage === 'day' && blizzardOn && SNOW.includes(family)) {
      makeBlizzard(out);
    }
    if (stage === 'day' && sandOn && SAND.includes(family)) {
      makeSandstorm(out);
    }
    if (stage === 'day' && ashOn && ASH.includes(family)) {
      makeAshstorm(out);
    }
    D2RMM.writeJson(visPath(family + '_' + stage), out);
  });
});

// terror zones, interiors and other one-off profiles: just darken in place
SINGLES.forEach((name) => {
  const def = D2RMM.readJson(visPath(name));
  if (!def || !def.entities) return;
  darken(def, keep);
  addFog(def);
  addWeather(def, name.replace(/_desecrated$/, ''));
  D2RMM.writeJson(visPath(name), def);
});

// ---------------------------------------------------------------------------
// Phantoms: the monster rows A Darker Atmosphere's plugin spawns. Without the plugin
// (d2rl-horror-night.dll) these rows are never used.
// Every melee monster that can appear in a level gets a phantom twin, so a phantom
// always looks like something that belongs where you are. A twin deals no table
// damage (the plugin drains a share of current life per hit instead), has no skills,
// gives nothing, and cannot be revived.
// How the plugin recognises them: aip8 = 7777, aip8(N) = the row number of the
// monster it imitates, aip8(H) = 7777. Which monsters belong to which level goes
// into the level table at the end of this file.
const levelMonsters = new Map(); // level id -> [monstats row numbers on Normal, on Nightmare/Hell]
if (config.phantoms) {
  const DIFFS = ['', '(N)', '(H)'];
  const MISSILES = ['MissA1', 'MissA2', 'MissS1', 'MissS2', 'MissS3', 'MissS4', 'MissC', 'MissSQ'];
  // AIs that need their skills or missiles to behave, or that use aip8 themselves
  const SKIP_AI = ['SandMaggot', 'VileMother', 'Regurgitator', 'Overseer', 'SuicideMinion', 'FallenShaman', 'FetishShaman', 'FingerMage', 'ReanimatedHorde', 'Swarm', 'DeathMauler'];
  const monstatsFile = 'global\\excel\\monstats.txt';
  const monstats2File = 'global\\excel\\monstats2.txt';
  const modelsFile = 'hd\\character\\monsters.json';
  const monstats = D2RMM.readTsv(monstatsFile);
  const monstats2 = D2RMM.readTsv(monstats2File);
  const levels = D2RMM.readTsv('global\\excel\\levels.txt');
  const models = D2RMM.readJson(modelsFile);
  const has = (table, column) => table.headers.includes(column);
  const set = (table, row, column, value) => {
    if (has(table, column)) row[column] = String(value);
  };

  // the game numbers monstats rows without the "Expansion" divider line
  const rowNumber = new Map();
  let next = 0;
  monstats.rows.forEach((r) => {
    if (r.Id === 'Expansion' || r.Id === '') return;
    rowNumber.set(r.Id, next++);
  });
  const byId = new Map(monstats.rows.map((r) => [r.Id, r]));

  const usable = (r) =>
    r &&
    r.isSpawn === '1' &&
    r.killable === '1' &&
    r.boss !== '1' &&
    r.npc !== '1' &&
    r.placespawn !== '1' &&
    r.deathDmg !== '1' &&
    r.rangedtype !== '1' &&
    Number(r.Velocity) > 0 &&
    !MISSILES.some((m) => r[m]) &&
    !DIFFS.some((d) => r['aip8' + d]) &&
    !SKIP_AI.includes(r.AI);

  // level id -> monsters, for Normal and for Nightmare/Hell
  const perLevel = [];
  const wanted = new Set();
  levels.rows.forEach((level) => {
    if (!/^\d+$/.test(level.Id || '')) return;
    const pick = (prefixes) => {
      const ids = new Set();
      prefixes.forEach((prefix) => {
        for (let i = 1; i <= 25; i++) {
          const id = level[prefix + i];
          if (id && usable(byId.get(id))) ids.add(id);
        }
      });
      return [...ids];
    };
    const normal = pick(['mon', 'umon']);
    const hell = pick(['nmon']);
    normal.concat(hell).forEach((id) => wanted.add(id));
    if (normal.length || hell.length) perLevel.push({ id: level.Id, normal, hell });
  });

  [...wanted].forEach((baseId) => {
    const id = 'hnp' + baseId;
    const base = byId.get(baseId);
    if (monstats.rows.some((r) => r.Id === id)) return;

    // own monstats2 row: same looks, but the corpse cannot be used or revived
    const base2 = monstats2.rows.find((r) => r.Id === base.MonStatsEx);
    if (base2) {
      const row2 = { ...base2, Id: id };
      set(monstats2, row2, '*hcIdx', monstats2.rows.length);
      set(monstats2, row2, 'revive', 0);
      set(monstats2, row2, 'corpseSel', 0);
      monstats2.rows.push(row2);
    }

    const row = { ...base, Id: id, BaseId: id, NextInClass: '' };
    set(monstats, row, '*hcIdx', next++);
    if (base2) row.MonStatsEx = id;
    for (const c of ['isSpawn', 'boss', 'primeevil', 'deathDmg', 'genericSpawn', 'zoo', 'placespawn', 'Drain', 'coldeffect']) set(monstats, row, c, 0);
    for (const c of ['killable', 'petIgnore', 'neverCount', 'noRatio', 'CannotDesecrate', 'CannotHerald', 'MinGrp', 'MaxGrp']) set(monstats, row, c, 1);
    for (const c of ['spawn', 'minion1', 'minion2', 'PartyMin', 'PartyMax', 'MonProp', 'SplEndDeath', 'TCQuestId', 'TCQuestCP']) set(monstats, row, c, '');
    for (let i = 1; i <= 8; i++) {
      set(monstats, row, 'Skill' + i, '');
      set(monstats, row, 'Sk' + i + 'mode', '');
      set(monstats, row, 'Sk' + i + 'lvl', '');
    }
    set(monstats, row, 'DamageRegen', 20);
    set(monstats, row, 'Velocity', Math.max(Number(base.Velocity) || 0, 4));
    set(monstats, row, 'Run', Math.max(Number(base.Run) || 0, 5));
    DIFFS.forEach((d) => {
      set(monstats, row, 'Level' + d, 1);
      set(monstats, row, d === '' ? 'minHP' : 'MinHP' + d, 30000);
      set(monstats, row, d === '' ? 'maxHP' : 'MaxHP' + d, 30000);
      set(monstats, row, 'Exp' + d, 0);
      set(monstats, row, 'Drain' + d, 0);
      set(monstats, row, 'coldeffect' + d, 0);
      for (const c of ['A1MinD', 'A1MaxD', 'A2MinD', 'A2MaxD', 'S1MinD', 'S1MaxD']) set(monstats, row, c + d, 0);
      for (const c of ['ResDm', 'ResMa', 'ResFi', 'ResLi', 'ResCo', 'ResPo']) set(monstats, row, c + d, 100);
      for (const e of ['El1', 'El2', 'El3']) for (const c of ['Pct', 'MinD', 'MaxD', 'Dur']) set(monstats, row, e + c + d, '');
      for (const c of ['TreasureClass', 'TreasureClassChamp', 'TreasureClassUnique', 'TreasureClassQuest', 'TreasureClassDesecrated', 'TreasureClassDesecratedChamp', 'TreasureClassDesecratedUnique', 'TreasureClassHerald'])
        set(monstats, row, c + d, '');
    });
    set(monstats, row, 'aip8', 7777);
    set(monstats, row, 'aip8(N)', rowNumber.get(baseId));
    set(monstats, row, 'aip8(H)', 7777);
    for (const e of ['El1', 'El2', 'El3']) for (const c of ['Mode', 'Type']) set(monstats, row, e + c, '');
    monstats.rows.push(row);

    // HD model: same as the monster it imitates
    if (models && models[baseId] && !models[id]) models[id] = models[baseId];
  });

  // Townsfolk among the dead: for every undead twin, one more copy per act that carries
  // the name of someone from that act's town. Marked with aip8(H) = 7778 + act number.
  // The plugin spawns one now and then and starts its haunting with it.
  const TOWNSFOLK = [
    ['Akara', 'Kashya', 'Charsi', 'Gheed', 'Warriv', 'DeckardCain'],
    ['Atma', 'Drognan', 'Fara', 'Greiz', 'Elzix', 'Geglash', 'Lysander', 'Jerhyn', 'Meshif'],
    ['asheara', 'hratli', 'alkor', 'ormus', 'Natalya', 'meshif'],
    ['Jamella', 'halbu'],
    ['Larzuk', 'Malah', 'Qual-Kehk', 'Drehya'],
  ];
  if (config.townsfolk) {
    const actOfLevel = new Map(levels.rows.map((l) => [l.Id, Number(l.Act)]));
    let nameTurn = 0;
    for (let act = 0; act < TOWNSFOLK.length; act++) {
      const here = new Set();
      perLevel.forEach((l) => {
        if (actOfLevel.get(l.id) === act) l.normal.concat(l.hell).forEach((id) => here.add(id));
      });
      [...here].forEach((baseId) => {
        const base = byId.get(baseId);
        if (base.lUndead !== '1' && base.hUndead !== '1') return;
        const twin = byId.get('hnp' + baseId) || monstats.rows.find((r) => r.Id === 'hnp' + baseId);
        if (!twin) return;
        const id = 'hnt' + act + baseId;
        const row = { ...twin, Id: id, BaseId: id, NameStr: TOWNSFOLK[act][nameTurn++ % TOWNSFOLK[act].length] };
        set(monstats, row, '*hcIdx', next++);
        set(monstats, row, 'aip8(H)', 7778 + act);
        monstats.rows.push(row);
        if (models && models[baseId]) models[id] = models[baseId];
      });
    }
  }

  perLevel.forEach((l) => levelMonsters.set(l.id, [l.normal.map((id) => rowNumber.get(id)), l.hell.map((id) => rowNumber.get(id))]));

  D2RMM.writeTsv(monstatsFile, monstats);
  D2RMM.writeTsv(monstats2File, monstats2);
  if (models) D2RMM.writeJson(modelsFile, models);
}

// ---------------------------------------------------------------------------
// Lightning: a harmless copy of the Thunder Storm bolt that A Darker Atmosphere's
// plugin drops near you during storms. No skill, no damage, hits nothing; it
// only shows the bolt, cracks, and lights the area around it.
// The plugin finds the row by Param4 = 7777 and Param5 = 4242.
if (lightningOn) {
  const missilesFile = 'global\\excel\\missiles.txt';
  const missiles = D2RMM.readTsv(missilesFile);
  const base = missiles.rows.find((r) => r.Missile === 'thunderstorm1');
  if (base && !missiles.rows.some((r) => r.Missile === 'hnlightning')) {
    const row = { ...base, Missile: 'hnlightning' };
    const set = (column, value) => {
      if (missiles.headers.includes(column)) row[column] = String(value);
    };
    set('*ID', missiles.rows.length);
    set('pSrvDoFunc', 1);
    set('pSrvHitFunc', '');
    set('pSrvDmgFunc', '');
    set('Param4', 7777);
    set('Param5', 4242);
    set('Vel', 0);
    set('MaxVel', 0);
    set('Range', 14);
    set('Light', config.lightningLight);
    set('Flicker', 1);
    set('CollideType', 0);
    set('CollideKill', 0);
    set('ClientCol', 0);
    set('ClientSend', 1); // without this the server never tells the client about the bolt
    set('AlwaysExplode', 0);
    set('ReturnFire', 0);
    set('GetHit', 0);
    set('Skill', 'Thunder Storm'); // the HD bolt hides itself when the source skill is plain Attack
    set('MissileSkill', '');
    for (const c of ['MinDamage', 'MaxDamage', 'EMin', 'EMax', 'EType', 'HitSound', 'ProgSound', 'ProgOverlay', 'ExplosionMissile', 'SubMissile1', 'SubMissile2', 'SubMissile3', 'HitSubMissile1', 'HitSubMissile2', 'HitSubMissile3', 'HitSubMissile4', 'CltSubMissile1', 'CltSubMissile2', 'CltSubMissile3', 'CltHitSubMissile1', 'CltHitSubMissile2', 'CltHitSubMissile3', 'CltHitSubMissile4'])
      set(c, '');
    for (let i = 1; i <= 5; i++) {
      for (const c of ['MinLevDam', 'MaxLevDam', 'MinELev', 'MaxELev']) set(c + i, '');
    }
    missiles.rows.push(row);
    // Fallback for the plugin ("bolt_row" in horror-night.toml): let the server send the
    // vanilla Cairn Stones bolt to clients too.
    const cairn = missiles.rows.find((r) => r.Missile === 'cairn stones bolt');
    if (cairn && missiles.headers.includes('ClientSend')) cairn.ClientSend = '1';
    D2RMM.writeTsv(missilesFile, missiles);

    const hdFile = 'hd\\missiles\\missiles.json';
    const hd = D2RMM.readJson(hdFile);
    if (hd) {
      // the Cairn Stones bolt: the game's own stationary bolt from the sky
      hd.hnlightning = 'cairn_stones_bolt';
      D2RMM.writeJson(hdFile, hd);
    }
  }
}

// ---------------------------------------------------------------------------
// Meteors: harmless copies of the Sorceress' Meteor that the plugin drops around you during an
// ash storm. The copy keeps everything the client shows (the rock falling with its tail, the
// explosion and its small fires, the impact sound) and loses what the server does when it
// lands: no hit, no damage, no burning ground.
//   hnmeteor     falls without the Meteor's red marker on the ground (Param5 = 4243)
//   hnmeteorhit  shows the marker: the one aimed at a player, who has until it lands to step
//                out of it (Param5 = 4244); the plugin does the burning, never to death
// The plugin finds the rows by Param4 = 7777 and those Param5 values.
if (ashOn) {
  const missilesFile = 'global\\excel\\missiles.txt';
  const missiles = D2RMM.readTsv(missilesFile);
  const base = missiles.rows.find((r) => r.Missile === 'meteorcenter');
  if (base && !missiles.rows.some((r) => r.Missile === 'hnmeteor')) {
    const add = (name, mark) => {
      const row = { ...base, Missile: name };
      const set = (column, value) => {
        if (missiles.headers.includes(column)) row[column] = String(value);
      };
      set('*ID', missiles.rows.length);
      set('Param4', 7777);
      set('Param5', mark);
      set('ClientSend', 1);
      set('GetHit', 0);
      set('ReturnFire', 0);
      for (const c of ['pSrvHitFunc', 'pSrvDmgFunc', 'ResultFlags', 'HitFlags', 'sHitPar1', 'sHitPar2', 'sHitPar3', 'MinDamage', 'MaxDamage', 'EMin', 'EMax', 'EType', 'ELen', 'SubMissile1', 'SubMissile2', 'SubMissile3', 'HitSubMissile1', 'HitSubMissile2', 'HitSubMissile3', 'HitSubMissile4'])
        set(c, '');
      for (let i = 1; i <= 5; i++) {
        for (const c of ['MinLevDam', 'MaxLevDam', 'MinELev', 'MaxELev']) set(c + i, '');
      }
      missiles.rows.push(row);
    };
    add('hnmeteor', 4243);
    add('hnmeteorhit', 4244);
    D2RMM.writeTsv(missilesFile, missiles);

    const hdFile = 'hd\\missiles\\missiles.json';
    const hd = D2RMM.readJson(hdFile);
    if (hd) {
      hd.hnmeteor = 'null'; // the game's own "nothing to show" (the falling rock is a missile of its own)
      hd.hnmeteorhit = hd.meteorcenter || 'meteor_target_indicator';
      D2RMM.writeJson(hdFile, hd);
    }
  }
}

// ---------------------------------------------------------------------------
// Falling ice: the blizzard's danger, in place of the lightning. Harmless copies of the Meteor
// above, the way it falls, but what falls is a spike of ice (the Sorceress' Glacial Spike) and what it
// does on landing is the Glacial Spike's freezing burst, without the fire.
//   hnicefall   the falling shard (a copy of the Meteor's falling rock, shown as a Glacial Spike:
//               the Blizzard's own shard is a short effect that draws its own drop, and is over
//               long before a fall of the Meteor's length comes into view)
//   hnice       falls without a marker on the ground (Param5 = 4245)
//   hnicehit    shows a marker: the one aimed at a player, who has until it lands to step out of
//               it (Param5 = 4246); the plugin does the damage, never to death
// The plugin finds the rows by Param4 = 7777 and those Param5 values.
// 'ice': no picture of its own - the plugin shows the marker (the Meteor's circle brings the
// Meteor's impact with it when the shard lands)
const ICE_MARKERS = { ice: 'null', meteor: 'meteor_target_indicator', crack: 'icecrack' };
if (iceOn) {
  const missilesFile = 'global\\excel\\missiles.txt';
  const missiles = D2RMM.readTsv(missilesFile);
  const centre = missiles.rows.find((r) => r.Missile === 'meteorcenter');
  const rock = missiles.rows.find((r) => r.Missile === 'meteor');
  if (centre && rock && !missiles.rows.some((r) => r.Missile === 'hnice')) {
    const make = (base, name) => {
      const row = { ...base, Missile: name };
      const set = (column, value) => {
        if (missiles.headers.includes(column)) row[column] = String(value);
      };
      set('*ID', missiles.rows.length);
      missiles.rows.push(row);
      return set;
    };
    make(rock, 'hnicefall');
    const add = (name, mark) => {
      const set = make(centre, name);
      set('Param4', 7777);
      set('Param5', mark);
      set('ClientSend', 1);
      set('GetHit', 0);
      set('ReturnFire', 0);
      for (const c of ['pSrvHitFunc', 'pSrvDmgFunc', 'ResultFlags', 'HitFlags', 'sHitPar1', 'sHitPar2', 'sHitPar3', 'MinDamage', 'MaxDamage', 'EMin', 'EMax', 'EType', 'ELen', 'SubMissile1', 'SubMissile2', 'SubMissile3', 'HitSubMissile1', 'HitSubMissile2', 'HitSubMissile3', 'HitSubMissile4', 'CltSubMissile2', 'CltHitSubMissile3', 'CltHitSubMissile4'])
        set(c, ''); // the last two are the Meteor's fires on the ground (the third is set again below)
      for (let i = 1; i <= 5; i++) {
        for (const c of ['MinLevDam', 'MaxLevDam', 'MinELev', 'MaxELev']) set(c + i, '');
      }
      set('CltSubMissile1', 'hnicefall');
      // on landing: what a Glacial Spike does when it hits - the freezing burst and the ice thrown
      // out of it - with a flash of blue light where the Meteor has a white one
      set('CltHitSubMissile1', 'freezingarrowexp1');
      set('cHitPar1', 3);
      set('CltHitSubMissile2', 'bluelightmissile');
      set('CltHitSubMissile3', 'freezingarrowexp2');
      set('cHitPar2', 3);
      set('ProgSound', 'sorceress_blizzard_impact_1');
      // a cold blue glow while it falls, as the game's ice missiles have
      set('Red', 81);
      set('Green', 81);
      set('Blue', 255);
    };
    add('hnice', 4245);
    add('hnicehit', 4246);
    D2RMM.writeTsv(missilesFile, missiles);

    const hdFile = 'hd\\missiles\\missiles.json';
    const hd = D2RMM.readJson(hdFile);
    if (hd) {
      hd.hnicefall = 'glacial_spike'; // the Sorceress' Glacial Spike, coming down
      hd.hnice = 'null';
      hd.hnicehit = ICE_MARKERS[config.iceMarker] || ICE_MARKERS.ice;
      D2RMM.writeJson(hdFile, hd);
    }
  }
}

// ---------------------------------------------------------------------------
// Snakes in Act 3: the little snakes of the jungle are critters, which levels.txt gives a percent
// chance to appear (cpct) in up to four slots (cmon). Vanilla has the snake once, at 30, beside
// the parrot. More of them the way the game itself has more rats in Kurast's sewers (the rat twice,
// at 100): a higher chance, and the snake again in the slots the level leaves empty.
//   more   the snake's chance 100
//   many   ...and the snake in the two empty slots as well, at 100
const SNAKES = { vanilla: null, more: { chance: 100, fill: false }, many: { chance: 100, fill: true } };
const snakes = Object.prototype.hasOwnProperty.call(SNAKES, config.act3Snakes) ? SNAKES[config.act3Snakes] : SNAKES.many;
if (snakes) {
  const levelsFile = 'global\\excel\\levels.txt';
  const levels = D2RMM.readTsv(levelsFile);
  let changed = false;
  levels.rows.forEach((level) => {
    const slots = [1, 2, 3, 4];
    if (!slots.some((n) => level['cmon' + n] === 'snake')) return; // only where the game has snakes already
    slots.forEach((n) => {
      if (level['cmon' + n] === 'snake') {
        level['cpct' + n] = String(snakes.chance);
        changed = true;
      } else if (snakes.fill && !level['cmon' + n]) {
        level['cmon' + n] = 'snake';
        level['cpct' + n] = String(snakes.chance);
        changed = true;
      }
    });
  });
  if (changed) D2RMM.writeTsv(levelsFile, levels);
}

// ---------------------------------------------------------------------------
// The level table for the plugin: global\excel\d2rloader\horror-night\levels.tsv.
//   #mod line: which of this mod's options the install was built with, so the plugin only
//              holds the game on night, flashes the sky or raises a sandstorm, an ash storm or
//              a blizzard when the lighting for it is really there.
//   per level: its phantom monsters on Normal and on Nightmare/Hell, then
//     weather  what falls there: 0 nothing, 1 rain, 2 snow, 3 sand (a desert, for sandstorms)
//     town     1 for a town
//     under    for a level with no weather of its own, what falls outside it (same numbers):
//              muffled thunder underground, muffled wind in Lut Gholein and the tombs
//     ash      the Act 4 ash storm: 0 none, 1 it blows here, 2 sheltered from it (the Pandemonium
//              Fortress: muffled wind). Its own column, after the others, so that a plugin from
//              before the ash storm reads the table as it always did.
//     blizzard the Act 5 blizzard, the same way: 1 it blows here (Act 5's open snowfields), 2
//              sheltered from it (Harrogath, and everything underground in Act 5: muffled wind).
//              Wherever it is not 0, the falling ice replaces the lightning and thunder.
//     monsoon  the Act 3 monsoon, the same way: 1 it blows here (Act 3's open country), 2
//              sheltered from it (the Kurast Docks, and everything underground in Act 3). Rain,
//              lightning and thunder carry on in a monsoon.
// Rain or snow is the game's own Rain column; which of the two, and what is "outside", follows
// the act the level's map type belongs to (Uber Tristram is an Act 5 level built from Act 1's
// Tristram: it rains there).
{
  const levels = D2RMM.readTsv('global\\excel\\levels.txt');
  const optional = (file) => {
    try {
      return D2RMM.readTsv(file);
    } catch (e) {
      return null;
    }
  };
  const types = optional('global\\excel\\lvltypes.txt');
  const actinfo = optional('global\\excel\\actinfo.txt');

  // D2RMM keeps a table's line ends as they are: in a file with Windows line ends the last
  // column's name and values end in a carriage return. "Act" is the last column of lvltypes.txt,
  // so it is read under either name.
  const cell = (row, column) => String(row[column] ?? row[column + '\r'] ?? '').trim();
  const typeAct = new Map();
  const typeName = new Map();
  ((types && types.rows) || []).forEach((t) => {
    const id = cell(t, 'Id');
    const act = Number(cell(t, 'Act'));
    if (!/^\d+$/.test(id)) return;
    if (act > 0) typeAct.set(id, act); // unreadable: the vanilla numbering below
    typeName.set(id, cell(t, 'Name'));
  });
  const actOfType = (type) => {
    if (typeAct.has(type)) return typeAct.get(type);
    const n = Number(type); // vanilla numbering, should lvltypes.txt be unreadable
    return !(n > 0) ? 0 : n <= 11 ? 1 : n <= 19 ? 2 : n <= 25 ? 3 : n <= 28 ? 4 : 5;
  };
  const nameOfType = (type) =>
    typeName.get(type) || (type === '16' ? 'Act 2 - Desert' : type === '19' ? 'Act 2 - Arcane' : type === '27' ? 'Act 4 - Mesa' : type === '28' ? 'Act 4 - Lava' : '');
  const towns = new Set(((actinfo && actinfo.rows) || []).map((a) => a.town).filter(Boolean));
  if (!towns.size) ['Act 1 - Town', 'Act 2 - Town', 'Act 3 - Town', 'Act 4 - Town', 'Act 5 - Town'].forEach((t) => towns.add(t));

  const lines = [];
  levels.rows.forEach((level) => {
    if (!/^\d+$/.test(level.Id || '') || level.Id === '0') return;
    const act = actOfType(level.LevelType);
    const type = nameOfType(level.LevelType);
    const town = towns.has(level.Name);
    let weather = 0;
    let under = 0;
    if (level.Rain === '1') weather = act === 5 ? 2 : 1;
    else if (type === 'Act 2 - Desert' && !town) weather = 3;
    else if (act === 1 || act === 3) under = 1;
    else if (act === 5) under = 2;
    else if (act === 2 && type !== 'Act 2 - Arcane') under = 3;
    // The ash storm: Act 4's open plains always; the River of Flame and the Chaos Sanctuary
    // (both of the lava type, told apart by the level's name) when their options are on.
    let ash = 0;
    if (ashOn) {
      if (type === 'Act 4 - Mesa') ash = 1;
      else if (type === 'Act 4 - Lava') ash = (/diablo/i.test(level.Name || '') ? config.ashChaos !== false : config.ashRiver !== false) ? 1 : 0;
      else if (town && act === 4) ash = 2;
    }
    // The blizzard: every open snowfield of Act 5; Harrogath and Act 5's caves and temples shelter.
    let blizzard = 0;
    if (blizzardOn && act === 5) {
      if (weather === 2 && !town) blizzard = 1;
      else if (town || under === 2) blizzard = 2;
    }
    // The monsoon: Act 3's open country (wherever it rains outside the docks); the Kurast Docks and
    // everything underground in Act 3 shelter from it.
    let monsoon = 0;
    if (monsoonOn && act === 3) {
      if (weather === 1 && !town) monsoon = 1;
      else if (town || under === 1) monsoon = 2;
    }
    const monsters = levelMonsters.get(level.Id) || [[], []];
    lines.push([level.Id, monsters[0].join(','), monsters[1].join(','), weather, town ? 1 : 0, under, ash, blizzard, monsoon].join('\t'));
  });
  const options = ['#mod', 'alwaysNight=' + (alwaysNight ? 1 : 0), 'skyFlash=' + (flashOn ? 1 : 0), 'sandstorm=' + (sandOn ? 1 : 0), 'ashstorm=' + (ashOn ? 1 : 0), 'blizzard=' + (blizzardOn ? 1 : 0), 'monsoon=' + (monsoonOn ? 1 : 0)].join('\t');
  D2RMM.writeTxt(
    'global\\excel\\d2rloader\\horror-night\\levels.tsv',
    options + '\r\nlevel\tnormal\tnightmare_hell\tweather\ttown\tunder\tash\tblizzard\tmonsoon\r\n' + lines.join('\r\n') + '\r\n'
  );
}
