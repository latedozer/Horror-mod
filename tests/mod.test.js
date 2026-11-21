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
    else if (/^\d+$/.test(cells[0])) table.set(Number(cells[0]), { normal: cells[1], hell: cells[2], weather: Number(cells[3]), town: Number(cells[4]), under: Number(cells[5]), ash: Number(cells[6]), blizzard: Number(cells[7]), monsoon: Number(cells[8]), cells: cells.length });
  }
  return { table, options };
}

const visData = (def) => def.entities.find((e) => e.name === 'vis_data').components[0];
const keyLight = (def) => def.entities.find((e) => e.name === 'global_key_light').components.find((c) => c.type === 'DirectionalLightDefinitionComponent');

// ---- everything on (the defaults) -------------------------------------------------------
{
  const out = run(defaults(modInfo.config));
  const { table, options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('options line says what was installed', options === '#mod\talwaysNight=1\tskyFlash=1\tsandstorm=1\tashstorm=1\tblizzard=1\tmonsoon=1', JSON.stringify(options));
  check('every level line has nine columns', [...table.values()].every((l) => l.cells === 9));

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
  // the blizzard's own column: 1 it blows here, 2 sheltered from it
  const blizzardBlows = [110, 111, 112, 117, 120, 121];
  check('the blizzard blows on the open snowfields of Act 5', blizzardBlows.every((id) => table.get(id).blizzard === 1), blizzardBlows.map((id) => id + ':' + table.get(id).blizzard).join(' '));
  check('...Harrogath and everything underground in Act 5 are sheltered from it',
    [109, 113, 114, 118, 122, 128, 131, 132].every((id) => table.get(id).blizzard === 2));
  check('...and no level outside Act 5 has it', [1, 2, 8, 40, 41, 75, 103, 104, 108, 133, 134, 136].every((id) => table.get(id).blizzard === 0));
  check('...while the snow columns stay as before (an older plugin reads Act 5 as it always did)', table.get(110).weather === 2 && table.get(113).under === 2);
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

  // the blizzard's look, in the "day" profile of Act 5's snowy families, in place of the lightning
  for (const family of ['expansion_siege', 'expansion_mountaintop', 'expansion_ruins_snow']) {
    const calm = visData(out('hd\\env\\vis\\' + family + '_night.json'));
    const snow = visData(out('hd\\env\\vis\\' + family + '_day.json'));
    check(family + ': the blizzard profile has dense pale fog', snow.depthFogDensity >= 2.4 && snow.heightFogDensity >= 1.3 && snow.heightFogColor.z > snow.heightFogColor.x,
      snow.depthFogDensity + ' / ' + JSON.stringify(snow.heightFogColor));
    check('...with hard wind and a shorter light', snow.windScale === 1.5 && snow.playerUnitLightDistanceScaler < calm.playerUnitLightDistanceScaler);
    const key = keyLight(out('hd\\env\\vis\\' + family + '_day.json'));
    check('...a cold light, and not the lightning look', key.color.z === 1 && key.color.x < key.color.z && key.power !== 45, JSON.stringify(key));
  }
  check('volumetric snow stays off by default', visData(out('hd\\env\\vis\\expansion_siege_day.json')).fogDensity === 0);

  // the monsoon: Act 3's outdoor families carry four looks in a row, so that the lightning stays
  for (const family of ['act3_jungle', 'act3_kurast', 'act3_travincal', 'act3_docktown']) {
    const file = (stage) => out('hd\\env\\vis\\' + family + '_' + stage + '.json');
    const calm = visData(file('night'));
    const storm = visData(file('day'));
    const lit = visData(file('dusk1'));
    check(family + ': the monsoon profile ("day") has dense dark green-grey fog', storm.depthFogDensity >= 2.4 && storm.heightFogDensity >= 1.3 && storm.heightFogColor.y > storm.heightFogColor.x
      && storm.depthFogColor.y < 0.3, storm.depthFogDensity + ' / ' + JSON.stringify(storm.heightFogColor));
    check('...with hard wind and a shorter light, and not the lightning look', storm.windScale === 1.5 && storm.playerUnitLightDistanceScaler < calm.playerUnitLightDistanceScaler
      && keyLight(file('day')).power !== 45);
    check('...the lightning flash is the "dawn1" profile: the night, lit', keyLight(file('dawn1')).power === 45 && visData(file('dawn1')).depthFogDensity === calm.depthFogDensity);
    check('...the "dawn2" profile is the night itself', JSON.stringify(visData(file('dawn2'))) === JSON.stringify(calm) && keyLight(file('dawn2')).power === keyLight(file('night')).power);
    check('...and "dusk1" is the monsoon lit by lightning', lit.depthFogDensity === storm.depthFogDensity && keyLight(file('dusk1')).power === 45);
  }
  check("Act 1's lightning is where it was: the \"day\" profile", keyLight(out('hd\\env\\vis\\act1_outdoors_day.json')).power === 45
    && keyLight(out('hd\\env\\vis\\act1_outdoors_dawn1.json')).power !== 45);
  check('the monsoon blows in the jungles, Kurast and Travincal', [76, 77, 78, 79, 80, 81, 82, 83].every((id) => table.get(id) && table.get(id).monsoon === 1),
    JSON.stringify([76, 83].map((id) => table.get(id))));
  check('...the docks and everything underground in Act 3 shelter from it', [75, 84, 86, 92, 94, 100, 102].every((id) => table.get(id) && table.get(id).monsoon === 2));
  check('...and nowhere else', [1, 2, 40, 41, 103, 104, 109, 110].every((id) => table.get(id) && table.get(id).monsoon === 0));
  check('rain still falls where the monsoon blows', table.get(76).weather === 1 && table.get(84).under === 1);
}

// ---- no leftover of the dust devil preview's pool of light ----------------------------------
{
  const on = run(defaults(modInfo.config));
  check('no pool-of-light row any more', !on('global\\excel\\missiles.txt').rows.some((r) => r.Missile === 'hnglow'));
}

// ---- snakes in Act 3 --------------------------------------------------------------------
{
  const critters = (out, id) => {
    const row = out('global\\excel\\levels.txt').rows.find((r) => r.Id === String(id));
    return [1, 2, 3, 4].map((n) => (row['cmon' + n] || '-') + ':' + (row['cpct' + n] || '-')).join(' ');
  };
  const many = run(defaults(modInfo.config));
  check('snakes, as shipped (many): the snake at 100 in its own slot and in the two free ones, the parrot as it was',
    [76, 77, 78, 79, 80, 81, 82, 83].every((id) => critters(many, id) === 'snake:100 parrot:30 snake:100 snake:100'), critters(many, 76));
  check('...only where the game has snakes: the spider caves, the sewers and Act 1 are untouched',
    critters(many, 84) === 'minispider:30 -:- -:- -:-' && critters(many, 92) === 'rat:100 rat:100 -:- -:-' && critters(many, 8) === 'rat:30 bat:30 -:- -:-', critters(many, 84));
  const more = run({ ...defaults(modInfo.config), act3Snakes: 'more' });
  check('snakes: more = the chance 100, no extra slots', critters(more, 76) === 'snake:100 parrot:30 -:- -:-', critters(more, 76));
  const plain = run({ ...defaults(modInfo.config), act3Snakes: 'vanilla' });
  check('snakes: as in the game = levels.txt is not written at all', plain('global\\excel\\levels.txt') === undefined);
  check('the plugin\'s level table is the same whatever the snakes are',
    many('global\\excel\\d2rloader\\horror-night\\levels.tsv') === plain('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
}

// ---- monsoons off -----------------------------------------------------------------------
{
  const out = run({ ...defaults(modInfo.config), monsoonEnabled: false });
  const { options, table } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('monsoons off: the options line says so', /\tmonsoon=0$/.test(options), JSON.stringify(options));
  check('monsoons off: no level is marked for it', [...table.values()].every((l) => l.monsoon === 0));
  check('monsoons off: Act 3 has the lightning look as its "day" profile again', keyLight(out('hd\\env\\vis\\act3_jungle_day.json')).power === 45
    && visData(out('hd\\env\\vis\\act3_jungle_day.json')).depthFogDensity === visData(out('hd\\env\\vis\\act3_jungle_night.json')).depthFogDensity
    && keyLight(out('hd\\env\\vis\\act3_jungle_dawn1.json')).power !== 45);
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
  check('always night off: no special day profiles are announced', options === '#mod\talwaysNight=0\tskyFlash=0\tsandstorm=0\tashstorm=0\tblizzard=0\tmonsoon=0', JSON.stringify(options));
  check('always night off: Act 5 has no blizzard', visData(out('hd\\env\\vis\\expansion_siege_day.json')).windScale !== 1.5);
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
  check('ash storms off: the options line says so, and the sandstorm is still announced', /\tsandstorm=1\tashstorm=0\tblizzard=1\t/.test(options), JSON.stringify(options));
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

// ---- blizzards: off, thickness, the falling ice --------------------------------------------
{
  const out = run({ ...defaults(modInfo.config), blizzardEnabled: false });
  const { table, options } = levelTable(out('global\\excel\\d2rloader\\horror-night\\levels.tsv'));
  check('blizzards off: the options line says so', /\tashstorm=1\tblizzard=0\t/.test(options), JSON.stringify(options));
  check('blizzards off: no level is marked for it', [...table.values()].every((l) => l.blizzard === 0));
  check('blizzards off: Act 5 gets the lightning look back', keyLight(out('hd\\env\\vis\\expansion_siege_day.json')).power === 45);
  const missiles = out('global\\excel\\missiles.txt');
  check('blizzards off: no ice rows', !missiles.rows.some((r) => /^hnice/.test(r.Missile)) && missiles.rows.some((r) => r.Missile === 'hnmeteor'));
  const thin = visData(run({ ...defaults(modInfo.config), blizzardThickness: 10 })('hd\\env\\vis\\expansion_siege_day.json'));
  const thick = visData(run({ ...defaults(modInfo.config), blizzardThickness: 100, blizzardVolumetric: true })('hd\\env\\vis\\expansion_siege_day.json'));
  check('blizzard thickness setting scales the fog', thin.depthFogDensity < 1.5 && thick.depthFogDensity === 3.5, thin.depthFogDensity + ' / ' + thick.depthFogDensity);
  check('volumetric snow option switches the volumetric fog on', thick.fogDensity === 1);
}
{
  const out = run(defaults(modInfo.config));
  const missiles = out('global\\excel\\missiles.txt');
  const row = (name) => missiles.rows.find((r) => r.Missile === name);
  const ice = row('hnice');
  const aimed = row('hnicehit');
  const fall = row('hnicefall');
  const real = row('meteorcenter');
  check('three ice rows are added, two marked for the plugin', fall && ice && aimed && ice.Param4 === '7777' && ice.Param5 === '4245' && aimed.Param4 === '7777' && aimed.Param5 === '4246');
  const serverColumns = ['pSrvHitFunc', 'pSrvDmgFunc', 'SubMissile1', 'HitSubMissile1', 'ResultFlags', 'HitFlags', 'EType', 'EMin', 'EMax', 'MinDamage', 'MaxDamage'];
  check('they do nothing on the server when they land: no hit, no damage, no fire',
    [ice, aimed].every((r) => serverColumns.every((c) => (r[c] || '') === '') && r.GetHit === '0' && r.ClientSend === '1'), JSON.stringify(serverColumns.map((c) => ice[c])));
  check('they fall the way the Meteor does (its client function, fall time and height)',
    [ice, aimed].every((r) => ['pCltDoFunc', 'pCltHitFunc', 'Range', 'CltParam1', 'CltParam2', 'CltParam3'].every((c) => r[c] === real[c])));
  check('...but what falls is the ice shard, it bursts as a Glacial Spike does on landing, and there is no fiery tail and no fire on the ground',
    [ice, aimed].every((r) => r.CltSubMissile1 === 'hnicefall' && r.CltSubMissile2 === '' && r.CltHitSubMissile1 === 'freezingarrowexp1' && r.CltHitSubMissile2 === 'bluelightmissile'
      && r.CltHitSubMissile3 === 'freezingarrowexp2' && r.CltHitSubMissile4 === '' && r.cHitPar1 === '3' && r.cHitPar2 === '3'
      && r.ProgSound === 'sorceress_blizzard_impact_1' && r.Blue === '255' && r.Red === '81'));
  check('the falling shard is a copy of the Meteor rock', fall.pCltDoFunc === row('meteor').pCltDoFunc && fall.Range === row('meteor').Range);
  check('the real Meteor is untouched', real.Param4 !== '7777' && real.CltSubMissile1 === 'meteor');
  check('the meteor and lightning rows are still there', row('hnmeteor') && row('hnlightning'));
  const hd = out('hd\\missiles\\missiles.json');
  check("HD: the shard is a Glacial Spike, neither ice row has a picture of its own (the plugin shows the aimed one's marker), the aimed meteor keeps the Meteor's circle",
    hd.hnicefall === 'glacial_spike' && hd.hnice === 'null' && hd.hnicehit === 'null' && hd.hnmeteorhit === 'meteor_target_indicator', JSON.stringify([hd.hnicefall, hd.hnice, hd.hnicehit]));
  const ids = missiles.rows.map((r) => r['*ID']).filter((id) => id !== '');
  check('row numbers stay in step', new Set(ids).size === ids.length && aimed['*ID'] === String(missiles.rows.indexOf(aimed)) && fall['*ID'] === String(missiles.rows.indexOf(fall)));
  const crack = run({ ...defaults(modInfo.config), iceMarker: 'crack' })('hd\\missiles\\missiles.json');
  check('the cracking-ice marker option', crack.hnicehit === 'icecrack');
  const circle = run({ ...defaults(modInfo.config), iceMarker: 'meteor' })('hd\\missiles\\missiles.json');
  check("the Meteor's circle marker option", circle.hnicehit === 'meteor_target_indicator');
  const noIce = run({ ...defaults(modInfo.config), blizzardIce: false });
  check('falling ice off: no ice rows, the blizzard still marked', !noIce('global\\excel\\missiles.txt').rows.some((r) => /^hnice/.test(r.Missile))
    && levelTable(noIce('global\\excel\\d2rloader\\horror-night\\levels.tsv')).table.get(110).blizzard === 1);
}

console.log(failures ? failures + ' check(s) FAILED' : 'all checks passed');
process.exit(failures ? 1 : 0);
