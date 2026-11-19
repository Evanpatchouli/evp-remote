import assert from "node:assert/strict";
import test from "node:test";
import { bindViewerInput, displayPoint, parseInput, wheelDelta } from "../web/input.mjs";
import { captureStatus, createPeer, displayCaptureOptions, expectsIncomingAudio, preferCodecs, selectedRoute,
  watchConnection, watchTrackMute } from "../web/peer.mjs";

test("a video-only host does not require an incoming audio track", () => {
  const track = (kind, currentDirection) => ({ receiver: { track: { kind } }, currentDirection });
  const connection = (...tracks) => ({ getTransceivers: () => tracks });
  assert.equal(expectsIncomingAudio(connection(track("video", "recvonly"))), false);
  assert.equal(expectsIncomingAudio(connection(track("audio", "inactive"))), false);
  assert.equal(expectsIncomingAudio(connection(track("audio", "recvonly"))), true);
});

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

test("screen sharing defaults to 1080p60 and high refresh targets native size without hard requirements", () => {
  const display = { width: 2560, height: 1440 };
  const baseline = displayCaptureOptions(false, display, 1.5);
  assert.deepEqual(baseline, {
    video: { displaySurface: "monitor", width: { ideal: 1920 }, height: { ideal: 1080 }, frameRate: { ideal: 60 } },
    audio: true, systemAudio: "include", monitorTypeSurfaces: "include", surfaceSwitching: "exclude",
  });
  const highRefresh = displayCaptureOptions(true, display, 1.5);
  assert.deepEqual(highRefresh, {
    ...baseline,
    video: {
      displaySurface: "monitor", width: { ideal: 3840 }, height: { ideal: 2160 },
      resizeMode: { ideal: "none" }, frameRate: { ideal: 144 },
    },
  });
  assert.deepEqual(displayCaptureOptions(true).video, {
    displaySurface: "monitor", resizeMode: { ideal: "none" }, frameRate: { ideal: 144 },
  });
});

test("host diagnostics show track settings and qualified downgrade reasons", () => {
  const display = { width: 2560, height: 1440 };
  const fullSize = { width: 3840, height: 2160, frameRate: 144, resizeMode: "none" };
  assert.equal(captureStatus(fullSize, true, display, 1.5), "采集画面 3840 × 2160，帧率设置为 144 帧/秒。");
  const reduced = { width: 1920, height: 1080, frameRate: 120, screenPixelRatio: 1.5 };
  assert.match(captureStatus(reduced, true, display, 1, true),
    /1920 × 1080，帧率设置为 120 帧\/秒.*低于 144 帧\/秒.*低于估算的屏幕分辨率.*无法确认浏览器是否应用了请求的编码设置/);
  assert.match(captureStatus({}, true, display, 1), /无法确认是否达到 144 帧\/秒.*无法确认是否按屏幕原生分辨率采集/);
  assert.equal(captureStatus(reduced, false, display, 1), "采集画面 1920 × 1080，帧率设置为 120 帧/秒。");
});

test("the host requests full-size high-refresh encoding only for the opted-in video sender", async t => {
  const requested = [];
  let rejectEncoderSettings = false;
  class MockPeer extends EventTarget {
    addTransceiver(track) {
      let parameters = { transactionId: "current", encodings: [{ active: true }] };
      return { sender: {
        getParameters() { return structuredClone(parameters); },
        async setParameters(value) {
          requested.push([track.kind, value]);
          if (rejectEncoderSettings) throw new DOMException("Unavailable", "NotSupportedError");
          parameters = value;
        },
      } };
    }
    createDataChannel(label, options) {
      assert.equal(label, "input");
      assert.deepEqual(options, { ordered: true });
      return {};
    }
    async createOffer() { return { type: "offer" }; }
    async setLocalDescription(description) { this.localDescription = { toJSON: () => description }; }
    close() {}
  }
  globalThis.RTCPeerConnection = MockPeer;
  t.after(() => { delete globalThis.RTCPeerConnection; });
  let fallbacks = 0;
  async function makeOffer(highRefresh) {
    let peer;
    const offer = await new Promise((resolve, reject) => {
      peer = createPeer({
        role: "host", highRefresh, iceServers: [],
        stream: { getTracks: () => [{ kind: "video" }, { kind: "audio" }] },
        sendSignal: resolve, onError: reject,
        onChannel() {}, onTrack() {}, onConnectionState() {}, onConnectionLost() {},
        onEncoderFallback() { fallbacks++; },
      });
    });
    peer.close();
    assert.deepEqual(offer, { description: { type: "offer" } });
  }
  await makeOffer(false);
  assert.deepEqual(requested, []);
  await makeOffer(true);
  assert.deepEqual(requested, [["video", {
    transactionId: "current", encodings: [{ active: true, scaleResolutionDownBy: 1, maxFramerate: 144 }],
    degradationPreference: "maintain-resolution",
  }]]);
  assert.equal(fallbacks, 0);
  rejectEncoderSettings = true;
  await makeOffer(true);
  assert.equal(fallbacks, 1);
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
