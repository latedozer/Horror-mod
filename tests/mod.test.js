// Runs d2rmm-mod/HorrorNight/mod.js outside D2RMM, against game data you extracted yourself,
// and checks what it would install. The game data is Blizzard's and is not in this repository.
//
//   node tests/mod.test.js <folder holding global/excel/*.txt and hd/env/vis/*.json>
//
// The folder is the "data" folder of an extraction (CascView, or D2RMM's own CascLib).
const fs = require('fs');
const path = require('path');

const dataRoot = process.argv[2];
if (!dataRoot || !fs.existsSync(dataRoot)) {
  console.error('usage: node tests/mod.test.js <extracted game data folder>');
  process.exit(2);
}
const modDir = path.join(__dirname, '..', 'd2rmm-mod', 'HorrorNight');
const source = fs.readFileSync(path.join(modDir, 'mod.js'), 'utf8');
const modInfo = JSON.parse(fs.readFileSync(path.join(modDir, 'mod.json'), 'utf8'));

// the settings D2RMM would pass in: every default from mod.json
function defaults(items, out = {}) {
  for (const item of items) {
    if (item.id && 'defaultValue' in item) out[item.id] = item.defaultValue;
    if (item.children) defaults(item.children, out);
  }
  return out;
}

// game paths are not case sensitive; the extracted files are
function resolve(file) {
  let at = dataRoot;
  for (const part of file.split(/[\\/]/)) {
    if (!fs.existsSync(at)) return null;
    const hit = fs.readdirSync(at).find((name) => name.toLowerCase() === part.toLowerCase());
    if (!hit) return null;
    at = path.join(at, hit);
  }
  return at;
}

