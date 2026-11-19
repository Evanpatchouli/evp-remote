import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { chromium } from 'playwright-core';
import { startDesktop } from '../desktop/server.mjs';
import { createSignalServer } from '../server/signaling.mjs';
import { installFixture } from './fixtures/rtc-browser.mjs';

const binary = process.argv[2];
const mode = process.argv[3] || 'direct';
if (!binary || !['direct', 'relay'].includes(mode))
  throw new Error('Usage: node tools/native-session-smoke.mjs <path to EvpSessionProbe.exe> [direct|relay]');
const turnUrls = (process.env.EVP_TURN_URLS || '').split(',').map(url => url.trim()).filter(Boolean);
if (mode === 'relay' && (!turnUrls.length || !process.env.EVP_TURN_SECRET))
  throw new Error('Relay validation requires a reachable test coturn and its matching local configuration.');
const key = randomBytes(24).toString('base64url');
const service = createSignalServer({ hostKey: key, turnUrls, turnSecret: process.env.EVP_TURN_SECRET });
await new Promise((res, rej) => service.once('error', rej).listen(0, '127.0.0.1', res));
const signalUrl = `ws://127.0.0.1:${service.address().port}/`;
const commands = [];
let released = false;
let desktop, browser, child;
try {
  desktop = await startDesktop({ signalUrl, createInput: async () => ({
    closed: new Promise(() => {}), write: command => commands.push(command),
    close: async () => { released = true; },
  }) });
  browser = await chromium.launch({ channel: process.env.EVP_BROWSER_CHANNEL || 'msedge',
    headless: true, args: ['--autoplay-policy=no-user-gesture-required',
      '--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
  const page = await browser.newPage();
  await page.addInitScript(installFixture, { host: true });
  await page.goto(desktop.launchUrl);
  await page.locator('#host-key').fill(key);
  await page.locator('#create-invite').click();
  await page.waitForFunction(() => Boolean(document.querySelector('#invite-code').value));
  const invite = await page.locator('#invite-code').inputValue();
  child = spawn(resolve(binary), [], { windowsHide: true, stdio: ['ignore','pipe','pipe'],
    env: { ...process.env, EVP_PROBE_URL: signalUrl, EVP_PROBE_INVITE: invite, EVP_PROBE_ROUTE: mode } });
  const closed = once(child, 'close');
  let report, errors = '', lines = '';
  child.stdout.setEncoding('utf8').on('data', text => {
    lines += text;
    const pending = lines.split(/\r?\n/);
    lines = pending.pop();
    for (const line of pending) if (line.startsWith('{"')) report = JSON.parse(line);
  });
  child.stderr.setEncoding('utf8').on('data', text => { errors = (errors + text).slice(-2048); });
  if (mode === 'relay') await page.locator('#relay-only').check({ timeout: 25000 });
  await page.locator('#accept-request').click({ timeout: 25000 });
  const deadline = Date.now() + 45000;
  const wanted = ['key 30 1','key 30 0','button 0 1','button 0 0','wheel 120'];
  while (Date.now() < deadline && child.exitCode === null &&
      (!report || !wanted.every(c => commands.includes(c)))) await delay(80);
  const detail = await page.evaluate(() => ({
    status: document.querySelector('#session-status').textContent,
    peers: window.smoke.peers.map(pc => ({ ice: pc.iceConnectionState, signaling: pc.signalingState })),
  }));
  assert.ok(report, `No native presentation report; ${JSON.stringify(detail)} ${errors}`);
  assert.equal(report.route, mode);
  assert.equal(report.receivedAudio, true);
  assert.equal(report.presentationFailures, 0);
  assert.ok(report.gpuDecoded + report.cpuDecoded >= 4, 'FFmpeg decoded the live frames');
  assert.ok(report.presents >= 4);
  assert.ok(wanted.every(c => commands.includes(c)) && commands.some(c => c.startsWith('move ')),
    'native input reached the approved host adapter');
  const codecs = await page.evaluate(async () => {
    const stats = await window.smoke.peers[0].getStats();
    return [...stats.values()].filter(r => r.type === 'outbound-rtp' && r.packetsSent > 0)
      .map(r => stats.get(r.codecId)?.mimeType);
  });
  assert.ok(codecs.includes('video/H264') && codecs.includes('audio/opus'), 'H.264 + Opus were sent');
  await page.locator('#stop-session').click();
  assert.equal((await closed)[0], 0, errors);
  assert.ok(released);
  console.log(JSON.stringify({ ...report, codecs, nativeInputDelivered: true, hostRevoked: true }));
} finally {
  if (child && child.exitCode === null) child.kill();
  await browser?.close();
  await desktop?.close();
  await new Promise(res => service.close(res));
}
