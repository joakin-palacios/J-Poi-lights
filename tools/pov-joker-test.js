'use strict';

const assert = require('node:assert/strict');
const { it } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../wled00/data/index.js'), 'utf8');
const configure = source.match(/^function configurePovJokerSlider\(enabled\)\r?\n\{[\s\S]*?^\}/m);
assert(configure, 'The joker slider configuration must exist');

function sliderContext(value) {
  const slider = {};
  const context = vm.createContext({
    povJokerStateValue: value,
    gId: id => {
      assert.equal(id, 'sliderC2');
      return slider;
    },
    updateTrail: element => assert.equal(element, slider)
  });
  vm.runInContext(configure[0], context);
  return { slider, context };
}

it('POV joker control has three integer positions and clamps legacy state', () => {
  for (const [value, expected] of [[0, 1], [1, 1], [2, 2], [3, 3], [255, 3]]) {
    const { slider, context } = sliderContext(value);
    vm.runInContext('configurePovJokerSlider(true)', context);
    assert.deepEqual(slider, { min: 1, max: 3, step: 1, value: expected });
  }
});

it('leaving POV restores the normal custom slider range and unclamped state', () => {
  const { slider, context } = sliderContext(200);
  vm.runInContext('configurePovJokerSlider(true); configurePovJokerSlider(false)', context);
  assert.deepEqual(slider, { min: 0, max: 255, step: 1, value: 200 });
});
