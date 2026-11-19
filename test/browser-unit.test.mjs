import assert from "node:assert/strict";
import test from "node:test";
import { bindViewerInput, displayPoint, parseInput, wheelDelta } from "../web/input.mjs";
import { createPeer, preferCodecs, selectedRoute, watchConnection, watchTrackMute } from "../web/peer.mjs";

test("H.264 and Opus are preferred while the full browser codec list is retained", () => {
  const vp8 = { mimeType: "video/VP8" };
  const rtx = { mimeType: "video/rtx" };
  const main = { mimeType: "video/H264", sdpFmtpLine: "packetization-mode=1;profile-level-id=4d001f" };
  const baseline = { mimeType: "video/H264", sdpFmtpLine: "packetization-mode=1;profile-level-id=42e01f" };
  const video = [vp8, rtx, main, baseline];
  assert.deepEqual(preferCodecs(video), [baseline, main, vp8, rtx]);
  assert.deepEqual(video, [vp8, rtx, main, baseline]);
  assert.deepEqual(preferCodecs([vp8, rtx]), [vp8, rtx]);
  const pcm = { mimeType: "audio/PCMU" };
  const opus = { mimeType: "audio/opus" };
  const red = { mimeType: "audio/red" };
  assert.deepEqual(preferCodecs([pcm, opus, red]), [opus, pcm, red]);
});

test("display coordinates account for horizontal and vertical letterboxing", () => {
  const rect = { left: 10, top: 20, width: 400, height: 200 };
  assert.deepEqual(displayPoint(rect, 1920, 960, 210, 120), { x: 0.5, y: 0.5 });
  assert.equal(displayPoint(rect, 100, 200, 35, 120), null);
  assert.deepEqual(displayPoint(rect, 100, 200, 160, 20), { x: 0, y: 0 });
  assert.deepEqual(displayPoint(rect, 100, 200, 260, 220), { x: 1, y: 1 });
  assert.equal(displayPoint(rect, 400, 100, 210, 55), null);
  assert.deepEqual(displayPoint(rect, 400, 100, 410, 170), { x: 1, y: 1 });
  assert.equal(displayPoint(rect, 0, 0, 210, 120), null);
});

test("the host accepts only input and drops events from an earlier permit", () => {
  const parse = event => parseInput(JSON.stringify(event), 2);
  assert.deepEqual(parse({ type: "move", x: 0.5, y: 1, epoch: 2, token: "unused" }), { type: "move", x: 0.5, y: 1 });
  assert.deepEqual(parse({ type: "release", epoch: 2 }), { type: "release" });
  assert.deepEqual(parse({ type: "key", code: "NumpadEnter", down: true, epoch: 2 }), { type: "key", code: "NumpadEnter", down: true });
  assert.equal(parse({ type: "key", code: "KeyA", down: true, epoch: 1 }), null);
  assert.equal(parse({ type: "button", button: 3, down: true, epoch: 2 }), null);
  assert.equal(parse({ type: "key", code: "VolumeUp", down: true, epoch: 2 }), null);
  assert.equal(parse({ type: "key", code: "KeyA", down: 1, epoch: 2 }), null);
  assert.equal(parse({ type: "move", x: -0.01, y: 1, epoch: 2 }), null);
  assert.equal(parse({ type: "wheel", delta: 32768, epoch: 2 }), null);
  assert.equal(parse({ type: "arm", token: "unused", epoch: 2 }), null);
  assert.equal(parse({ type: "ping", epoch: 2 }), null);
  assert.equal(parseInput("-"), null);
});

test("wheel direction and range match the Windows input adapter", () => {
  assert.equal(wheelDelta(-120, 0), 120);
  assert.equal(wheelDelta(1, 1), -120);
  assert.equal(wheelDelta(-1, 2), 360);
  assert.equal(wheelDelta(1_000, 1), -32768);
});

test("pointer release outside the video and keyboard blur release held controls", t => {
  const browserWindow = new EventTarget();
  const browserDocument = new EventTarget();
  const globals = { window: browserWindow, document: browserDocument, cancelAnimationFrame() {} };
  for (const [name, value] of Object.entries(globals)) globalThis[name] = value;
  t.after(() => {
    for (const name of Object.keys(globals)) delete globalThis[name];
  });
  const video = Object.assign(new EventTarget(), {
    videoWidth: 100,
    videoHeight: 100,
    focus() {},
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 200, height: 100 }),
  });
  const events = [];
  bindViewerInput(video, () => true, event => Boolean(events.push(event)));
  function fire(target, type, props = {}) {
    target.dispatchEvent(Object.assign(new Event(type, { cancelable: true }), props));
  }
  fire(video, "mousedown", { button: 0, clientX: 15, clientY: 40 });
  assert.deepEqual(events, []);
  fire(video, "mousedown", { button: 0, clientX: 100, clientY: 50 });
  fire(browserWindow, "mouseup", { button: 0, clientX: 220, clientY: 50 });
  fire(video, "keydown", { code: "KeyA" });
  fire(video, "keydown", { code: "KeyA", repeat: true });
  fire(video, "blur");
  fire(video, "keyup", { code: "KeyA" });
  assert.deepEqual(events, [
    { type: "move", x: 0.5, y: 0.5 },
    { type: "button", button: 0, down: true },
    { type: "button", button: 0, down: false },
    { type: "key", code: "KeyA", down: true },
    { type: "key", code: "KeyA", down: true },
    { type: "release" },
  ]);
});

