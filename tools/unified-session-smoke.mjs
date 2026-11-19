import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { createInterface } from 'node:readline';
import { resolve } from 'node:path';
import { createSignalServer } from '../server/signaling.mjs';

// Both processes use the same probe binary and the shipping HomeWindow and DesktopController. Only capture
// devices, OS input, and local clicks use test adapters. Invitations stay in memory.
const directory = process.argv[2];
const mode = process.argv[3] || 'direct';
if (!directory || !['direct', 'deny', 'relay', 'video-only', 'release-retry'].includes(mode)) {
  throw new Error('Usage: node tools/unified-session-smoke.mjs <probe runtime directory> [direct|relay|deny|video-only|release-retry]');
}
const key = randomBytes(24).toString('base64url');
const turnUrls = (process.env.EVP_TURN_URLS || '').split(',').map(url => url.trim()).filter(Boolean);
if (mode === 'relay' && (!turnUrls.length || !process.env.EVP_TURN_SECRET)) {
  throw new Error('A reachable test TURN server and its signing secret are required.');
}
const server = createSignalServer({hostKey:key, turnUrls, turnSecret:process.env.EVP_TURN_SECRET});
await new Promise((ok,fail) => server.once('error',fail).listen(0,'127.0.0.1',ok));
const url = `ws://127.0.0.1:${server.address().port}/`;
const children = [];
const reports = {};
const childEnv = {...process.env};
delete childEnv.EVP_TURN_SECRET;
delete childEnv.EVP_HOST_KEY;
let inviteResolve, inviteReject;
const invitation = new Promise((ok,fail) => { inviteResolve=ok; inviteReject=fail; });
function launch(role, env) {
  const child = spawn(resolve(directory,'EvpUnifiedProbe.exe'),[],{
    windowsHide:true, stdio:['ignore','pipe','pipe'],
    env:{...childEnv,EVP_PROBE_ROLE:role,EVP_PROBE_URL:url,EVP_PROBE_CASE:mode,EVP_PROBE_ROUTE:mode==='relay'?'relay':'direct',...env}
  });
  children.push(child);
  let errors='';
  child.stderr.setEncoding('utf8').on('data',part=>{errors=(errors+part).slice(-2048);});
  createInterface({input:child.stdout}).on('line',line=>{
    if(!line.startsWith('{')) return;
    const data=JSON.parse(line);
    if(role==='host' && data.invite) inviteResolve(data.invite);
    else reports[role]=data;
  });
  return new Promise((ok,fail)=>{
    child.once('error',fail);
    child.once('close',code=>code===0?ok():fail(new Error(`${role} exited ${code}: ${errors}`)));
  });
}
try {
  const host=launch('host',{EVP_PROBE_KEY:key});
  host.then(() => inviteReject(new Error('Host exited before invitation')),inviteReject);
  const invite=await invitation;
  const viewer=launch('viewer',{EVP_PROBE_INVITE:invite});
  await Promise.all([host,viewer]);
  assert.equal(reports.host?.passed,true);
  assert.equal(reports.host.denied, mode==='deny');
  if(mode==='deny') {
    assert.equal(reports.host.videoStarts,0);
    assert.equal(reports.host.inputDelivered,false);
  } else {
    assert.equal(reports.host.mediaStops,1);
    assert.equal(reports.host.inputDelivered,true);
    if(mode==='release-retry') assert.equal(reports.host.releasedAfterRevocation,1);
    assert.equal(reports.viewer.route,mode==='relay'?'relay':'direct');
    assert.equal(reports.viewer.receivedAudio,mode!=='video-only');
    assert.ok(reports.viewer.gpuDecoded+reports.viewer.cpuDecoded>=4);
    assert.ok(reports.viewer.presents>=4);
    assert.equal(reports.viewer.presentationFailures,0);
  }
  console.log(JSON.stringify({mode,...reports}));
} finally {
  for(const child of children) if(child.exitCode===null) child.kill();
  await new Promise(ok=>server.close(ok));
}
