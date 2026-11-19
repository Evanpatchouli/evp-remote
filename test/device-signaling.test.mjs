import assert from 'node:assert/strict';
import { createHash, createHmac, generateKeyPairSync, randomBytes, sign } from 'node:crypto';
import { on, once } from 'node:events';
import { test } from 'node:test';
import { WebSocket } from 'ws';
import { createSignalServer } from '../server/signaling.mjs';

const HOST_KEY = 'test-host-key';
const TURN_SECRET = 'test-turn-secret';
const TURN_URLS = ['turn:relay.example:3478'];
const STUN_URLS = ['stun:relay.example:3478'];
const PAYLOAD_LIMIT = 56 * 1024;

const token = () => randomBytes(32).toString('base64url');
const encodeJson = value => Buffer.from(JSON.stringify(value)).toString('base64url');

async function listen(context, options = {}) {
  const server = createSignalServer({ hostKey: HOST_KEY, ...options });
  context.after(() => new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve())));
  const listening = once(server, 'listening');
  server.listen(0, '127.0.0.1');
  await listening;
  return async () => {
    const socket = new WebSocket(`ws://127.0.0.1:${server.address().port}/`, { maxPayload: 64 * 1024 });
    const stream = on(socket, 'message', { close: ['close'] });
    await once(socket, 'open');
    return {
      socket,
      send: message => socket.send(JSON.stringify(message)),
      receive: async () => {
        const { done, value } = await stream.next();
        assert.equal(done, false, 'socket closed before the next message');
        return JSON.parse(value[0].toString());
      },
    };
  };
}

