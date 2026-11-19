import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const tool = fileURLToPath(new URL('../tools/probe-native-media.mjs', import.meta.url));
const hasTools = ['ffmpeg', 'ffprobe'].every(command =>
  spawnSync(command, ['-version'], { stdio: 'ignore', windowsHide: true }).status === 0);
const options = { encoding: 'utf8', windowsHide: true };

test('ffprobe counts encoded frames, duplicate timestamps, and a target gap', {
  skip: !hasTools && 'FFmpeg and ffprobe are unavailable on PATH',
}, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'evp-probe-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const recording = join(directory, '144 fps sample.mkv');
  execFileSync('ffmpeg', [
    '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=64x64:rate=144',
    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
    '-frames:v', '144', '-c:v', 'ffv1', '-c:a', 'pcm_s16le', recording,
  ], options);
  const report = JSON.parse(execFileSync(process.execPath, [tool, recording], options));
  assert.equal(report.video.codec, 'ffv1');
  assert.equal(report.video.width, 64);
  assert.equal(report.video.height, 64);
  assert.equal(report.video.pixelFormat, 'yuv420p');
  assert.deepEqual(report.video.componentBitDepths, [8, 8, 8]);
  assert.equal(report.video.chromaSampling, '4:2:0');
  assert.equal(report.audio[0].codec, 'pcm_s16le');
  assert.equal(report.audio[0].sampleRateHz, 48_000);
  assert.equal(report.cadence.frameCount, 144);
  assert.equal(report.cadence.targetIntervalGaps, 0);
  assert.equal(report.cadence.segmentCount, 1);
  assert.ok(Math.abs(report.cadence.effectiveIntervalFps - 144) < 0.1);

  const irregular = join(directory, 'irregular.mkv');
  // Duplicate one frame timestamp, then jump forward by multiple target intervals.
  execFileSync('ffmpeg', [
    '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=64x64:rate=144',
    '-vf', 'setpts=if(eq(N\\,2)\\,PTS-1\\,if(gte(N\\,3)\\,PTS+2\\,PTS))',
    '-frames:v', '6', '-fps_mode', 'passthrough', '-c:v', 'ffv1', irregular,
  ], options);
  const result = JSON.parse(execFileSync(process.execPath, [tool, irregular, '--target-fps', '144'], options));
  assert.deepEqual(result.audio, []);
  assert.equal(result.cadence.frameCount, 6);
  assert.equal(result.cadence.repeatedTimestampFrames, 1);
  assert.equal(result.cadence.targetIntervalGaps, 1);
  assert.ok(result.cadence.longestInterval.durationSeconds > 0.025);
  assert.ok(result.cadence.effectiveIntervalFps < 100);

  const invalid = join(directory, 'invalid.mp4');
  await writeFile(invalid, 'not a recording');
  const failed = spawnSync(process.execPath, [tool, invalid], options);
  assert.equal(failed.status, 1);
  assert.equal(failed.stdout, '');
  assert.ok(failed.stderr.trim());
});

test('ffprobe tool rejects an invalid cadence target before opening a recording', () => {
  const failed = spawnSync(process.execPath, [tool, 'unused.mkv', '--target-fps', '0'], options);
  assert.equal(failed.status, 1);
  assert.match(failed.stderr, /finite positive number/);
});
