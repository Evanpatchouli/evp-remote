import { execFile, spawn } from 'node:child_process';
import { stat } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { promisify } from 'node:util';

const usage = 'Usage: node tools/probe-native-media.mjs <recording> [--target-fps 144]';
const run = promisify(execFile);
const metadataEntries = [
  'format=format_name,duration',
  'stream=index,codec_type,codec_name,profile,width,height,pix_fmt,bits_per_raw_sample,color_range,color_space,color_primaries,color_transfer,time_base,avg_frame_rate,sample_rate,channels,channel_layout',
  'stream_disposition=default,attached_pic',
].join(':');
const chromaSampling = {
  '0,0': '4:4:4',
  '0,1': '4:4:0',
  '1,0': '4:2:2',
  '1,1': '4:2:0',
  '2,0': '4:1:1',
  '2,2': '4:1:0',
};
const diagnosticLimit = 4096;
const segmentLimit = 32;
const microsPerSecond = 1_000_000;

async function probeJson(args) {
  const { stdout, stderr } = await run('ffprobe', ['-v', 'error', '-of', 'json', ...args], {
    maxBuffer: 1024 * 1024, windowsHide: true,
  });
  if (stderr.trim()) throw new Error(stderr.trim().slice(0, diagnosticLimit));
  return JSON.parse(stdout);
}

function timestampMicros(line) {
  const match = /\|best_effort_timestamp_time=(-?\d+(?:\.\d+)?)(?:\||$)/.exec(line);
  if (!match) return null;
  const micros = Math.round(Number(match[1]) * microsPerSecond);
  return Number.isSafeInteger(micros) ? micros : null;
}

async function summarizeFrames(lines, targetFps) {
  const targetGapMicros = 1.5 * (microsPerSecond / targetFps);
  const segments = [];
  let segmentCount = 0;
  let segment;
  let frameCount = 0;
  let missingTimestampFrames = 0;
  let repeatedTimestampFrames = 0;
  let backwardTimestampJumps = 0;
  let targetIntervalGaps = 0;
  let longestInterval = null;
  let positiveIntervals = 0;
  let measuredMicros = 0;

  function finishSegment() {
    if (!segment) return;
    segmentCount++;
    if (segments.length < segmentLimit) {
      const elapsed = (segment.end - segment.start) / microsPerSecond;
      segments.push({
        firstFrame: segment.firstFrame,
        frameCount: segment.frames,
        startSeconds: segment.start / microsPerSecond,
        endSeconds: segment.end / microsPerSecond,
        effectiveIntervalFps: elapsed > 0 ? segment.intervals / elapsed : null,
      });
    }
    segment = undefined;
  }

  for await (const line of lines) {
    if (line !== 'frame' && !line.startsWith('frame|')) continue;
    frameCount++;
    const timestamp = timestampMicros(line);
    if (timestamp === null) {
      missingTimestampFrames++;
      finishSegment();
      continue;
    }
    if (segment && timestamp < segment.end) {
      backwardTimestampJumps++;
      finishSegment();
    }
    if (!segment) {
      segment = { firstFrame: frameCount, start: timestamp, end: timestamp, frames: 1, intervals: 0 };
      continue;
    }
    const delta = timestamp - segment.end;
    if (delta === 0) {
      repeatedTimestampFrames++;
    } else {
      positiveIntervals++;
      measuredMicros += delta;
      segment.intervals++;
      if (delta > targetGapMicros) targetIntervalGaps++;
      if (!longestInterval || delta > longestInterval.micros) {
        longestInterval = { from: segment.end, to: timestamp, micros: delta };
      }
    }
    segment.end = timestamp;
    segment.frames++;
  }
  finishSegment();
  return {
    frameCount,
    timestampedFrames: frameCount - missingTimestampFrames,
    missingTimestampFrames,
    repeatedTimestampFrames,
    backwardTimestampJumps,
    targetFps,
    gapThresholdSeconds: targetGapMicros / microsPerSecond,
    targetIntervalGaps,
    longestInterval: longestInterval && {
      startSeconds: longestInterval.from / microsPerSecond,
      endSeconds: longestInterval.to / microsPerSecond,
      durationSeconds: longestInterval.micros / microsPerSecond,
    },
    effectiveIntervalFps: measuredMicros > 0 ? positiveIntervals * microsPerSecond / measuredMicros : null,
    segmentCount,
    segments,
    segmentsOmitted: segmentCount - segments.length,
  };
}

