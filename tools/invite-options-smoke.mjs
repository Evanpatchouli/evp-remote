import { spawn } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { WebSocketServer } from 'ws';

const directory = process.argv[2];
if (!directory) throw new Error('Usage: node tools/invite-options-smoke.mjs <probe runtime directory>');

function canonicalToken(value) {
  return typeof value === 'string' && value.length === 43
    && Buffer.from(value, 'base64url').toString('base64url') === value;
}

async function runCase(probeCase) {
  const legacy = probeCase === 'auto-invite-legacy';
  const key = randomBytes(24).toString('base64url');
  const legacyCode = legacy ? randomBytes(16).toString('base64url') : undefined;
  const server = new WebSocketServer({ host: '127.0.0.1', port: 0, path: '/', maxPayload: 4096 });
  await once(server, 'listening');
  let child, timer, childClosed;
  try {
    await new Promise((pass, reject) => {
      const fail = reason => reject(new Error(reason));
      timer = setTimeout(() => fail('Host probe timed out.'), 20_000);
      server.on('error', () => fail('Loopback stub failed.'));
      let host, verifier;
      let proofSent = false, reported = false, exited = false, disconnected = false;
      function checkDone() {
        if (exited && disconnected && reported) pass();
      }

      server.on('connection', socket => {
        let role;
        socket.on('error', () => fail('Signaling socket failed.'));
        socket.on('close', () => {
          if (socket === host) {
            disconnected = true;
            checkDone();
          }
        });
        socket.on('message', (raw, binary) => {
          let message;
          try {
            if (!binary) message = JSON.parse(raw.toString());
          } catch {
            return fail('Malformed signaling frame.');
          }
          if (!role) {
            if (message?.type === 'device-hello') {
              if (message.key !== key) return fail('Unauthenticated device registration.');
              role = 'device';
              socket.send(JSON.stringify({ type: 'error', message: 'The message type is unsupported.' }),
                error => { if (error) fail('Device protocol rejection failed.'); });
              return;
            }
            if (host) return fail('Unexpected second host connection.');
            if (message?.type !== 'host' || message.key !== key || message.inviteTtlMinutes !== 720
                || !canonicalToken(message.inviteVerifier) || Object.hasOwn(message, 'invite')) {
              return fail('Expected an authenticated 12-hour verifier registration.');
            }
            role = 'host';
            host = socket;
            verifier = message.inviteVerifier;
            const reply = legacy
              ? { type: 'invitation', invite: legacyCode }
              : { type: 'invitation', inviteTtlMinutes: 720, inviteVerifier: verifier };
            host.send(JSON.stringify(reply), error => { if (error) fail('Invitation response failed.'); });
          } else if (role === 'device') {
            fail('Device continued after protocol rejection.');
          } else if (message?.type !== 'end' && message?.type !== 'reject') {
            fail('Host sent unexpected signaling during a denied session.');
          }
        });
      });

      const env = Object.fromEntries(Object.entries(process.env).filter(([name]) => !/^EVP_/i.test(name)));
      child = spawn(resolve(directory, 'EvpUnifiedProbe.exe'), [], {
        windowsHide: true, stdio: ['ignore', 'pipe', legacy ? 'pipe' : 'ignore'],
        env: {
          ...env, EVP_PROBE_ROLE: 'host', EVP_PROBE_CASE: probeCase, EVP_PROBE_ROUTE: 'direct',
          EVP_PROBE_URL: `ws://127.0.0.1:${server.address().port}/`, EVP_PROBE_KEY: key,
        },
      });
      child.once('error', () => fail('Could not launch the host probe.'));
      if (legacy) {
        let tail = '';
        child.stderr.setEncoding('utf8').on('data', chunk => {
          const text = tail + chunk;
          if (text.includes(legacyCode)) fail('Host exposed the legacy invitation in diagnostics.');
          tail = text.slice(1 - legacyCode.length);
        });
      }
      childClosed = new Promise(resolveClose => child.once('close', code => {
        resolveClose();
        exited = true;
        if (code !== 0 || !reported) return fail('Host exited without a successful report.');
        checkDone();
      }));
      createInterface({ input: child.stdout }).on('line', line => {
        let data;
        try {
          data = JSON.parse(line);
        } catch {
          return fail('Unexpected host output.');
        }
        if (!data || typeof data !== 'object' || Object.keys(data).length !== 1 || reported) {
          return fail('Unexpected host output record.');
        }
        if (Object.hasOwn(data, 'invite')) {
          if (legacy || proofSent || !canonicalToken(data.invite)
              || createHash('sha256').update(data.invite, 'ascii').digest('base64url') !== verifier) {
            return fail('Host emitted an unexpected invitation.');
          }
          const wrong = Buffer.from(data.invite, 'base64url');
          wrong[0] ^= 1;
          proofSent = true;
          host.send(JSON.stringify({ type: 'request', inviteProof: wrong.toString('base64url') }), error => {
            if (error) fail('Invalid-proof request could not be delivered.');
          });
          return;
        }
        const report = data.inviteOptionsReport;
        if (!verifier || proofSent !== !legacy || report?.case !== probeCase
            || report.outcome !== (legacy ? 'upgrade-required' : 'invalid-proof')
            || report.inviteEmitted !== !legacy || report.mediaStarted !== false || report.inputGranted !== false) {
          return fail('Host did not report the expected local failure and clean adapter state.');
        }
        reported = true;
      });
    });
  } finally {
    clearTimeout(timer);
    if (child && child.exitCode === null && child.signalCode === null) child.kill();
    for (const socket of server.clients) socket.terminate();
    await Promise.all([childClosed, new Promise(done => server.close(done))]);
  }
  console.log(`${probeCase} passed`);
}

for (const name of ['auto-invite-legacy', 'auto-invite-invalid-proof']) {
  await runCase(name);
}