test("the selected transport pair determines the route", () => {
  const stats = new Map([
    ["transport", { type: "transport", selectedCandidatePairId: "selected" }],
    ["selected", { type: "candidate-pair", localCandidateId: "local", remoteCandidateId: "remote" }],
    ["other", { type: "candidate-pair", nominated: true, state: "succeeded", localCandidateId: "relay", remoteCandidateId: "remote" }],
    ["local", { candidateType: "host" }],
    ["remote", { candidateType: "srflx" }],
    ["relay", { candidateType: "relay" }],
  ]);
  assert.deepEqual(selectedRoute(stats), { kind: "direct", local: "host", remote: "srflx" });
  stats.get("transport").selectedCandidatePairId = "other";
  assert.deepEqual(selectedRoute(stats), { kind: "relay", local: "relay", remote: "srflx" });
  stats.get("other").localCandidateId = "remote";
  stats.get("other").remoteCandidateId = "relay";
  assert.deepEqual(selectedRoute(stats), { kind: "relay", local: "srflx", remote: "relay" });
  stats.get("transport").selectedCandidatePairId = undefined;
  assert.equal(selectedRoute(stats), null);
});

test("the connection can recover before the deadline and fails after a sustained drop", t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const connection = new EventTarget();
  const events = [];
  const stop = watchConnection(connection, state => events.push(state), () => events.push("lost"));
  function change(state) {
    connection.connectionState = state;
    connection.dispatchEvent(new Event("connectionstatechange"));
  }
  change("disconnected");
  t.mock.timers.tick(7_999);
  change("connected");
  t.mock.timers.tick(8_000);
  assert.deepEqual(events, ["disconnected", "connected"]);
  change("disconnected");
  change("connecting");
  t.mock.timers.tick(8_000);
  assert.deepEqual(events, ["disconnected", "connected", "disconnected", "connecting", "lost"]);
  stop();
});

test("an ICE failure ends the recovery window immediately", t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const connection = new EventTarget();
  const events = [];
  const stop = watchConnection(connection, state => events.push(state), () => events.push("lost"));
  connection.connectionState = "disconnected";
  connection.dispatchEvent(new Event("connectionstatechange"));
  connection.connectionState = "failed";
  connection.dispatchEvent(new Event("connectionstatechange"));
  t.mock.timers.tick(8_000);
  assert.deepEqual(events, ["disconnected", "lost"]);
  stop();
});

test("a live audio track may briefly mute but a sustained mute ends the session", t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const track = Object.assign(new EventTarget(), { muted: true, readyState: "live" });
  const events = [];
  const stop = watchTrackMute(track, muted => events.push(muted), () => events.push("unavailable"));
  t.mock.timers.tick(7_999);
  track.muted = false;
  track.dispatchEvent(new Event("unmute"));
  t.mock.timers.tick(30_000);
  assert.deepEqual(events, [true, false]);
  track.muted = true;
  track.dispatchEvent(new Event("mute"));
  t.mock.timers.tick(8_000);
  assert.deepEqual(events, [true, false, true, "unavailable"]);
  stop();
});

test("ICE candidates received before the offer are applied after the remote description", async t => {
  const seen = [];
  class MockPeer extends EventTarget {
    constructor(options) {
      super();
      assert.equal(options.iceTransportPolicy, "relay");
      this.remoteDescription = null;
    }
    async setRemoteDescription(description) {
      this.remoteDescription = description;
      seen.push(description.type);
    }
    async addIceCandidate(candidate) { seen.push(candidate); }
    async createAnswer() { return { type: "answer" }; }
    async setLocalDescription(description) { this.localDescription = { toJSON: () => description }; }
    close() {}
  }
  globalThis.RTCPeerConnection = MockPeer;
  t.after(() => { delete globalThis.RTCPeerConnection; });
  const peer = createPeer({ role: "viewer", iceServers: [], relayOnly: true,
    sendSignal: data => seen.push(data), onChannel() {}, onTrack() {}, onConnectionState() {}, onConnectionLost() {},
  });
  const candidate = { candidate: "early", sdpMid: "0" };
  await peer.receive({ candidate });
  assert.deepEqual(seen, []);
  await peer.receive({ description: { type: "offer" } });
  assert.deepEqual(seen, ["offer", candidate, { description: { type: "answer" } }]);
  peer.close();
});
