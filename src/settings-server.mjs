import { execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { writeFileSync } from 'node:fs';
import { mkdir, readFile, rename, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { homedir } from 'node:os';
import { dirname, join } from 'node:path';
import { promisify } from 'node:util';

const exec = promisify(execFile);
const method = 'microsoft-chinese-keyboard';
const defaults = Object.freeze({
  PageSize: 5,
  ShiftSwitch: true,
  ChinesePunctuation: true,
  FullWidth: false,
});
const staticFiles = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/app.mjs', ['app.mjs', 'text/javascript; charset=utf-8']],
  ['/style.css', ['style.css', 'text/css; charset=utf-8']],
]);

function validateDisplay(input) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new TypeError('设置格式无效。');
  const result = { ...defaults };
  for (const [name, fallback] of Object.entries(defaults)) {
    const value = input[name];
    if (typeof fallback === 'boolean' && typeof value === 'boolean') result[name] = value;
    else if (name === 'PageSize' && Number.isInteger(value) && value >= 3 && value <= 9) result[name] = value;
    else throw new TypeError(`设置 ${name} 的值无效。`);
  }
  return result;
}

function validateNative(input) {
  if (!input || typeof input !== 'object' ||
      typeof input.AutoCorrection !== 'boolean' || typeof input.FuzzyEnabled !== 'boolean' ||
      typeof input.SelfLearning !== 'boolean' || !Number.isInteger(input.FuzzyPairs) ||
      input.FuzzyPairs < 0 || input.FuzzyPairs > 0xffffffff) {
    throw new TypeError('微软引擎设置格式无效。');
  }
  return {
    AutoCorrection: input.AutoCorrection,
    FuzzyEnabled: input.FuzzyEnabled,
    FuzzyPairs: input.FuzzyPairs,
    SelfLearning: input.SelfLearning,
  };
}

export function startSettingsServer({ readNative, updateNative }) {
  const port = Number(process.env.IME_SETTINGS_PORT ?? 17865);
  if (!Number.isInteger(port) || port < 1024 || port > 65535) throw new Error('Invalid settings port');
  const config = join(process.env.XDG_CONFIG_HOME || join(homedir(), '.config'),
    'fcitx5', 'conf', method + '.conf');
  const permittedHosts = new Set([`127.0.0.1:${port}`, `localhost:${port}`]);
  const assets = new URL('../web/', import.meta.url);
  let pendingWrite = Promise.resolve();

  const server = createServer(async (request, response) => {
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    response.setHeader('Content-Security-Policy', "default-src 'self'; frame-ancestors 'none'; base-uri 'none'");

    const json = (status, body) => {
      response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
      response.end(JSON.stringify(body));
    };
    if (!permittedHosts.has(request.headers.host)) {
      json(403, { error: '请从本机设置地址访问。' });
      return;
    }

    try {
      if (request.url === '/api/settings' && request.method === 'GET') {
        let ini;
        try {
          ini = await readFile(config, 'utf8');
        } catch (error) {
          if (error.code !== 'ENOENT') throw error;
          ini = '';
        }
        const settings = { ...defaults };
        for (const line of ini.split(/\r?\n/)) {
          const match = /^([A-Za-z]+)\s*=\s*(\S+)\s*$/.exec(line);
          if (!match) continue;
          const [, key, value] = match;
          if (typeof defaults[key] === 'boolean' && /^(true|false)$/i.test(value)) {
            settings[key] = value.toLowerCase() === 'true';
          }
          if (key === 'PageSize' && /^[3-9]$/.test(value)) settings[key] = Number(value);
        }
        const native = readNative();
        if (native) json(200, { native, display: settings, cloudEnabled: false });
        else json(503, { error: '微软引擎正在加载设置。' });
        return;
      }

      if (request.url === '/api/settings' && request.method === 'PUT') {
        const origin = request.headers.origin;
        if (origin && origin !== 'http://' + request.headers.host) {
          json(403, { error: '请求来自其他网站。' });
          return;
        }
        if (!/^application\/json(?:\s*;|$)/i.test(request.headers['content-type'] ?? '')) {
          json(415, { error: '设置请求需要 JSON 格式。' });
          return;
        }
        let body = '';
        for await (const chunk of request) {
          body += chunk.toString('utf8');
          if (Buffer.byteLength(body) > 4096) {
            json(413, { error: '设置请求过大。' });
            return;
          }
        }
        let settings;
        let native;
        try {
          const input = JSON.parse(body);
          settings = validateDisplay(input?.display);
          native = validateNative(input?.native);
        } catch (error) {
          json(400, { error: error instanceof SyntaxError ? '设置 JSON 格式无效。' : error.message });
          return;
        }

        const update = pendingWrite.then(async () => {
          const previous = readNative();
          if (!previous) throw new Error('微软引擎正在加载设置。');
          if (Object.keys(native).some((key) => native[key] !== previous[key])) {
            native = await updateNative(native);
          }
          await mkdir(dirname(config), { recursive: true, mode: 0o700 });
          const temporary = config + '.' + randomUUID();
          const text = Object.entries(settings).map(([key, value]) =>
            `${key}=${typeof value === 'boolean' ? value ? 'True' : 'False' : value}\n`).join('');
          try {
            await writeFile(temporary, text, { flag: 'wx', mode: 0o600 });
            await rename(temporary, config);
          } finally {
            await rm(temporary, { force: true });
          }
        });
        pendingWrite = update.catch(() => {});
        try {
          await update;
        } catch (error) {
          json(503, { error: error.message });
          return;
        }
        let applied = true;
        try {
          await exec('busctl', ['--user', 'call', 'org.fcitx.Fcitx5', '/controller',
            'org.fcitx.Fcitx.Controller1', 'ReloadAddonConfig', 's', method], { timeout: 3000 });
        } catch {
          applied = false;
        }
        json(200, { native, display: settings, applied, cloudEnabled: false });
        return;
      }

      const asset = staticFiles.get(request.url);
      if (asset && request.method === 'GET') {
        const [name, type] = asset;
        const body = await readFile(new URL(name, assets));
        response.writeHead(200, { 'Content-Type': type });
        response.end(body);
        return;
      }
      json(404, { error: '页面不存在。' });
    } catch (error) {
      console.error('Settings request:', error.message);
      if (!response.headersSent) json(500, { error: '无法读写 Fcitx5 设置。' });
    }
  });

  server.on('error', (error) => console.error('Could not start local settings:', error.message));
  server.listen(port, '127.0.0.1', () => {
    const url = `http://127.0.0.1:${port}/`;
    if (process.env.XDG_RUNTIME_DIR) {
      writeFileSync(join(process.env.XDG_RUNTIME_DIR, 'microsoft-ime-engine.url'), url + '\n', { mode: 0o600 });
    }
    console.error(`Microsoft IME settings: ${url}`);
  });
  return server;
}
