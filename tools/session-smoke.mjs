import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createSocket } from 'node:dgram';
import { networkInterfaces } from 'node:os';
import { setTimeout as delay } from 'node:timers/promises';
import { chromium } from 'playwright-core';
import { createSignalServer } from '../server/signaling.mjs';
import { startDesktop } from '../desktop/server.mjs';

// This fixture runs in each browser. Only the host substitutes capture devices.
// The peers, codecs, ICE, signaling and application event handlers are real.
function installFixture({ host, unreachable }) {
  const diagnostics = window.smoke = { peers: [], filtered: 0 };
  function candidateForTest(line) {
    if (!unreachable || !/^(?:a=)?candidate:/i.test(line) || /\btyp relay\b/i.test(line)) return line;
    diagnostics.filtered++;
    const fields = line.split(' ');
    if (fields[2]?.toLowerCase() !== 'udp') return '';
    fields[4] = unreachable.address;
    fields[5] = String(unreachable.port);
    return fields.join(' ');
  }
  const Peer = window.RTCPeerConnection;
  window.RTCPeerConnection = class extends Peer {
    constructor(config) {
      super(config);
      diagnostics.peers.push(this);
    }
    async setRemoteDescription(description) {
      if (unreachable && description?.sdp) {
        const lines = description.sdp.split('\r\n');
        const mapped = lines.flatMap(line => {
          const candidate = candidateForTest(line);
          return candidate || !line ? [candidate] : [];
        });
        description = { type: description.type, sdp: mapped.join('\r\n') };
      }
      return super.setRemoteDescription(description);
    }
    async addIceCandidate(candidate) {
      if (unreachable && candidate?.candidate) {
        const line = candidateForTest(candidate.candidate);
        if (!line) return;
        candidate = { ...candidate, candidate: line };
      }
      return super.addIceCandidate(candidate);
    }
  };
  if (!host) return;
  navigator.mediaDevices.getDisplayMedia = async () => {
    const canvas = document.createElement('canvas');
    canvas.width = 640;
    canvas.height = 360;
    const context = canvas.getContext('2d');
    let count = 0;
    function draw() {
      count++;
      context.fillStyle = count % 2 ? '#42609c' : '#407859';
      context.fillRect(0, 0, canvas.width, canvas.height);
      context.fillStyle = 'white';
      context.font = '32px sans-serif';
      context.fillText(String(count), 16, 48);
    }
    draw();
    const stream = canvas.captureStream(15);
    const video = stream.getVideoTracks()[0];
    const settings = video.getSettings.bind(video);
    video.getSettings = () => ({ ...settings(), displaySurface: 'monitor' });
    const timer = setInterval(draw, 65);
    const audio = new AudioContext();
    await audio.resume();
    const tone = audio.createOscillator();
    const output = audio.createMediaStreamDestination();
    tone.frequency.value = 440;
    tone.connect(output);
    tone.start();
    stream.addTrack(output.stream.getAudioTracks()[0]);
    diagnostics.fixture = { stream, audio, tone, canvas, timer };
    return stream;
  };
}

async function waitFor(check, label, timeout = 30_000) {
  const end = Date.now() + timeout;
  do {
    const result = await check();
    if (result) return result;
    await delay(120);
  } while (Date.now() < end);
  throw new Error(`Timed out waiting for ${label}`);
}

async function metrics(page) {
  return page.evaluate(async () => {
    const pc = window.smoke.peers.at(-1);
    if (!pc) return {};
    const stats = await pc.getStats();
    const values = [...stats.values()];
    const transport = values.find(row => row.type === 'transport' && row.selectedCandidatePairId);
    const pair = stats.get(transport?.selectedCandidatePairId);
    const local = stats.get(pair?.localCandidateId)?.candidateType;
    const remote = stats.get(pair?.remoteCandidateId)?.candidateType;
    const audio = values.find(row => row.type === 'inbound-rtp' && row.kind === 'audio');
    const video = values.find(row => row.type === 'inbound-rtp' && row.kind === 'video');
    const shared = document.querySelector('#remote-video').srcObject;
    if (shared?.getAudioTracks().length && !window.smoke.probe) {
      const context = new AudioContext();
      const source = context.createMediaStreamSource(shared);
      const analyser = context.createAnalyser();
      analyser.fftSize = 2048;
      source.connect(analyser);
      await context.resume();
      window.smoke.probe = { context, analyser, source };
    }
    let toneHz = 0;
    if (window.smoke.probe) {
      const { context, analyser } = window.smoke.probe;
      const bins = new Float32Array(analyser.frequencyBinCount);
      analyser.getFloatFrequencyData(bins);
      const peak = Math.max(...bins);
      if (peak > -55) toneHz = bins.indexOf(peak) * context.sampleRate / analyser.fftSize;
    }
    return {
      local, remote, policy: pc.getConfiguration().iceTransportPolicy,
      filtered: window.smoke.filtered, framesDecoded: video?.framesDecoded || 0,
      videoCodec: stats.get(video?.codecId)?.mimeType,
      audioCodec: stats.get(audio?.codecId)?.mimeType,
      audioPackets: audio?.packetsReceived || 0, audioEnergy: audio?.totalAudioEnergy || 0, toneHz,
      displayed: document.querySelector('#remote-video').getVideoPlaybackQuality().totalVideoFrames,
    };
  });
}