function run(config) {
  const written = new Map(); // lower-case path -> what the mod wrote
  const key = (file) => file.replace(/\//g, '\\').toLowerCase();
  const D2RMM = {
    readTsv(file) {
      if (written.has(key(file))) return JSON.parse(JSON.stringify(written.get(key(file))));
      const real = resolve(file);
      if (!real) throw new Error('no such file: ' + file);
      // as D2RMM itself reads a table: split on \n only, so in a file with Windows line ends the
      // last column's name and values keep their carriage return ("Act\r")
      const lines = fs.readFileSync(real, 'latin1').split('\n').filter((line) => line !== '');
      const headers = lines[0].split('\t');
      const rows = lines.slice(1).map((line) => {
        const cells = line.split('\t');
        const row = {};
        headers.forEach((header, i) => (row[header] = cells[i] === undefined ? '' : cells[i]));
        return row;
      });
      return { headers, rows };
    },
    readJson(file) {
      if (written.has(key(file))) return JSON.parse(JSON.stringify(written.get(key(file))));
      const real = resolve(file);
      return real ? JSON.parse(fs.readFileSync(real, 'utf8').replace(/^﻿/, '')) : null;
    },
    writeTsv: (file, value) => written.set(key(file), value),
    writeJson: (file, value) => written.set(key(file), value),
    writeTxt: (file, value) => written.set(key(file), value),
  };
  new Function('D2RMM', 'config', source)(D2RMM, config);
  return (file) => written.get(key(file));
}

let failures = 0;
function check(what, ok, detail) {
  if (!ok) {
    failures++;
    console.log('FAIL  ' + what + (detail !== undefined ? '  (' + detail + ')' : ''));
  } else {
    console.log('ok    ' + what);
  }
}

function levelTable(text) {
  const table = new Map();
  let options = null;
  for (const line of text.split('\r\n')) {
    const cells = line.split('\t');
    if (cells[0] === '#mod') options = line;
    else if (/^\d+$/.test(cells[0])) table.set(Number(cells[0]), { normal: cells[1], hell: cells[2], weather: Number(cells[3]), town: Number(cells[4]), under: Number(cells[5]), ash: Number(cells[6]), cells: cells.length });
  }
  return { table, options };
}

const visData = (def) => def.entities.find((e) => e.name === 'vis_data').components[0];
const keyLight = (def) => def.entities.find((e) => e.name === 'global_key_light').components.find((c) => c.type === 'DirectionalLightDefinitionComponent');

// ---- everything on (the defaults) -------------------------------------------------------
{
  const out = run(defaults(modInfo.config));
  const { table, options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('options line says what was installed', options === '#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1', JSON.stringify(options));
  check('every level line has seven columns', [...table.values()].every((l) => l.cells === 7));

  // level -> [weather, town, under]   (0 nothing, 1 rain, 2 snow, 3 sand)
  const expected = {
    1: [1, 1, 0], // Rogue Encampment: rain, town
    2: [1, 0, 0], // Blood Moor
    8: [0, 0, 1], // Den of Evil: under the rain
    17: [1, 0, 0], // Burial Grounds
    27: [0, 0, 1], // Outer Cloister
    39: [1, 0, 0], // Moo Moo Farm
    40: [0, 1, 3], // Lut Gholein: sheltered from the sand
    41: [3, 0, 0], // Rocky Waste
    46: [3, 0, 0], // Canyon of the Magi
    47: [0, 0, 3], // Sewers
    66: [0, 0, 3], // Tal Rasha's Tomb
    74: [0, 0, 0], // Arcane Sanctuary
    75: [1, 1, 0], // Kurast Docks
    83: [1, 0, 0], // Travincal
    84: [0, 0, 1], // Spider Cave
    103: [0, 1, 0], // Pandemonium Fortress
    104: [0, 0, 0], // Outer Steppes: no rain, snow or sand (the ash has its own column)
    108: [0, 0, 0], // Chaos Sanctuary
    109: [2, 1, 0], // Harrogath: snow, town
    113: [0, 0, 2], // Crystalline Passage: under the snow
    117: [2, 0, 0], // Frozen Tundra
    121: [2, 0, 0], // Nihlathak's Temple
    133: [0, 0, 1], // Matron's Den: an Act 1 crypt
    134: [3, 0, 0], // Forgotten Sands: desert
    135: [0, 0, 2], // Furnace of Pain
    136: [1, 0, 0], // Uber Tristram: rain
  };
  for (const [id, [weather, town, under]] of Object.entries(expected)) {
    const got = table.get(Number(id));
    check('level ' + id + ' is weather ' + weather + ', town ' + town + ', under ' + under, got && got.weather === weather && got.town === town && got.under === under,
      got ? [got.weather, got.town, got.under].join(',') : 'missing');
  }
  // the ash storm's own column: 1 it blows here, 2 sheltered from it
  const ashExpected = { 103: 2, 104: 1, 105: 1, 106: 1, 107: 1, 108: 1 };
  check('the ash storm blows in the five outdoor areas of Act 4, and the Pandemonium Fortress is sheltered from it',
    Object.entries(ashExpected).every(([id, ash]) => table.get(Number(id)).ash === ash), Object.keys(ashExpected).map((id) => id + ':' + table.get(Number(id)).ash).join(' '));
  check('...and nowhere else', [...table.entries()].every(([id, l]) => (id in ashExpected ? true : l.ash === 0)));
  check('level 0 is left out', !table.has(0));
  check('Blood Moor lists phantom monsters', table.get(2).normal.length > 0, table.get(2).normal);
  check('a town lists none', table.get(40).normal === '' && table.get(40).hell === '');

  const night = visData(out('hd\\env\\vis\\act2_town_night.json'));
  const storm = visData(out('hd\\env\\vis\\act2_town_day.json'));
  check('desert storm profile has dense fog', storm.depthFogDensity >= 2.4 && storm.heightFogDensity >= 1.3, storm.depthFogDensity + ' / ' + storm.heightFogDensity);
  check('...in a sand colour', storm.depthFogColor.x > storm.depthFogColor.y && storm.depthFogColor.y > storm.depthFogColor.z);
  check('...and is thicker than the calm night', storm.depthFogDensity > night.depthFogDensity * 2, night.depthFogDensity);
  check('...with hard wind and a shorter light', storm.windScale === 1.5 && storm.playerUnitLightDistanceScaler < night.playerUnitLightDistanceScaler);
  check('volumetric fog stays off by default', storm.fogDensity === 0, storm.fogDensity);
  check('the Lost City has the storm profile too', visData(out('hd\\env\\vis\\act2_ruin_day.json')).depthFogDensity >= 2.4);
  check('the tombs do not', visData(out('hd\\env\\vis\\act2_tomb_day.json')).depthFogDensity < 1);
  const dusk = visData(out('hd\\env\\vis\\act2_town_dusk1.json'));
  check('other desert stages stay the calm night', dusk.depthFogDensity === night.depthFogDensity && dusk.windScale === night.windScale);
  check('rain areas still get the lightning profile', keyLight(out('hd\\env\\vis\\act1_outdoors_day.json')).power === 45);

  // the ash storm's look, in the "day" profile of Act 4's three outdoor lighting families
  for (const family of ['act4_mesa', 'act4_lava', 'act4_diab']) {
    const calm = visData(out('hd\\env\\vis\\' + family + '_night.json'));
    const ash = visData(out('hd\\env\\vis\\' + family + '_day.json'));
    check(family + ': the storm profile has dense fog', ash.depthFogDensity >= 2.4 && ash.heightFogDensity >= 1.3, ash.depthFogDensity + ' / ' + ash.heightFogDensity);
    check('...with hard wind and a shorter light', ash.windScale === 1.5 && ash.playerUnitLightDistanceScaler < calm.playerUnitLightDistanceScaler);
    const dusk = visData(out('hd\\env\\vis\\' + family + '_dusk1.json'));
    check('...and its other stages stay the calm night', dusk.heightFogDensity === calm.heightFogDensity && dusk.windScale === calm.windScale);
  }
  const ash = visData(out('hd\\env\\vis\\act4_lava_day.json'));
  const ashKey = keyLight(out('hd\\env\\vis\\act4_lava_day.json')).color;
  check('default look: grey-brown ash lit red-orange', ash.heightFogColor.x > ash.heightFogColor.y && ash.heightFogColor.y > ash.heightFogColor.z && ashKey.x === 1 && ashKey.y > ashKey.z && ashKey.y < 0.7,
    JSON.stringify(ash.heightFogColor) + ' ' + JSON.stringify(ashKey));
  check('volumetric smoke stays off by default', ash.fogDensity === 0, ash.fogDensity);
  check('the Pandemonium Fortress keeps its own lighting', visData(out('hd\\env\\vis\\act4_fort_day.json')).windScale !== 1.5);
}

// ---- sandstorms off ---------------------------------------------------------------------
{
  const out = run({ ...defaults(modInfo.config), sandstormEnabled: false });
  const { options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('sandstorms off: the options line says so', /\tsandstorm=0\t/.test(options), JSON.stringify(options));
  check('sandstorms off: the desert day is the calm night',
    visData(out('hd\\env\\vis\\act2_town_day.json')).depthFogDensity === visData(out('hd\\env\\vis\\act2_town_night.json')).depthFogDensity);
}

// ---- Always night off: nothing may take over the day profile ------------------------------
{
  const out = run({ ...defaults(modInfo.config), alwaysNight: false });
  const { options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('always night off: no special day profiles are announced', options === '#mod\talwaysNight=0\tskyFlash=0\tsandstorm=0\tashstorm=0', JSON.stringify(options));
  check('always night off: the desert day has no storm', visData(out('hd\\env\\vis\\act2_town_day.json')).windScale !== 1.5);
  check('always night off: Act 4 has no ash storm', visData(out('hd\\env\\vis\\act4_mesa_day.json')).windScale !== 1.5);
}

// ---- phantoms off: the table is still written ----------------------------------------------
{
  const out = run({ ...defaults(modInfo.config), phantoms: false });
  const { table } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('phantoms off: the level table is still there', table.size > 100 && table.get(41).weather === 3, table.size);
  check('phantoms off: no monsters listed', [...table.values()].every((l) => l.normal === '' && l.hell === ''));
  check('phantoms off: monstats untouched', out('global\\excel\\monstats.txt') === undefined);
}

// ---- thickness and the experimental fog ---------------------------------------------------
{
  const thin = visData(run({ ...defaults(modInfo.config), sandThickness: 10 })('hd\\env\\vis\\act2_town_day.json'));
  const thick = visData(run({ ...defaults(modInfo.config), sandThickness: 100, sandVolumetric: true })('hd\\env\\vis\\act2_town_day.json'));
  check('thickness setting scales the fog', thin.depthFogDensity < 1.5 && thick.depthFogDensity === 3.5, thin.depthFogDensity + ' / ' + thick.depthFogDensity);
  check('volumetric option switches the volumetric fog on', thick.fogDensity === 1);
}

// ---- ash storms: off, per area, the three looks, thickness --------------------------------
{
  const out = run({ ...defaults(modInfo.config), ashstormEnabled: false });
  const { table, options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('ash storms off: the options line says so, and the sandstorm is still announced', /\tsandstorm=1\tashstorm=0$/.test(options), JSON.stringify(options));
  check('ash storms off: no level is marked for ash', [...table.values()].every((l) => l.ash === 0));
  check('ash storms off: the Act 4 day is the calm night',
    visData(out('hd\\env\\vis\\act4_mesa_day.json')).heightFogDensity === visData(out('hd\\env\\vis\\act4_mesa_night.json')).heightFogDensity
      && visData(out('hd\\env\\vis\\act4_lava_day.json')).windScale !== 1.5);
}
{
  const ashOf = (config) => {
    const { table } = levelTable(run({ ...defaults(modInfo.config), ...config })('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
    return [104, 105, 106, 107, 108, 103].map((id) => table.get(id).ash).join('');
  };
  check('River of Flame option off: calm there, storm in the rest', ashOf({ ashRiver: false }) === '111012', ashOf({ ashRiver: false }));
  check('Chaos Sanctuary option off: calm there, storm in the rest', ashOf({ ashChaos: false }) === '111102', ashOf({ ashChaos: false }));
  check('both off: only the three plains', ashOf({ ashRiver: false, ashChaos: false }) === '111002', ashOf({ ashRiver: false, ashChaos: false }));
}
{
  const look = (name) => {
    const out = run({ ...defaults(modInfo.config), ashLook: name });
    return { fog: visData(out('hd\\env\\vis\\act4_lava_day.json')).heightFogColor, key: keyLight(out('hd\\env\\vis\\act4_lava_day.json')).color };
  };
  const grey = look('ash');
  const ember = look('ember');
  check('grey ash look: grey fog and grey light', grey.fog.x - grey.fog.z < 0.05 && grey.key.x - grey.key.z < 0.1, JSON.stringify(grey));
  check('ember look: red fog and red light', ember.fog.x > ember.fog.y * 3 && ember.key.x > ember.key.y * 3, JSON.stringify(ember));
  const unknown = look('something else');
  check('a look this version does not know falls back to the default', unknown.key.x === 1 && unknown.key.y === 0.5);
  const thin = visData(run({ ...defaults(modInfo.config), ashThickness: 10 })('hd\\env\\vis\\act4_lava_day.json'));
  const thick = visData(run({ ...defaults(modInfo.config), ashThickness: 100, ashVolumetric: true })('hd\\env\\vis\\act4_lava_day.json'));
  check('ash thickness setting scales the fog', thin.depthFogDensity < 1.5 && thick.depthFogDensity === 3.5 && thin.heightFogDensity < thick.heightFogDensity, thin.depthFogDensity + ' / ' + thick.depthFogDensity);
  check('volumetric smoke option switches the volumetric fog on', thick.fogDensity === 1);
}

// ---- the ash storm's meteors ---------------------------------------------------------------
{
  const out = run(defaults(modInfo.config));
  const missiles = out('global\\excel\\missiles.txt');
  const row = (name) => missiles.rows.find((r) => r.Missile === name);
  const meteor = row('hnmeteor');
  const aimed = row('hnmeteorhit');
  const real = row('meteorcenter');
  check('two meteor rows are added, marked for the plugin', meteor && aimed && meteor.Param4 === '7777' && meteor.Param5 === '4243' && aimed.Param4 === '7777' && aimed.Param5 === '4244');
  const serverColumns = ['pSrvHitFunc', 'pSrvDmgFunc', 'HitSubMissile1', 'ResultFlags', 'HitFlags', 'EType', 'EMin', 'EMax', 'MinDamage', 'MaxDamage'];
  check('they do nothing on the server when they land: no hit, no damage, no burning ground',
    [meteor, aimed].every((r) => serverColumns.every((c) => (r[c] || '') === '') && r.GetHit === '0'), JSON.stringify(serverColumns.map((c) => meteor[c])));
  check('the real Meteor does (so the columns checked are the right ones)', real.pSrvHitFunc !== '' && real.HitSubMissile1 === 'meteorfire');
  const clientColumns = ['pCltDoFunc', 'pCltHitFunc', 'CltSubMissile1', 'CltSubMissile2', 'CltHitSubMissile1', 'ProgSound', 'Range', 'Skill', 'CltParam1'];
  check('they keep everything the client shows: the falling rock, its tail, the explosion, the sound',
    [meteor, aimed].every((r) => clientColumns.every((c) => r[c] === real[c]) && r.ClientSend === '1'), JSON.stringify(clientColumns.map((c) => meteor[c])));
  check('the real Meteor is untouched', real.Param4 !== '7777' && real.GetHit === '1');
  check('the lightning row is still there', row('hnlightning') && row('hnlightning').Param5 === '4242');
  const hd = out('hd\\missiles\\missiles.json');
  check("HD: the plain meteor has no ground marker, the aimed one has the Meteor's", hd.hnmeteor === 'null' && hd.hnmeteorhit === 'meteor_target_indicator' && hd.hnlightning === 'cairn_stones_bolt',
    hd.hnmeteor + ' / ' + hd.hnmeteorhit);
  const ids = missiles.rows.map((r) => r['*ID']).filter((id) => id !== '');
  check('row numbers stay in step', new Set(ids).size === ids.length && meteor['*ID'] === String(missiles.rows.indexOf(meteor)));
}
{
  const out = run({ ...defaults(modInfo.config), ashstormEnabled: false });
  const missiles = out('global\\excel\\missiles.txt');
  check('ash storms off: no meteor rows', !missiles.rows.some((r) => /^hnmeteor/.test(r.Missile)) && missiles.rows.some((r) => r.Missile === 'hnlightning'));
  const onlyAsh = run({ ...defaults(modInfo.config), lightning: false })('global\\excel\\missiles.txt');
  check('lightning off: the meteor rows are still added', onlyAsh.rows.some((r) => r.Missile === 'hnmeteor') && !onlyAsh.rows.some((r) => r.Missile === 'hnlightning'));
}

console.log(failures ? failures + ' check(s) FAILED' : 'all checks passed');
process.exit(failures ? 1 : 0);