function newIdentity() {
  const { publicKey, privateKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const uncompressed = publicKey.export({ format: 'der', type: 'spki' }).subarray(-65);
  assert.equal(uncompressed[0], 4);
  const keyBytes = uncompressed.subarray(1);
  return {
    privateKey,
    publicKey: keyBytes.toString('base64url'),
    id: createHash('sha256').update(keyBytes).digest('base64url'),
  };
}

function signFields({ privateKey }, ...fields) {
  const bytes = Buffer.from(`evp-device-v1\n${fields.join('\n')}`);
  return sign('sha256', bytes, { key: privateKey, dsaEncoding: 'ieee-p1363' }).toString('base64url');
}

function registration(identity, nonce) {
  const { publicKey } = identity;
  return { type: 'device-register', publicKey, signature: signFields(identity, 'register', nonce, publicKey) };
}

async function hello(peer) {
  peer.send({ type: 'device-hello', key: HOST_KEY });
  const { type, nonce } = await peer.receive();
  assert.equal(type, 'device-challenge');
  const bytes = Buffer.from(nonce, 'base64url');
  assert.equal(bytes.length, 32);
  assert.equal(bytes.toString('base64url'), nonce);
  return nonce;
}

async function register(peer, identity = newIdentity()) {
  peer.send(registration(identity, await hello(peer)));
  assert.deepEqual(await peer.receive(), { type: 'device-registered', id: identity.id });
  peer.identity = identity;
  return peer;
}

async function expectError(peer, code, attempt) {
  assert.deepEqual(await peer.receive(), {
    type: 'device-error', code, ...(attempt === undefined ? {} : { attempt }),
  });
}

async function expectPresence(peer, ids, online) {
  peer.send({ type: 'device-presence', ids });
  assert.deepEqual(await peer.receive(), { type: 'device-presence', online });
}

function startMessage(viewer, host, mode, attempt = token()) {
  const { id: from } = viewer.identity;
  const { id: target } = host.identity;
  const payload = encodeJson(mode === 'pair' ? {
    mac: createHmac('sha256', randomBytes(32)).update(`evp-device-v1\npair-code\n${from}\n${target}\n${attempt}`).digest('base64url'),
    name: 'Viewer',
  } : {});
  return {
    type: 'device-start', mode, target, attempt, payload,
    signature: signFields(viewer.identity, 'start', mode, from, target, attempt, payload),
  };
}

async function start(viewer, host, mode = 'control') {
  const message = startMessage(viewer, host, mode);
  viewer.send(message);
  assert.deepEqual(await host.receive(), {
    type: 'device-request', mode, from: viewer.identity.id, publicKey: viewer.identity.publicKey,
    attempt: message.attempt, payload: message.payload, signature: message.signature,
  });
  return { viewer, host, mode, attempt: message.attempt, nonce: token() };
}

function frameMessage(session, role, stage, seq, payload = encodeJson({})) {
  const { attempt, nonce, host, viewer, mode } = session;
  return {
    type: 'device-frame', attempt, stage, seq, payload,
    signature: signFields(session[role].identity, 'frame', mode, viewer.identity.id, host.identity.id,
      attempt, nonce, role, stage, String(seq), payload),
  };
}

async function exchangeFrame(session, role, stage, seq, payload) {
  const message = frameMessage(session, role, stage, seq, payload);
  session[role].send(message);
  assert.deepEqual(await session[role === 'host' ? 'viewer' : 'host'].receive(), message);
}

async function challengeAttempt(session, relayOnly = false) {
  const { nonce, host, mode } = session;
  const body = mode === 'pair' ? { nonce, publicKey: host.identity.publicKey, name: 'Host' } : { nonce, relayOnly };
  await exchangeFrame(session, 'host', 'challenge', 1, encodeJson(body));
}

async function confirmAttempt(session, relayOnly = false) {
  await challengeAttempt(session, relayOnly);
  await exchangeFrame(session, 'viewer', 'confirm', 1);
}

async function expectEnded(session, reason) {
  for (const peer of [session.host, session.viewer]) {
    assert.deepEqual(await peer.receive(), { type: 'device-ended', attempt: session.attempt, reason });
  }
}

test('requires the deployment key and a fresh socket-local P-256 registration proof', { timeout: 5000 }, async context => {
  const open = await listen(context);
  const [first, second] = await Promise.all([open(), open()]);
  const identity = newIdentity();
  first.send(registration(identity, token()));
  await expectError(first, 'auth');
  first.send({ type: 'device-hello', key: 'incorrect' });
  await expectError(first, 'auth');
  first.send({ type: 'device-presence', ids: [] });
  await expectError(first, 'auth');

  const [firstNonce, secondNonce] = await Promise.all([hello(first), hello(second)]);
  assert.notEqual(firstNonce, secondNonce);
  const firstProof = registration(identity, firstNonce);
  second.send(firstProof);
  await expectError(second, 'auth');
  second.send(registration(identity, secondNonce));
  await expectError(second, 'auth');

  const freshNonce = await hello(first);
  assert.notEqual(firstNonce, freshNonce);
  first.send(firstProof);
  await expectError(first, 'auth');
  first.send(registration(identity, freshNonce));
  await expectError(first, 'auth');

  const paddedProof = registration(identity, await hello(first));
  first.send({ ...paddedProof, signature: `${paddedProof.signature}==` });
  await expectError(first, 'protocol');
  first.send(paddedProof);
  await expectError(first, 'auth');

  const wrongSignerNonce = await hello(first);
  first.send({
    type: 'device-register', publicKey: identity.publicKey,
    signature: signFields(newIdentity(), 'register', wrongSignerNonce, identity.publicKey),
  });
  await expectError(first, 'auth');
  await register(first, identity);
  await expectPresence(first, [identity.id], [identity.id]);
});

test('filters bounded presence queries and does not let duplicate identities evict a live socket', { timeout: 5000 }, async context => {
  const open = await listen(context);
  const [original, duplicate, observer] = await Promise.all([open(), open(), open()]);
  await Promise.all([register(original), register(observer)]);
  duplicate.send(registration(original.identity, await hello(duplicate)));
  await expectError(duplicate, 'duplicate');
  duplicate.send({ type: 'device-presence', ids: [original.identity.id] });
  await expectError(duplicate, 'auth');

  const ids = [original.identity.id, original.identity.id, ...Array.from({ length: 62 }, token)];
  await expectPresence(observer, [], []);
  await expectPresence(observer, ids, [original.identity.id]);
  observer.send({ type: 'device-presence', ids: [...ids, token()] });
  await expectError(observer, 'protocol');
  observer.send({ type: 'device-presence', ids: [`${original.identity.id}=`] });
  await expectError(observer, 'protocol');

  const session = await start(observer, original);
  await confirmAttempt(session);
  original.send({ type: 'device-accept', attempt: session.attempt, relayOnly: false });
  for (const role of ['host', 'viewer']) {
    assert.deepEqual(await session[role].receive(), {
      type: 'device-ready', attempt: session.attempt, role, relayOnly: false, iceServers: [],
    });
  }
  original.socket.terminate();
  assert.deepEqual(await observer.receive(), { type: 'device-ended', attempt: session.attempt, reason: 'disconnected' });
  await expectPresence(observer, [original.identity.id], []);
  observer.send(startMessage(observer, original, 'control', session.attempt));
  await expectError(observer, 'offline', session.attempt);

  await register(duplicate, original.identity);
  await expectPresence(observer, [original.identity.id], [original.identity.id]);
  duplicate.send({ type: 'device-accept', attempt: session.attempt, relayOnly: false });
  await expectError(duplicate, 'protocol', session.attempt);
});

test('bounds registrations below the socket limit and reclaims capacity on disconnect', { timeout: 15_000 }, async context => {
  const open = await listen(context);
  const devices = [];
  while (devices.length < 256) {
    devices.push(...await Promise.all(Array.from({ length: 32 }, async () => register(await open()))));
  }
  const [overflow, attended] = await Promise.all([open(), open()]);
  const identity = newIdentity();
  overflow.send(registration(identity, await hello(overflow)));
  await expectError(overflow, 'capacity');
  attended.send({ type: 'host', key: HOST_KEY });
  assert.equal((await attended.receive()).type, 'invitation');

  const [host, viewer] = devices;
  const session = await start(viewer, host);
  host.socket.terminate();
  assert.deepEqual(await viewer.receive(), { type: 'device-ended', attempt: session.attempt, reason: 'disconnected' });
  await register(overflow, identity);
});

test('scopes pairing stages, roles, sequences and attempt ownership without issuing ICE', { timeout: 5000 }, async context => {
  const open = await listen(context, { turnSecret: TURN_SECRET, turnUrls: TURN_URLS });
  const [host, viewer, otherHost, outsider] = await Promise.all(Array.from({ length: 4 }, async () => register(await open())));
  const session = await start(viewer, host, 'pair');
  const { attempt } = session;
  outsider.send(startMessage(outsider, otherHost, 'pair', attempt));
  await expectError(outsider, 'duplicate', attempt);
  const busyHost = startMessage(outsider, host, 'pair');
  outsider.send(busyHost);
  await expectError(outsider, 'busy', busyHost.attempt);
  const busyViewer = startMessage(viewer, otherHost, 'control');
  viewer.send(busyViewer);
  await expectError(viewer, 'busy', busyViewer.attempt);
  outsider.send({ type: 'device-end', attempt });
  await expectError(outsider, 'protocol', attempt);
  outsider.send(frameMessage(session, 'host', 'challenge', 1));
  await expectError(outsider, 'protocol', attempt);

  for (const [role, stage, seq] of [['viewer', 'challenge', 1], ['viewer', 'confirm', 1], ['host', 'challenge', 2]]) {
    session[role].send(frameMessage(session, role, stage, seq));
    await expectError(session[role], 'protocol', attempt);
  }
  await challengeAttempt(session);
  host.send(frameMessage(session, 'host', 'paired', 2));
  await expectError(host, 'protocol', attempt);
  await exchangeFrame(session, 'viewer', 'confirm', 1);
  host.send({ type: 'device-accept', attempt, relayOnly: false });
  await expectError(host, 'protocol', attempt);
  await exchangeFrame(session, 'host', 'paired', 2);
  host.send(frameMessage(session, 'host', 'signal', 3));
  await expectError(host, 'protocol', attempt);
  viewer.send({ type: 'device-end', attempt, reason: 'unbounded text' });
  await expectError(viewer, 'protocol', attempt);
  viewer.send({ type: 'device-end', attempt });
  await expectEnded(session, 'ended');
  host.send({ type: 'device-end', attempt: 'not a wire ID' });
  await expectError(host, 'protocol');
});

test('issues ICE only on control host acceptance and preserves bounded signed payload bytes', { timeout: 5000 }, async context => {
  const open = await listen(context, { turnSecret: TURN_SECRET, turnUrls: TURN_URLS, stunUrls: STUN_URLS });
  const [host, viewer] = await Promise.all(Array.from({ length: 2 }, async () => register(await open())));
  const session = await start(viewer, host);
  const { attempt } = session;
  const accept = { type: 'device-accept', attempt, relayOnly: true };
  host.send(accept);
  await expectError(host, 'protocol', attempt);
  await confirmAttempt(session, true);
  viewer.send(accept);
  await expectError(viewer, 'protocol', attempt);
  viewer.send(frameMessage(session, 'viewer', 'signal', 2));
  await expectError(viewer, 'protocol', attempt);
  host.send({ ...accept, relayOnly: 'true' });
  await expectError(host, 'protocol', attempt);

  host.send(accept);
  const usernames = [];
  for (const role of ['host', 'viewer']) {
    const { iceServers, ...ready } = await session[role].receive();
    assert.deepEqual(ready, { type: 'device-ready', attempt, role, relayOnly: true });
    assert.equal(iceServers.length, 2);
    assert.deepEqual(iceServers[0], { urls: STUN_URLS });
    const { urls, username, credential } = iceServers[1];
    assert.deepEqual(urls, TURN_URLS);
    assert.match(username, /^\d+:[a-f\d]{32}$/);
    assert.equal(credential, createHmac('sha1', TURN_SECRET).update(username).digest('base64'));
    usernames.push(username);
  }
  assert.notEqual(...usernames);
  host.send(accept);
  await expectError(host, 'protocol', attempt);

  const prefix = ' { "description" : { "type": "offer", "sdp": "';
  const suffix = '" } }\n';
  const sdpLength = PAYLOAD_LIMIT * 3 / 4 - Buffer.byteLength(prefix + suffix);
  const payload = Buffer.from(`${prefix}${'x'.repeat(sdpLength)}${suffix}`).toString('base64url');
  assert.equal(payload.length, PAYLOAD_LIMIT);
  const invalidPayloads = [
    Buffer.from(`${prefix}${'x'.repeat(sdpLength + 1)}${suffix}`).toString('base64url'),
    `${encodeJson({})}=`, encodeJson([]), Buffer.from('{').toString('base64url'),
    Buffer.from([0x7b, 0x22, 0xc0, 0x22, 0x3a, 0x30, 0x7d]).toString('base64url'),
  ];
  for (const invalid of invalidPayloads) {
    viewer.send(frameMessage(session, 'viewer', 'signal', 2, invalid));
    await expectError(viewer, 'protocol', attempt);
  }
  viewer.send(frameMessage(session, 'viewer', 'signal', 3));
  await expectError(viewer, 'protocol', attempt);
  await exchangeFrame(session, 'viewer', 'signal', 2, payload);
  viewer.send(frameMessage(session, 'viewer', 'signal', 2, payload));
  await expectError(viewer, 'protocol', attempt);
  await exchangeFrame(session, 'host', 'signal', 2, encodeJson({ description: { type: 'answer', sdp: 'answer' } }));
  await exchangeFrame(session, 'viewer', 'signal', 3, encodeJson({ candidate: { candidate: 'ice' } }));
  host.send({ type: 'device-end', attempt });
  await expectEnded(session, 'ended');
});

test('refuses relay-only control without TURN and expires pending and accepted attempts', { timeout: 5000 }, async context => {
  const open = await listen(context, { maxRooms: 1, consentTtlMs: 600, sessionTtlMs: 300 });
  const [host, viewer] = await Promise.all(Array.from({ length: 2 }, async () => register(await open())));
  const pending = await start(viewer, host);
  await confirmAttempt(pending, true);
  host.send({ type: 'device-accept', attempt: pending.attempt, relayOnly: true });
  await expectError(host, 'relay', pending.attempt);
  await expectEnded(pending, 'timeout');
  host.send({ type: 'device-accept', attempt: pending.attempt, relayOnly: false });
  await expectError(host, 'protocol', pending.attempt);

  const active = await start(viewer, host);
  await confirmAttempt(active);
  host.send({ type: 'device-accept', attempt: active.attempt, relayOnly: false });
  for (const role of ['host', 'viewer']) {
    assert.deepEqual(await active[role].receive(), {
      type: 'device-ready', attempt: active.attempt, role, relayOnly: false, iceServers: [],
    });
  }
  host.send(frameMessage(pending, 'host', 'signal', 2));
  await expectError(host, 'protocol', pending.attempt);
  await expectEnded(active, 'timeout');
  await expectPresence(viewer, [host.identity.id], [host.identity.id]);
});

test('separates device and attended traffic while sharing the room cap', { timeout: 5000 }, async context => {
  const open = await listen(context, { maxRooms: 1 });
  const [host, viewer, attendedHost, attendedViewer] = await Promise.all([open(), open(), open(), open()]);
  await Promise.all([register(host), register(viewer)]);
  attendedHost.send({ type: 'host', key: HOST_KEY });
  const { type, invite } = await attendedHost.receive();
  assert.equal(type, 'invitation');
  attendedHost.send({ type: 'device-hello', key: HOST_KEY });
  await expectError(attendedHost, 'protocol');
  viewer.send({ type: 'join', invite });
  await expectError(viewer, 'protocol');
  const atCapacity = startMessage(viewer, host, 'pair');
  viewer.send(atCapacity);
  await expectError(viewer, 'capacity', atCapacity.attempt);

  attendedViewer.send({ type: 'join', invite });
  assert.deepEqual(await attendedHost.receive(), { type: 'request' });
  assert.deepEqual(await attendedViewer.receive(), { type: 'waiting' });
  attendedHost.send({ type: 'accept', relayOnly: false });
  assert.deepEqual(await attendedHost.receive(), { type: 'ready', role: 'host', relayOnly: false, iceServers: [] });
  assert.deepEqual(await attendedViewer.receive(), { type: 'ready', role: 'viewer', relayOnly: false, iceServers: [] });
  const signal = { type: 'signal', data: { description: { type: 'offer', sdp: 'attended offer' } } };
  attendedViewer.send(signal);
  assert.deepEqual(await attendedHost.receive(), signal);
  attendedViewer.send({ type: 'end' });
  for (const peer of [attendedHost, attendedViewer]) {
    assert.deepEqual(await peer.receive(), { type: 'ended', reason: 'The viewer ended the session.' });
  }

  const session = await start(viewer, host, 'pair');
  host.send({ type: 'host', key: HOST_KEY });
  await expectError(host, 'protocol');
  attendedHost.send({ type: 'host', key: HOST_KEY });
  const full = await attendedHost.receive();
  assert.equal(full.type, 'error');
  assert.match(full.message, /at capacity/);
  host.send({ type: 'device-end', attempt: session.attempt, reason: 'declined' });
  await expectEnded(session, 'declined');
  attendedHost.send({ type: 'host', key: HOST_KEY });
  assert.equal((await attendedHost.receive()).type, 'invitation');
});
