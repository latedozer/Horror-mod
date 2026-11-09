// Horror Night - always night, darker world, light radius matters.
// Only touches HD lighting profiles (data/hd/env/vis). Weather (rain/snow) is untouched.
const FAMILIES = ["act1_barracks", "act1_campfire", "act1_catacombs", "act1_cathedral", "act1_caves", "act1_court", "act1_crypt", "act1_outdoors", "act1_tristram", "act2_arcane", "act2_bigcliff", "act2_maggot", "act2_outdoors", "act2_palace", "act2_palace_cells", "act2_palace_clean", "act2_ruin", "act2_sewer", "act2_tomb", "act2_town", "act3_docktown", "act3_jungle", "act3_jungle_dungeon", "act3_kurast", "act3_kurast_stone_tile", "act3_sewer", "act3_spider", "act3_temple", "act3_travincal", "act4_diab", "act4_fort", "act4_lava", "act4_mesa", "expansion_baallair", "expansion_baallair_throneroom", "expansion_icecave", "expansion_mountaintop", "expansion_ruins", "expansion_ruins_snow", "expansion_siege", "expansion_siege_town", "expansion_town", "expansion_wildtemple", "expansion_wildtemple_tempenter", "uberarreatsummit"];
const SINGLES = ["act1_barracks_desecrated", "act1_catacombs_desecrated", "act1_caves_desecrated", "act1_court_desecrated", "act1_crypt_desecrated", "act1_outdoors_desecrated", "act1_outdoors_interior01_vis", "act2_arcane_desecrated", "act2_maggot_desecrated", "act2_palace_cells_desecrated", "act2_palace_desecrated", "act2_sewer_desecrated", "act2_tainted_sun", "act2_tomb_desecrated", "act2_town_desecrated", "act2_town_interior_vis", "act3_docktown_desecrated", "act3_jungle_dungeon_desecrated", "act3_sewer_desecrated", "act3_spider_desecrated", "act3_temple_desecrated", "act3_travincal_desecrated", "act4_fort_interior_vis", "act4_lava_desecrated", "act4_mesa_desecrated", "denofevil_vis", "diabloappears_vis", "expansion_baallair_throneroom_desecrated", "expansion_icecave_desecrated", "expansion_mountaintop_desecrated", "expansion_siege_desecrated", "expansion_wildtemple_desecrated", "expansion_wildtemple_tempenter_desecrated", "uberarreatsummit_desecrated", "visbox_act1_cathedral_vis", "visbox_docktown_interior01_vis", "visbox_harrograth_vis", "visbox_kurast_hut_ambient_vis", "visbox_kurast_temple_ambient_vis", "visbox_monastry_vis", "visbox_tempenter_darkness_vis", "visbox_tempenter_roof_vis", "visbox_tower_vis"];
const TOWNS = ["act2_town", "act3_docktown", "act4_fort", "expansion_town", "expansion_siege_town"];
// outdoor lighting families that get permanent weather
const RAIN = ['act1_outdoors', 'act1_court', 'act1_tristram', 'act3_jungle', 'act3_kurast', 'act3_kurast_stone_tile', 'act3_docktown', 'act3_travincal'];
const SNOW = ['expansion_siege', 'expansion_siege_town', 'expansion_town', 'expansion_mountaintop', 'expansion_ruins', 'expansion_ruins_snow', 'expansion_wildtemple_tempenter', 'uberarreatsummit'];
const RAIN_VFX = 'data/hd/vfx/particles/env/common/weather/Rain.particles';
const SNOW_VFX = 'data/hd/vfx/particles/env/common/weather/Snow.particles';
const STAGES = ['dawn1', 'dawn2', 'day', 'dusk1', 'dusk2', 'night'];

const keep = 1 - Math.min(Math.max(config.darkness, 0), 95) / 100; // fraction of ambient light kept
const keyCap = config.keyLightCap;
const monsterKeep = Math.min(Math.max(config.monsterVisibility, 0), 100) / 100;
const radiusScale = config.lightRadiusScale / 100;

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
  if (!config.alwaysWeather) return;
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
  const amount = Math.min(Math.max(config.fogAmount, 0), 100) / 100;
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

// Sky flash: the Horror Night plugin holds the renderer on "night" and, for a lightning
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

FAMILIES.forEach((family) => {
  const isTown = TOWNS.includes(family);
  const amount = isTown && config.gentleTowns ? Math.max(keep, 0.8) : keep;
  const night = D2RMM.readJson(visPath(family + '_night'));
  if (!night || !night.entities) return;
  STAGES.forEach((stage) => {
    let out;
    if (config.alwaysNight || stage === 'night') {
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
    if (stage === 'day' && config.skyFlash && config.alwaysNight && (RAIN.includes(family) || SNOW.includes(family))) {
      makeFlash(out, night);
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
// Phantoms: the monster rows the Horror Night plugin spawns. Without the plugin
// (d2rl-horror-night.dll) these rows are never used.
// Every melee monster that can appear in a level gets a phantom twin, so a phantom
// always looks like something that belongs where you are. A twin deals no table
// damage (the plugin drains a share of current life per hit instead), has no skills,
// gives nothing, and cannot be revived.
// How the plugin recognises them: aip8 = 7777, aip8(N) = the row number of the
// monster it imitates, aip8(H) = 7777. Which monsters belong to which level is
// written to global\excel\d2rloader\horror-night\levels.tsv.
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

  const lines = perLevel.map((l) => [l.id, l.normal.map((id) => rowNumber.get(id)).join(','), l.hell.map((id) => rowNumber.get(id)).join(',')].join('\t'));
  D2RMM.writeTxt('global\\excel\\d2rloader\\horror-night\\levels.tsv', 'level\tnormal\tnightmare_hell\r\n' + lines.join('\r\n') + '\r\n');

  D2RMM.writeTsv(monstatsFile, monstats);
  D2RMM.writeTsv(monstats2File, monstats2);
  if (models) D2RMM.writeJson(modelsFile, models);
}

// ---------------------------------------------------------------------------
// Lightning: a harmless copy of the Thunder Storm bolt that the Horror Night
// plugin drops near you during storms. No skill, no damage, hits nothing; it
// only shows the bolt, cracks, and lights the area around it.
// The plugin finds the row by Param4 = 7777 and Param5 = 4242.
if (config.lightning) {
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
// Sound: music steps back, the world's own ambience (wind, drips, distant noises)
// steps forward. Only the volume columns change.
if (config.musicVolume !== 100 || config.ambientVolume !== 100) {
  const soundsFile = 'global\\excel\\sounds.txt';
  const sounds = D2RMM.readTsv(soundsFile);
  const scale = (row, percent) => {
    for (const column of ['Volume Min', 'Volume Max']) {
      const value = Number(row[column]);
      if (row[column] !== '' && !Number.isNaN(value)) row[column] = String(Math.max(0, Math.min(255, Math.round((value * percent) / 100))));
    }
  };
  sounds.rows.forEach((row) => {
    if (row.IsMusic === '1') scale(row, config.musicVolume);
    else if (row.IsAmbientScene === '1' || row.IsAmbientEvent === '1') scale(row, config.ambientVolume);
  });
  D2RMM.writeTsv(soundsFile, sounds);
}
