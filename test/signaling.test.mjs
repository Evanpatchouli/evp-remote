import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHmac } from 'node:crypto';
import { on, once } from 'node:events';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import { createSignalServer } from '../server/signaling.mjs';

const HOST_KEY = 'test-host-key';
const TURN_SECRET = 'test-turn-secret';
const TURN_URLS = ['turn:relay.example:3478', 'turns:relay.example:5349?transport=tcp'];
const STUN_URLS = ['stun:relay.example:3478'];

async function listen(t, options = {}) {
  const server = createSignalServer({ hostKey: HOST_KEY, ...options });
  t.after(async () => {
    await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  });
  const listening = once(server, 'listening');
  server.listen(0, '127.0.0.1');
  await listening;
  return async () => {
    const socket = new WebSocket(`ws://127.0.0.1:${server.address().port}/`);
    const stream = on(socket, 'message');
    await once(socket, 'open');
    return {
      socket,
      send: message => socket.send(JSON.stringify(message)),
      receive: async () => JSON.parse((await stream.next()).value[0].toString()),
    };
  };
}

async function createInvitation(host) {
  host.send({ type: 'host', key: HOST_KEY });
  const { type, invite } = await host.receive();
  assert.equal(type, 'invitation');
  assert.match(invite, /^[A-Za-z0-9_-]{22}$/);
  return invite;
}

async function join(host, viewer) {
  const invite = await createInvitation(host);
  viewer.send({ type: 'join', invite });
  assert.deepEqual(await viewer.receive(), { type: 'waiting' });
  assert.deepEqual(await host.receive(), { type: 'request' });
  return invite;
}

async function assertError(peer, pattern) {
  const reply = await peer.receive();
  assert.equal(reply.type, 'error');
  assert.match(reply.message, pattern);
}

test('requires the host key and explicit host consent, then forwards only paired signaling', { timeout: 5000 }, async t => {
  const open = await listen(t);
  const [host, viewer, outsider, otherHost, otherViewer] = await Promise.all(Array.from({ length: 5 }, open));
  host.send({ type: 'host', key: 'wrong-key' });
  await assertError(host, /host access key/);
  const invite = await join(host, viewer);
  const otherInvite = await join(otherHost, otherViewer);
  assert.notEqual(invite, otherInvite);
  outsider.send({ type: 'join', invite });
  await assertError(outsider, /already used/);

  const data = { description: { type: 'offer', sdp: 'test offer' } };
  viewer.send({ type: 'signal', data });
  await assertError(viewer, /must accept/);
  viewer.send({ type: 'accept', relayOnly: false });
  await assertError(viewer, /Only the host/);
  host.send({ type: 'accept', relayOnly: true });
  await assertError(host, /requires a configured TURN server/);

  host.send({ type: 'accept', relayOnly: false });
  otherHost.send({ type: 'accept', relayOnly: false });
  for (const peer of [host, otherHost]) {
    assert.deepEqual(await peer.receive(), { type: 'ready', role: 'host', iceServers: [], relayOnly: false });
  }
  for (const peer of [viewer, otherViewer]) {
    assert.deepEqual(await peer.receive(), { type: 'ready', role: 'viewer', iceServers: [], relayOnly: false });
  }
  host.send({ type: 'signal', data });
  const answer = { description: { type: 'answer', sdp: 'other answer' } };
  otherViewer.send({ type: 'signal', data: answer });
  assert.deepEqual(await viewer.receive(), { type: 'signal', data });
  assert.deepEqual(await otherHost.receive(), { type: 'signal', data: answer });
  outsider.send({ type: 'signal', data });
  await assertError(outsider, /must accept/);

  viewer.send({ type: 'end' });
  const ended = { type: 'ended', reason: 'The viewer ended the session.' };
  assert.deepEqual(await host.receive(), ended);
  assert.deepEqual(await viewer.receive(), ended);
  viewer.send({ type: 'signal', data });
  await assertError(viewer, /must accept/);
  outsider.send({ type: 'join', invite });
  await assertError(outsider, /already used/);
  otherHost.send({ type: 'signal', data });
  assert.deepEqual(await otherViewer.receive(), { type: 'signal', data });
});

test('releases invitation capacity and consumes the invite even when the host declines', { timeout: 5000 }, async t => {
  const open = await listen(t, { maxRooms: 1 });
  const [host, viewer, otherHost] = await Promise.all([open(), open(), open()]);
  const invite = await join(host, viewer);
  otherHost.send({ type: 'host', key: HOST_KEY });
  await assertError(otherHost, /at capacity/);

  host.send({ type: 'reject' });
  const ended = { type: 'ended', reason: 'The host declined the request.' };
  assert.deepEqual(await host.receive(), ended);
  assert.deepEqual(await viewer.receive(), ended);
  viewer.send({ type: 'join', invite });
  await assertError(viewer, /already used/);
  assert.notEqual(await createInvitation(otherHost), invite);
});

test('reports actionable session failures to both peers using bounded reason codes', { timeout: 5000 }, async t => {
  const open = await listen(t);
  const [host, viewer] = await Promise.all([open(), open()]);
  for (const [peer, type, reason, expected] of [
    [host, 'reject', 'audio-unavailable', /Share system audio/],
    [host, 'reject', 'capture-denied', /choose the entire/],
    [host, 'end', 'input-unavailable', /Reopen the host launcher/],
    [viewer, 'end', 'transport-failed', /TURN service/],
    [viewer, 'end', 'arbitrary text', /^The viewer ended the session\.$/],
  ]) {
    await join(host, viewer);
    if (type === 'end') {
      host.send({ type: 'accept', relayOnly: false });
      await Promise.all([host.receive(), viewer.receive()]);
    }
    peer.send({ type, reason });
    const result = await Promise.all([host.receive(), viewer.receive()]);
    assert.deepEqual(result[0], result[1]);
    assert.equal(result[0].type, 'ended');
    assert.match(result[0].reason, expected);
  }
});

