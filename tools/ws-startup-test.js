'use strict';

const assert = require('node:assert/strict');
const { it } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const source = fs.readFileSync(path.join(__dirname, '../wled00/wled_server.cpp'), 'utf8');
const startup = source.match(/^void initServer\(\)\s*\{[\s\S]*?^\}/m);
assert(startup, 'The server startup function must exist');
const registration = [...startup[0].matchAll(/#ifdef WLED_ENABLE_WEBSOCKETS[^\n]*\n[\s\S]*?#endif/g)]
  .find(match => match[0].includes('server.addHandler(&ws)'));
assert(registration, 'WebSocket registration must be guarded for builds without WebSockets');

const compiler = process.env.CXX || 'c++';
const available = spawnSync(compiler, ['--version']).status === 0;

for (const enabled of [true, false]) {
  it(enabled ? 'AP-only server startup handles an effect command without station initialization'
    : 'server startup supports builds without WebSockets', {
    skip: available ? false : 'Set CXX to a C++ compiler to run the startup self-check'
  }, () => {
    // Compile the actual startup block against a small server/socket model.
    const program = `
${enabled ? '#define WLED_ENABLE_WEBSOCKETS' : ''}
using Callback = void (*)(int&);
constexpr void wsEvent(int& effect) { effect = 1; }
struct Socket {
  Callback callback = nullptr;
  constexpr void onEvent(Callback handler) { callback = handler; }
};
struct Server {
  bool registered = false;
  Callback callback = nullptr;
  constexpr void addHandler(Socket* socket) { registered = true; callback = socket->callback; }
  constexpr void receive(int& effect) const { if (callback) callback(effect); }
};
constexpr bool apOnlyStartup() {
  Socket ws;
  Server server;
  ${registration[0]}
  int effect = 210;
  server.receive(effect);
  return ${enabled ? 'server.registered && ws.callback && effect == 1'
    : '!server.registered && !ws.callback && effect == 210'};
}
static_assert(apOnlyStartup(), "AP-only WebSocket command dispatch must not depend on station initialization");
`;
    const result = spawnSync(compiler, ['-std=c++14', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', '-x', 'c++', '-'], {
      input: program, encoding: 'utf8'
    });
    assert.equal(result.status, 0, result.error ? result.error.message : result.stderr);
  });
}
