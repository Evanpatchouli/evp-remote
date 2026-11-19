import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { createSignalServer } from '../server/signaling.mjs';

const directory = process.argv[2];
const scenario = process.argv[3] || 'direct';
const route = scenario === 'relay' ? 'relay' : 'direct';
if (!directory || !['direct', 'relay', 'expiry'].includes(scenario)) {
  throw new Error('Usage: node tools/device-session-smoke.mjs <probe runtime directory> [direct|relay|expiry]');
}
const turnUrls = (process.env.EVP_TURN_URLS || '').split(',').map(url => url.trim()).filter(Boolean);
if (route === 'relay' && (!turnUrls.length || !process.env.EVP_TURN_SECRET)) {
  throw new Error('A reachable test TURN server and its signing secret are required.');
}

const accessKey = randomBytes(32).toString('base64url');
const profiles = await mkdtemp(resolve(tmpdir(), 'evp-devices-'));
const childEnv = { ...process.env };
for (const key of Object.keys(childEnv)) {
  if (/^EVP_(PROBE_|TURN_SECRET$|HOST_KEY$)/i.test(key)) delete childEnv[key];
}
const children = new Set();
let server;
let port = 0;

async function startBroker() {
  server = createSignalServer({ hostKey: accessKey, turnUrls, turnSecret: process.env.EVP_TURN_SECRET });
  await new Promise((resolve, reject) => server.once('error', reject).listen(port, '127.0.0.1', resolve));
  port = server.address().port;
}

async function stopBroker() {
  if (!server) return;
  const stopping = server;
  server = undefined;
  await new Promise((resolve, reject) => stopping.close(error => error ? reject(error) : resolve()));
}

function launch(role, profile, stage, extra, onLine = () => {}) {
  const child = spawn(resolve(directory, 'EvpUnifiedProbe.exe'), [], {
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
    env: {
      ...childEnv, EVP_PROBE_ROLE: role, EVP_PROBE_DEVICE_STAGE: stage,
      EVP_PROBE_DEVICE_STORE: resolve(profiles, `${profile}.dat`),
      EVP_PROBE_URL: `ws://127.0.0.1:${port}/`, EVP_PROBE_ROUTE: route, ...extra,
    },
  });
  children.add(child);
  let errors = '';
  let report;
  child.stderr.setEncoding('utf8').on('data', part => { errors = (errors + part).slice(-2048); });
  return new Promise((resolve, reject) => {
    createInterface({ input: child.stdout }).on('line', line => {
      if (!line.startsWith('{')) {
        if (stage === 'device-checks') errors = (errors + line + '\n').slice(-2048);
        return;
      }
      try {
        const data = JSON.parse(line);
        if (data.id && !data.registered) report = data;
        onLine(data);
      } catch (error) {
        reject(error);
      }
    });
    child.once('error', reject);
    child.once('close', code => {
      children.delete(child);
      if (code === 0 && (report || stage === 'device-checks')) resolve(report);
      else reject(new Error(`${stage} ${role} exited ${code}: ${errors}`));
    });
  });
}

function channel() {
  let publish;
  let fail;
  const received = new Promise((resolve, reject) => { publish = resolve; fail = reject; });
  return { received, publish, fail };
}

async function pair(stage, hostProfile, viewerProfile) {
  const code = channel();
  const viewerDone = resolve(profiles, `${stage}.done`);
  let displayedCode;
  let issuedAt;
  const host = launch('host', hostProfile, stage, { EVP_PROBE_KEY: accessKey, EVP_PROBE_VIEWER_DONE: viewerDone }, data => {
    if (data.pairingCode) {
      displayedCode = data.pairingCode;
      issuedAt = performance.now();
      if (stage !== 'expired-pair') code.publish(displayedCode);
    }
    if (stage === 'expired-pair' && data.expired) {
      assert.ok(performance.now() - issuedAt >= 299000, 'pairing code expired too early');
      code.publish(displayedCode);
    }
  });
  host.then(() => code.fail(new Error(`Host did not generate a code for ${stage}`)), code.fail);
  const valid = await code.received;
  const target = valid.split('.')[1];
  const submitted = stage === 'wrong-pair' ? `EVP1.${target}.${randomBytes(32).toString('base64url')}` : valid;
  const viewer = launch('viewer', viewerProfile, stage, {
    EVP_PROBE_KEY: accessKey, EVP_PROBE_PAIRING_CODE: submitted, EVP_PROBE_TARGET: target,
  }).then(async report => { await writeFile(viewerDone, 'done'); return report; });
  const [hostReport, viewerReport] = await Promise.all([host, viewer]);
  assert.equal(hostReport.passed, true);
  assert.equal(viewerReport.passed, true);
  assert.notEqual(hostReport.id, viewerReport.id);
  if (stage === 'pair' || stage === 'auto-pair') {
    assert.equal(hostReport.id, viewerReport.peer);
    assert.equal(viewerReport.id, hostReport.peer);
  } else {
    assert.ok(!hostReport.peer && !viewerReport.peer);
  }
  return [hostReport.id, viewerReport.id];
}

