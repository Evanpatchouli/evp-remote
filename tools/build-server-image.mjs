import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const { values: { push } } = parseArgs({ options: { push: { type: 'boolean', default: false } } });
const root = fileURLToPath(new URL('../', import.meta.url));
const { version } = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
if (typeof version !== 'string' || version.length > 128 || version.trim() !== version ||
    !/^\d+\.\d+\.\d+(?:-[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*)?(?:\+[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*)?$/.test(version)) {
  throw new Error('package.json version must be a semantic version that fits a 128-character Docker tag.');
}
const image = `ghcr.io/evanpatchouli/evp-remote-signal:${version.replace('+', '_')}`;
const args = [
  'buildx', 'build', '--file', 'deploy/linux/Dockerfile', '--tag', image,
  '--label', `org.opencontainers.image.version=${version}`, '--build-arg', `EVP_SIGNAL_IMAGE=${image}`,
  ...(push ? ['--platform', 'linux/amd64,linux/arm64', '--push'] : ['--load']), '.',
];
console.log(`${push ? 'Building and pushing' : 'Building locally'} ${image}`);
const result = spawnSync('docker', args, { cwd: root, stdio: 'inherit' });
if (result.error) throw result.error;
process.exitCode = result.status ?? 1;
