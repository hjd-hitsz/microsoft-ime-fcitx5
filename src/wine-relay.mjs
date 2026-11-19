import { spawn } from 'node:child_process';
import { chmodSync } from 'node:fs';
import { createServer } from 'node:net';
import { createInterface } from 'node:readline';
import { startSettingsServer } from './settings-server.mjs';

const [, , socketPath, command, ...args] = process.argv;
if (!socketPath || !command) throw new Error('Expected a Unix socket path and an engine command');

process.umask(0o077);
const socketActivation = Number(process.env.LISTEN_PID) === process.pid && process.env.LISTEN_FDS === '1';
let worker;
let client;
let http;
let stopping = false;
let pending;
let serial = 0;

const socketServer = createServer((socket) => {
  if (client || !worker?.ready || pending) {
    socket.destroy();
    return;
  }
  const connection = { socket, worker };
  client = connection;
  const lines = createInterface({ input: socket, crlfDelay: Infinity });
  connection.lines = lines;
  if (worker.resetting) lines.pause();
  lines.on('error', () => socket.destroy());
  lines.on('line', (line) => {
    if (client !== connection || !/^[HKRS] [0-9 ]{1,100}$/.test(line)) return;
    if (!connection.worker.process.stdin.write(line + '\n')) lines.pause();
  });
  const resume = () => {
    if (!connection.worker.resetting) lines.resume();
  };
  connection.worker.process.stdin.on('drain', resume);
  socket.on('error', () => socket.destroy());
  socket.on('close', () => {
    lines.close();
    connection.worker.process.stdin.off('drain', resume);
    if (client !== connection) return;
    client = undefined;
    if (!stopping && connection.worker.ready && !connection.worker.resetting &&
        !connection.worker.process.stdin.destroyed) {
      connection.worker.resetting = true;
      connection.worker.process.stdin.write('R 0\n');
    }
  });
});

function stop(status) {
  if (stopping) return;
  stopping = true;
  process.exitCode = status;
  client?.socket.destroy();
  if (socketServer.listening) socketServer.close();
  http?.close();
  finishUpdate(new Error('微软引擎服务正在关闭。'));
  closeWorker(worker);
}

function closeWorker(current) {
  if (!current) return;
  current.ready = false;
  current.process.stdin.end();
  if (current.process.exitCode === null && current.process.signalCode === null && current.process.pid) {
    current.stopTimer = setTimeout(() => {
      try {
        process.kill(-current.process.pid, 'SIGTERM');
      } catch (error) {
        if (error.code !== 'ESRCH') console.error(error.message);
      }
    }, 3000);
    current.stopTimer.unref();
  }
}

function finishUpdate(error, settings) {
  if (!pending) return;
  const update = pending;
  pending = undefined;
  clearTimeout(update.timer);
  if (error) update.reject(error);
  else update.resolve(settings);
}

function readNative() {
  if (pending || !worker?.ready) return null;
  return worker.settings;
}

function updateNative(settings) {
  if (pending || !worker?.ready) return Promise.reject(new Error('微软引擎正在加载设置。'));
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => finishUpdate(new Error('微软引擎设置回读超时。')), 10000);
    pending = { id: ++serial, settings, resolve, reject, timer, phase: 'write' };
    worker.process.stdin.write([
      'E', serial, Number(settings.AutoCorrection), Number(settings.FuzzyEnabled),
      settings.FuzzyPairs, Number(settings.SelfLearning),
    ].join(' ') + '\n');
  });
}

function startWorker() {
  const process = spawn(command, args, { detached: true, stdio: ['pipe', 'pipe', 'inherit'] });
  const current = { process, ready: false, resetting: false, restart: false };
  worker = current;
  createInterface({ input: process.stdout, crlfDelay: Infinity }).on('line', (line) => {
    if (worker !== current || stopping) return;
    const native = /^N (\d+) (\d+) (\d+) (\d+)$/.exec(line);
    if (native) {
      current.settings = {
        AutoCorrection: Number(native[1]) !== 0,
        FuzzyEnabled: Number(native[2]) !== 0,
        FuzzyPairs: Number(native[3]),
        SelfLearning: Number(native[4]) !== 0,
      };
      return;
    }
    if (line === 'READY') {
      if (!current.settings) {
        console.error('Microsoft engine ready without registry settings');
        stop(1);
        return;
      }
      current.ready = true;
      if (!http) {
        http = startSettingsServer({ readNative, updateNative });
        if (socketActivation) socketServer.listen({ fd: 3 });
        else socketServer.listen(socketPath, () => chmodSync(socketPath, 0o600));
      }
      if (pending?.phase === 'read') {
        const valid = Object.keys(pending.settings).every((key) => current.settings[key] === pending.settings[key]);
        finishUpdate(valid ? null : new Error('微软引擎重新加载后的设置与请求不一致。'), current.settings);
      }
      return;
    }

    const reply = /^W (\d+) (\d+)$/.exec(line);
    if (reply && pending?.id === Number(reply[1])) {
      if (Number(reply[2]) !== 0) {
        finishUpdate(new Error(`微软注册表写入失败（错误码 ${reply[2]}）。`));
        return;
      }
      pending.phase = 'read';
      current.restart = true;
      current.ready = false;
      client?.socket.destroy();
      closeWorker(current);
      return;
    }
    if (current.resetting) {
      if (line === 'A 0 1') {
        current.resetting = false;
        if (client?.worker === current) client.lines.resume();
      }
      return;
    }
    if (client?.worker === current && /^[ACUM] /.test(line)) client.socket.write(line + '\n');
  });
  process.stdin.on('error', () => {
    if (!stopping && !current.restart) stop(1);
  });
  process.on('error', (error) => {
    console.error(error.message);
    stop(1);
  });
  process.on('exit', (status) => {
    clearTimeout(current.stopTimer);
    if (stopping) return;
    if (current.restart) startWorker();
    else stop(status ?? 1);
  });
}

socketServer.on('error', (error) => {
  console.error(error.message);
  stop(1);
});
process.on('SIGTERM', () => stop(0));
process.on('SIGINT', () => stop(0));
startWorker();
