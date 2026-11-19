import assert from 'node:assert/strict';
import { createConnection } from 'node:net';
import { createInterface } from 'node:readline';
import { setTimeout as delay } from 'node:timers/promises';

const { IME_SETTINGS_PORT, MS_IME_SOCKET } = process.env;
const url = `http://127.0.0.1:${IME_SETTINGS_PORT}/api/settings`;
const display = { PageSize: 3, ShiftSwitch: true, ChinesePunctuation: true, FullWidth: false };

async function getSettings() {
  let last;
  for (let count = 0; count < 40; count++) {
    try {
      const response = await fetch(url, { signal: AbortSignal.timeout(3000) });
      if (response.ok) return await response.json();
      last = new Error(`Settings returned HTTP ${response.status}`);
    } catch (error) {
      last = error;
    }
    await delay(100);
  }
  throw last;
}

async function apply(native) {
  const response = await fetch(url, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json', Origin: new URL(url).origin },
    body: JSON.stringify({ native, display }),
    signal: AbortSignal.timeout(12000),
  });
  const state = await response.json();
  assert.equal(response.status, 200, JSON.stringify(state));
  assert.deepEqual(state.native, native);
  assert.deepEqual(state.display, display);
  assert.deepEqual((await getSettings()).native, native);
}

async function testInput(input, expected) {
  const socket = createConnection(MS_IME_SOCKET);
  socket.setTimeout(4000, () => socket.destroy(new Error('IME query timed out')));
  const lines = createInterface({ input: socket, crlfDelay: Infinity });
  lines.on('error', () => socket.destroy());
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });
  socket.write('H 1 3 1\n' + [...input].map((letter, index) =>
    `K ${index + 2} ${letter.toUpperCase().charCodeAt(0)} 0\n`).join(''));
  try {
    for await (const line of lines) {
      const [type, , , preedit, ...values] = line.split(' ');
      if (type !== 'U' || Buffer.from(preedit, 'hex').toString('utf8') !== input) continue;
      assert.equal(values.length, display.PageSize);
      const top = Buffer.from(values[0], 'hex').toString('utf8');
      assert.equal(top, expected);
      console.log(`${input} → ${top}`);
      return;
    }
    assert.fail('The engine did not return a candidate list');
  } finally {
    lines.close();
    socket.destroy();
    await delay(80);
  }
}

const initial = await getSettings();
try {
  await apply({ AutoCorrection: true, FuzzyEnabled: false, FuzzyPairs: 1, SelfLearning: true });
  await testInput('nihoa', '你好');
  await testInput('zongguo', '总过');
  await apply({ AutoCorrection: false, FuzzyEnabled: true, FuzzyPairs: 1, SelfLearning: false });
  await testInput('nihoa', '你吼啊');
  await testInput('zongguo', '中国');
  const forbidden = await fetch(url, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json', Origin: 'https://example.invalid' },
    body: JSON.stringify({ display, native: initial.native }),
  });
  assert.equal(forbidden.status, 403);
  console.log('Native registry options were reloaded and changed the real engine candidates.');
} finally {
  await apply(initial.native);
}