test('expires unused invitations and unanswered requests', { timeout: 5000 }, async t => {
  const open = await listen(t, { inviteTtlMs: 400, consentTtlMs: 200 });
  const [host, viewer] = await Promise.all([open(), open()]);
  const expired = await createInvitation(host);
  assert.deepEqual(await host.receive(), { type: 'ended', reason: 'The invitation expired.' });
  viewer.send({ type: 'join', invite: expired });
  await assertError(viewer, /expired/);

  const invite = await join(host, viewer);
  const ended = { type: 'ended', reason: 'The request expired while waiting for the host.' };
  assert.deepEqual(await host.receive(), ended);
  assert.deepEqual(await viewer.receive(), ended);
  host.send({ type: 'accept', relayOnly: false });
  await assertError(host, /pending request/);
  viewer.send({ type: 'join', invite });
  await assertError(viewer, /already used/);
});

test('issues distinct time-limited coturn credentials to the approved pair and expires the session', { timeout: 5000 }, async t => {
  const open = await listen(t, { turnSecret: TURN_SECRET, turnUrls: TURN_URLS, stunUrls: STUN_URLS, sessionTtlMs: 250 });
  const [host, viewer] = await Promise.all([open(), open()]);
  await join(host, viewer);
  const before = Date.now();
  host.send({ type: 'accept', relayOnly: true });
  const ready = await Promise.all([host.receive(), viewer.receive()]);
  const after = Date.now();
  const usernames = ready.map(({ type, iceServers, relayOnly }) => {
    assert.equal(type, 'ready');
    assert.equal(relayOnly, true);
    assert.equal(iceServers.length, 2);
    assert.deepEqual(iceServers[0], { urls: STUN_URLS });
    const { urls, username, credential } = iceServers[1];
    assert.deepEqual(urls, TURN_URLS);
    assert.match(username, /^\d+:[a-f\d]{32}$/);
    const expiry = Number(username.split(':')[0]);
    assert.ok(expiry >= Math.ceil((before + 60_250) / 1000));
    assert.ok(expiry <= Math.ceil((after + 60_250) / 1000));
    assert.equal(credential, createHmac('sha1', TURN_SECRET).update(username).digest('base64'));
    return username;
  });
  assert.deepEqual(ready.map(({ role }) => role), ['host', 'viewer']);
  assert.notEqual(...usernames);
  for (const peer of [host, viewer]) {
    assert.deepEqual(await peer.receive(), { type: 'ended', reason: 'The session expired.' });
  }
});

test('disconnecting a peer invalidates pending and approved sessions', { timeout: 5000 }, async t => {
  const open = await listen(t);
  const [host, viewer] = await Promise.all([open(), open()]);
  const invite = await join(host, viewer);
  viewer.socket.terminate();
  assert.deepEqual(await host.receive(), { type: 'ended', reason: 'The viewer disconnected.' });

  const nextViewer = await open();
  nextViewer.send({ type: 'join', invite });
  await assertError(nextViewer, /already used/);
  await join(host, nextViewer);
  host.send({ type: 'accept', relayOnly: false });
  assert.equal((await host.receive()).type, 'ready');
  assert.equal((await nextViewer.receive()).type, 'ready');
  host.socket.terminate();
  assert.deepEqual(await nextViewer.receive(), { type: 'ended', reason: 'The host disconnected.' });
});

test('rejects oversized frames and rate-limits bursts', { timeout: 5000 }, async t => {
  const open = await listen(t);
  const [oversized, flood] = await Promise.all([open(), open()]);
  const oversizedClose = once(oversized.socket, 'close');
  oversized.socket.send('x'.repeat(64 * 1024 + 1));
  assert.equal((await oversizedClose)[0], 1009);
  const floodClose = once(flood.socket, 'close');
  for (let i = 0; i < 256; i++) flood.send({ type: 'join', invite: 'invalid' });
  assert.equal((await floodClose)[0], 1008);
});

test('rejects insecure public binds and incomplete production TURN configuration', () => {
  const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('EVP_')));
  const main = fileURLToPath(new URL('../server/main.mjs', import.meta.url));
  const cwd = new URL('..', import.meta.url);
  const cases = [
    [{ EVP_SIGNAL_HOST: '0.0.0.0' }, /requires TLS/],
    [{ EVP_SIGNAL_HOST: '0.0.0.0', EVP_SIGNAL_TRUST_PROXY: '1' }, /requires a loopback bind/],
    [{ EVP_SIGNAL_TRUST_PROXY: '1' }, /requires EVP_TURN_URLS and EVP_TURN_SECRET/],
    [{ EVP_SIGNAL_TRUST_PROXY: '1', EVP_TURN_URLS: TURN_URLS.join(',') }, /TURN shared secret is required/],
  ];
  for (const [overrides, expected] of cases) {
    const result = spawnSync(process.execPath, [main], {
      cwd,
      env: { ...env, NODE_ENV: 'production', EVP_HOST_KEY: HOST_KEY, ...overrides },
      encoding: 'utf8',
      timeout: 3000,
    });
    assert.equal(result.status, 1, result.error?.message);
    assert.match(result.stderr, expected);
  }
});
