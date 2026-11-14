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
      const lines = fs.readFileSync(real, 'latin1').split(/\r?\n/).filter((line) => line.length);
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
    else if (/^\d+$/.test(cells[0])) table.set(Number(cells[0]), { normal: cells[1], hell: cells[2], weather: Number(cells[3]), town: Number(cells[4]), under: Number(cells[5]), cells: cells.length });
  }
  return { table, options };
}

const visData = (def) => def.entities.find((e) => e.name === 'vis_data').components[0];
const keyLight = (def) => def.entities.find((e) => e.name === 'global_key_light').components.find((c) => c.type === 'DirectionalLightDefinitionComponent');

// ---- everything on (the defaults) -------------------------------------------------------
{
  const out = run(defaults(modInfo.config));
  const { table, options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('options line says what was installed', options === '#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1', JSON.stringify(options));
  check('every level line has six columns', [...table.values()].every((l) => l.cells === 6));

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
}

// ---- sandstorms off ---------------------------------------------------------------------
{
  const out = run({ ...defaults(modInfo.config), sandstormEnabled: false });
  const { options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('sandstorms off: the options line says so', /sandstorm=0$/.test(options), JSON.stringify(options));
  check('sandstorms off: the desert day is the calm night',
    visData(out('hd\\env\\vis\\act2_town_day.json')).depthFogDensity === visData(out('hd\\env\\vis\\act2_town_night.json')).depthFogDensity);
}

// ---- Always night off: nothing may take over the day profile ------------------------------
{
  const out = run({ ...defaults(modInfo.config), alwaysNight: false });
  const { options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('always night off: no special day profiles are announced', options === '#mod\talwaysNight=0\tskyFlash=0\tsandstorm=0', JSON.stringify(options));
  check('always night off: the desert day has no storm', visData(out('hd\\env\\vis\\act2_town_day.json')).windScale !== 1.5);
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

console.log(failures ? failures + ' check(s) FAILED' : 'all checks passed');
process.exit(failures ? 1 : 0);