async function probeFrames(recording, index, targetFps) {
  const child = spawn('ffprobe', [
    '-v', 'error', '-select_streams', String(index), '-show_frames',
    '-show_entries', 'frame=best_effort_timestamp_time', '-of', 'compact=p=1:nk=0', '-i', recording,
  ], { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  let diagnostic = '';
  child.stderr.setEncoding('utf8').on('data', chunk => {
    diagnostic += chunk.slice(0, diagnosticLimit - diagnostic.length);
  });
  const completion = new Promise((resolveExit, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => resolveExit({ code, signal }));
  });
  const lines = createInterface({ input: child.stdout, crlfDelay: Infinity });
  const [cadence, { code, signal }] = await Promise.all([summarizeFrames(lines, targetFps), completion]);
  if (code !== 0 || diagnostic.trim()) {
    throw new Error(diagnostic.trim() || `ffprobe frame scan failed (${signal ?? code}).`);
  }
  if (!cadence.frameCount) throw new Error('ffprobe returned no frames for the selected video stream.');
  return cadence;
}

async function inspect(recording, targetFps) {
  const { streams = [], format } = await probeJson(['-show_entries', metadataEntries, '-i', recording]);
  const videos = streams.filter(stream => stream.codec_type === 'video' && !stream.disposition?.attached_pic);
  const video = videos.find(stream => stream.disposition?.default) ?? videos[0];
  if (!video) throw new Error('Recording has no video stream other than cover art.');
  if (!Number.isInteger(video.index) || !video.codec_name || !video.pix_fmt ||
      !(video.width > 0 && video.height > 0) || !format?.format_name) {
    throw new Error('ffprobe did not identify the video codec, dimensions, pixel format, or container.');
  }
  const { pixel_formats: formats } = await probeJson(['-show_pixel_formats']);
  const pixelFormat = formats?.find(description => description.name === video.pix_fmt);
  const chroma = pixelFormat && `${pixelFormat.log2_chroma_w},${pixelFormat.log2_chroma_h}`;
  const audio = streams.filter(stream => stream.codec_type === 'audio').map(stream => ({
    streamIndex: stream.index,
    codec: stream.codec_name ?? null,
    sampleRateHz: Number(stream.sample_rate) || null,
    channels: stream.channels ?? null,
    channelLayout: stream.channel_layout ?? null,
  }));
  return {
    container: format.format_name,
    containerDurationSeconds: Number.isFinite(Number(format.duration)) ? Number(format.duration) : null,
    video: {
      streamIndex: video.index,
      codec: video.codec_name,
      profile: video.profile ?? null,
      width: video.width,
      height: video.height,
      pixelFormat: video.pix_fmt,
      componentBitDepths: pixelFormat?.components?.map(component => component.bit_depth) ?? [],
      codedBitDepth: Number(video.bits_per_raw_sample) || null,
      chromaSampling: chromaSampling[chroma] ?? null,
      colorRange: video.color_range ?? null,
      colorSpace: video.color_space ?? null,
      colorPrimaries: video.color_primaries ?? null,
      colorTransfer: video.color_transfer ?? null,
      timeBase: video.time_base ?? null,
      averageFrameRateMetadata: video.avg_frame_rate ?? null,
    },
    audio,
    cadence: await probeFrames(recording, video.index, targetFps),
    interpretation: [
      'Frame counts come from decoding this recording. A missing or backward best_effort_timestamp_time ends a segment.',
      'Repeated timestamps count adjacent equal timestamps. Effective FPS counts positive intervals over their duration.',
      'Target gaps exceed 1.5 times the target interval. Audio fields describe encoded tracks.',
      'Identical image content, unique visible display frames, and input latency require separate measurements.',
    ],
  };
}

async function main(args) {
  if (args.length === 1 && args[0] === '--help') {
    console.log(usage);
    return;
  }
  const [input, flag, value] = args;
  if (args.length !== 1 && (args.length !== 3 || flag !== '--target-fps')) throw new Error(usage);
  const targetFps = flag ? Number(value) : 144;
  if (!Number.isFinite(targetFps) || targetFps <= 0 || !Number.isFinite(1.5 * microsPerSecond / targetFps)) {
    throw new Error('--target-fps must be a finite positive number.');
  }
  const recording = resolve(input);
  if (!(await stat(recording)).isFile()) throw new Error('Recording must be a file.');
  console.log(JSON.stringify(await inspect(recording, targetFps), null, 2));
}

main(process.argv.slice(2)).catch(error => {
  const detail = error.code === 'ENOENT' && error.path === 'ffprobe'
    ? 'Install ffprobe and add it to PATH.'
    : error.stderr?.trim() || error.message;
  console.error(detail.slice(0, diagnosticLimit));
  process.exitCode = 1;
});
