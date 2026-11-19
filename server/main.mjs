import { readFileSync } from 'node:fs';
import { createSignalServer } from './signaling.mjs';

function parseUrls(value = '') {
  return value.split(',').map(url => url.trim()).filter(Boolean);
}

try {
  const env = process.env;
  const keyFile = env.EVP_TLS_KEY_FILE;
  const certFile = env.EVP_TLS_CERT_FILE;
  if (Boolean(keyFile) !== Boolean(certFile)) {
    throw new Error('Set both EVP_TLS_KEY_FILE and EVP_TLS_CERT_FILE to enable WSS.');
  }

  const proxy = env.EVP_SIGNAL_TRUST_PROXY === '1';
  const host = env.EVP_SIGNAL_HOST ?? (keyFile ? '0.0.0.0' : '127.0.0.1');
  const port = Number(env.EVP_SIGNAL_PORT ?? 8787);
  if (!Number.isInteger(port) || port < 1 || port > 65_535) {
    throw new Error('EVP_SIGNAL_PORT must be an integer between 1 and 65535.');
  }
  const loopback = host === '127.0.0.1' || host === '::1';
  if (proxy && !loopback) throw new Error('Proxy mode requires a loopback bind address.');
  const production = env.NODE_ENV === 'production' || proxy || !loopback;
  if (production && !keyFile && !proxy) {
    throw new Error('Production signaling requires TLS or an explicit loopback TLS reverse proxy.');
  }

  const turnUrls = parseUrls(env.EVP_TURN_URLS);
  if (production && !turnUrls.length) {
    throw new Error('Production signaling requires EVP_TURN_URLS and EVP_TURN_SECRET.');
  }
  const tls = keyFile ? { key: readFileSync(keyFile), cert: readFileSync(certFile) } : undefined;
  const server = createSignalServer({
    hostKey: env.EVP_HOST_KEY,
    stunUrls: parseUrls(env.EVP_STUN_URLS),
    turnUrls,
    turnSecret: env.EVP_TURN_SECRET,
    tls,
  });
  server.on('error', error => {
    console.error(`Signaling server failed to listen (${error.code ?? 'unknown error'}).`);
    process.exitCode = 1;
  });
  server.listen(port, host, () => {
    const scheme = tls ? 'wss' : 'ws';
    const address = host.includes(':') ? `[${host}]` : host;
    const location = `${scheme}://${address}:${port}/`;
    console.info(`Signaling server listening on ${location}${proxy ? ' behind a TLS reverse proxy' : ''}.`);
  });
} catch (error) {
  console.error(`Signaling configuration failed. ${error.message}`);
  process.exitCode = 1;
}
