'use strict';

const assert = require('node:assert/strict');
const { it } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../wled00/data/index.js'), 'utf8');
const configure = source.match(/^function configurePovCycleSlider\(enabled\)\r?\n\{[\s\S]*?^\}/m);
const setOption = source.match(/^function setOption\(i=1, v=false\)\r?\n\{[\s\S]*?^\}/m);
assert(configure && setOption);

function controls(value, checked) {
  const slider = {};
  const checkbox = { checked };
  const wrapper = { hidden: null, classList: { toggle: (name, hidden) => {
    assert.equal(name, 'hide'); wrapper.hidden = hidden;
  } } };
  let sent;
  const context = vm.createContext({
    povRepeatStateValue: value, povCycleControl: false,
    gId: id => ({ sliderC3: slider, slider4: wrapper, checkO1: checkbox })[id],
    updateTrail: element => assert.equal(element, slider),
    requestJson: object => { sent = object; }
  });
  vm.runInContext(configure[0] + '\n' + setOption[0], context);
  return { slider, checkbox, wrapper, context, sent: () => sent };
}

it('cycle repeats use integer positions 1–20 and are hidden when unchecked', () => {
  for (const [value, expected] of [[0, 1], [1, 1], [12, 12], [20, 20], [31, 20]]) {
    const ui = controls(value, false);
    vm.runInContext('configurePovCycleSlider(true)', ui.context);
    assert.deepEqual(ui.slider, { min: 1, max: 20, step: 1, value: expected });
    assert.equal(ui.wrapper.hidden, true);
  }
});

it('checkbox shows and hides the slider immediately and sends normal WLED state', () => {
  const ui = controls(5, false);
  vm.runInContext('configurePovCycleSlider(true); setOption(1, true)', ui.context);
  assert.equal(ui.wrapper.hidden, false);
  assert.equal(ui.sent().seg.o1, true);
  vm.runInContext('setOption(1, false)', ui.context);
  assert.equal(ui.wrapper.hidden, true);
  assert.equal(ui.sent().seg.o1, false);
  assert.equal(ui.slider.value, 5);
});

it('leaving POV restores C3 range/state and leaves other effects visibility alone', () => {
  const ui = controls(31, true);
  vm.runInContext('configurePovCycleSlider(true)', ui.context);
  ui.wrapper.hidden = 'controlled by the next effect';
  vm.runInContext('configurePovCycleSlider(false)', ui.context);
  assert.deepEqual(ui.slider, { min: 0, max: 31, step: 1, value: 31 });
  assert.equal(ui.wrapper.hidden, 'controlled by the next effect');
});