async function control(stage, hostProfile, hostId, viewerProfile, viewerId, probeCase) {
  const online = channel();
  const input = channel();
  let restart;
  const viewerDone = resolve(profiles, `${stage}.done`);
  const caseEnv = probeCase ? { EVP_PROBE_CASE: probeCase } : {};
  const host = launch('host', hostProfile, stage, { ...caseEnv, EVP_PROBE_VIEWER_DONE: viewerDone }, data => {
    if (data.registered) online.publish(data.id);
    if (data.inputDelivered && stage === 'disconnect' && !restart) {
      restart = (async () => {
        await stopBroker();
        await startBroker();
      })();
      restart.then(input.publish, input.fail);
    }
  });
  host.then(() => online.fail(new Error(`Host did not register for ${stage}`)), online.fail);
  assert.equal(await online.received, hostId);
  const viewer = launch('viewer', viewerProfile, stage, { ...caseEnv, EVP_PROBE_TARGET: hostId })
    .then(async report => { await writeFile(viewerDone, 'done'); return report; });
  const [hostReport, viewerReport] = await Promise.all(stage === 'disconnect' ? [host, viewer, input.received] : [host, viewer]);
  assert.equal(hostReport.passed, true);
  assert.equal(hostReport.id, hostId);
  assert.equal(viewerReport.id, viewerId);
  if (['locked', 'disabled', 'forgotten'].includes(stage)) {
    assert.equal(hostReport.videoStarts, 0);
    assert.equal(viewerReport.passed, true);
    return;
  }
  assert.equal(hostReport.inputDelivered ?? hostReport.noResumedMedia, true);
  if (stage === 'disconnect') assert.equal(hostReport.reregistered, true);
  else assert.equal(hostReport.mediaStops, 1);
  if (stage === 'revoke') assert.equal(hostReport.releasedKeys, 1);
  assert.equal(viewerReport.route, route);
  assert.equal(viewerReport.receivedAudio, probeCase !== 'audio-unavailable');
  assert.ok(viewerReport.presents >= 4);
  assert.ok(viewerReport.gpuDecoded + viewerReport.cpuDecoded >= 4);
  assert.equal(viewerReport.presentationFailures, 0);
}

async function verifyDevices() {
  if (scenario === 'expiry') {
    await pair('expired-pair', 'expiry-host', 'expiry-viewer');
    console.log(JSON.stringify({ pairingCodeExpired: true, expiredRequestRejected: true }));
    return;
  }
  if (route === 'direct') {
    await launch('device-checks', 'checks', 'device-checks', {});
    await pair('wrong-pair', 'wrong-host', 'wrong-viewer');
    await pair('changed-code', 'changed-host', 'changed-viewer');
    await pair('deny-pair', 'deny-host', 'deny-viewer');
    await pair('auto-pair', 'auto-host', 'auto-viewer');
  }
  const [firstId, secondId] = await pair('pair', 'first', 'second');
  await stopBroker();
  await startBroker();
  await control('control', 'first', firstId, 'second', secondId);
  await control(route === 'direct' ? 'revoke' : 'control', 'second', secondId, 'first', firstId);
  if (route === 'direct') {
    await control('control', 'first', firstId, 'second', secondId, 'audio-unavailable');
    await control('disabled', 'second', secondId, 'first', firstId);
    await control('locked', 'first', firstId, 'second', secondId);
    await control('lock-active', 'first', firstId, 'second', secondId);
    await control('session-disconnect', 'first', firstId, 'second', secondId);
    await control('disconnect', 'first', firstId, 'second', secondId);
    await stopBroker();
    const removed = await launch('host', 'first', 'offline-remove', {});
    assert.equal(removed.passed, true);
    assert.equal(removed.previousPeer, secondId);
    await startBroker();
    await control('forgotten', 'first', firstId, 'second', secondId);
  }
  console.log(JSON.stringify({ route, pairedAfterRestart: true, bidirectional: true, decodedAndPresented: true,
    inputDelivered: true, ...(route === 'direct' ? { wrongCodeRejected: true, pairingDeclined: true,
      codeReplacedOnEdit: true, preapprovedPairingWithoutMedia: true,
      consumedCodeRefused: true, lockedRefused: true, activeDesktopLossStopped: true, sessionDisconnectStopped: true,
      disabledRefused: true, audioUnavailablePreservedControl: true, releasedKeysOnRevoke: true, reregisteredWithoutResuming: true,
      removedWhileOffline: true, rejectedAfterUnpair: true } : {}) }));
}

try {
  await startBroker();
  await verifyDevices();
} finally {
  await Promise.all([...children].map(child => new Promise(resolve => {
    child.once('close', resolve);
    child.kill();
  })));
  await stopBroker();
  await rm(profiles, { recursive: true, force: true });
}