async function runCase(mode, signalUrl, access, hostBrowser, viewerBrowser) {
  const commands = [];
  let unreachable, sink, checks = 0;
  let nativeClosed = false;
  const local = await startDesktop({ signalUrl, createInput: async () => {
    let resolveClosed;
    return {
      closed: new Promise(resolve => { resolveClosed = resolve; }),
      write(command) { commands.push(command); },
      async close() { nativeClosed = true; resolveClosed(); },
    };
  } });
  const contexts = [];
  let host, viewer;
  try {
    if (mode === 'fallback') {
      // Keep real relay candidates. Redirect direct UDP checks to our own silent socket.
      const iface = Object.values(networkInterfaces()).flat().find(ip => !ip.internal && ip.family === 'IPv4');
      if (!iface) throw new Error('Fallback fixture requires an active local IPv4 interface.');
      sink = createSocket('udp4');
      sink.on('message', packet => {
        if (packet.length >= 20 && packet.readUInt32BE(4) === 0x2112a442) checks++;
      });
      await new Promise((resolve, reject) => sink.once('error', reject).bind(0, iface.address, resolve));
      unreachable = { address: iface.address, port: sink.address().port };
    }
    for (const [browser, isHost] of [[hostBrowser, true], [viewerBrowser, false]]) {
      const context = await browser.newContext();
      contexts.push(context);
      await context.addInitScript(installFixture, { host: isHost, unreachable });
      const page = await context.newPage();
      page.setDefaultTimeout(18_000);
      await page.goto(isHost ? local.launchUrl : local.launchUrl.split('#')[0]);
      if (isHost) host = page;
      else viewer = page;
    }
    await host.locator('#host-key').fill(access);
    await host.locator('#create-invite').click();
    const invite = await waitFor(() => host.locator('#invite-code').inputValue(), 'invitation');
    await viewer.locator('#viewer-invite').fill(invite);
    await viewer.locator('#join-session').click();
    if (mode === 'relay') await host.locator('#relay-only').check();
    await host.locator('#accept-request').click();
    await viewer.locator('#playback-button').click();
    const result = await waitFor(async () => {
      const data = await metrics(viewer);
      const hasRelay = [data.local, data.remote].includes('relay');
      return data.local && data.remote && hasRelay === (mode !== 'direct') &&
        data.framesDecoded > 3 && data.displayed > 2 && data.audioPackets > 0 &&
        Math.abs(data.toneHz - 440) < 30 && data;
    }, 'selected ICE route, displayed video, and decoded tone', 45_000);
    assert.equal(result.policy, mode === 'relay' ? 'relay' : 'all');
    assert.match(result.audioCodec, /^audio\/opus$/i);
    assert.match(result.videoCodec, /^video\//i);
    const expectedRoute = mode === 'direct' ? 'direct' : 'relay';
    await viewer.locator(`#route-status[data-kind="${expectedRoute}"]`).waitFor();
    if (mode === 'fallback') {
      assert.ok(result.filtered + (await metrics(host)).filtered > 0, 'direct candidates were redirected');
      await waitFor(() => checks > 0, 'an actual unanswered ICE connectivity check');
    }
    const screen = viewer.locator('#remote-video');
    await viewer.locator('#remote-video[data-control="ready"]').waitFor();
    await screen.click();
    await viewer.keyboard.press('KeyA');
    await viewer.mouse.wheel(0, 120);
    await waitFor(() => ['button 0 1', 'button 0 0', 'key 30 1', 'key 30 0'].every(
      command => commands.includes(command)) && ['move ', 'wheel '].every(
      prefix => commands.some(command => command.startsWith(prefix))), 'Windows adapter key and mouse transitions');
    await host.locator('#stop-session').click();
    await waitFor(() => nativeClosed, 'native adapter revocation');
    await waitFor(() => host.evaluate(() => window.smoke.fixture.stream.getTracks()
      .every(track => track.readyState === 'ended')), 'capture track cleanup');
    await viewer.locator('#session-status[data-state="ended"]').waitFor();
    const afterStop = commands.length;
    await viewer.keyboard.press('KeyB');
    assert.equal(commands.length, afterStop);
    console.log(JSON.stringify({ mode, ...result, unansweredDirectChecks: checks, receivedInput: true, revoked: true }));
  } catch (error) {
    for (const [role, page] of [['host', host], ['viewer', viewer]]) {
      if (page) console.error(role, await page.locator('#session-status').textContent(), await metrics(page));
    }
    throw error;
  } finally {
    await Promise.allSettled(contexts.map(context => context.close()));
    sink?.close();
    await local.close();
  }
}

const mode = process.argv[2] || 'direct';
if (!['direct', 'relay', 'fallback', 'all'].includes(mode)) throw new Error('Use direct, relay, fallback or all.');
const turnUrls = (process.env.EVP_TURN_URLS || '').split(',').map(s => s.trim()).filter(Boolean);
if (mode !== 'direct' && (!turnUrls.length || !process.env.EVP_TURN_SECRET)) {
  throw new Error('Relay tests need EVP_TURN_URLS and EVP_TURN_SECRET for a reachable coturn service.');
}
const hostKey = randomBytes(24).toString('base64url');
const signal = createSignalServer({ hostKey, turnUrls, turnSecret: process.env.EVP_TURN_SECRET });
const browsers = [];
try {
  await new Promise((resolve, reject) => signal.once('error', reject).listen(0, '127.0.0.1', resolve));
  for (let i = 0; i < 2; i++) {
    browsers.push(await chromium.launch({ channel: process.env.EVP_BROWSER_CHANNEL || 'msedge', headless: true,
      args: ['--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling',
        '--disable-renderer-backgrounding'] }));
  }
  for (const scenario of mode === 'all' ? ['direct', 'fallback', 'relay'] : [mode]) {
    await runCase(scenario, `ws://127.0.0.1:${signal.address().port}`, hostKey, ...browsers);
  }
} finally {
  await Promise.allSettled(browsers.map(browser => browser.close()));
  await new Promise(resolve => signal.close(resolve));
}
