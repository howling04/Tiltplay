import { createServer } from 'node:http';
import { existsSync } from 'node:fs';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as wait } from 'node:timers/promises';
import puppeteer from 'puppeteer-core';

const sampleRate = 48_000;
const cycles = 16;
const warmupCycles = 4;
const secondsPerCycle = 3;
const prerollFrames = warmupCycles * secondsPerCycle * sampleRate;
const frames = cycles * secondsPerCycle * sampleRate;
const headerBytes = 44;
const frameBytes = 4;
const source = new URL('./Quiet_Meridian.strudel.js', import.meta.url);
const output = new URL('./SW_Luna_QuietMeridian.wav', import.meta.url);
const verification = new URL('./loop-verification.json', import.meta.url);
const browserPath = [
  process.env.LUNA_CHROMIUM,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  '/usr/bin/chromium',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].find(path => path && existsSync(path));
if (!browserPath) throw new Error('Set LUNA_CHROMIUM to a Chromium-based browser executable.');

const strudel = new URL('./node_modules/@strudel/web/dist/index.mjs', import.meta.url);
const server = createServer(async (req, res) => {
  if (req.url === '/') {
    res.setHeader('Content-Type', 'text/html; charset=utf-8');
    res.end('<!doctype html><meta charset="utf-8"><link rel="icon" href="data:,"><title>Quiet Meridian render</title>');
    return;
  }
  const routes = { '/strudel.mjs': strudel, '/score.js': source };
  const file = routes[req.url];
  if (!file) {
    res.writeHead(404).end();
    return;
  }
  res.setHeader('Content-Type', 'text/javascript; charset=utf-8');
  res.end(await readFile(file));
});

const temp = await mkdtemp(join(tmpdir(), 'quiet-meridian-'));
let browser;
try {
  await new Promise((resolve, reject) => server.listen(0, '127.0.0.1', resolve).once('error', reject));
  browser = await puppeteer.launch({
    executablePath: browserPath,
    args: ['--autoplay-policy=no-user-gesture-required'],
    protocolTimeout: 600_000,
  });
  const page = await browser.newPage();
  const cdp = await page.createCDPSession();
  await cdp.send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: temp });
  const errors = [];
  page.on('console', message => {
    if (message.text().startsWith('[loop] ')) console.log(message.text());
    if (message.type() === 'error' || /\[superdough\].*(error|failed|not found)/i.test(message.text())) {
      errors.push(message.text());
    }
  });
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const scoreAudit = await page.evaluate(async ({ cycles, warmupCycles, secondsPerCycle, sampleRate }) => {
    // Seed the random state used by SuperDough to generate noise and convolution tails.
    let seed = 42;
    Math.random = () => {
      seed = (Math.imul(1664525, seed) + 1013904223) >>> 0;
      return seed / 4294967296;
    };
    const { initStrudel, renderPatternAudio } = await import('/strudel.mjs');
    const repl = await initStrudel();
    console.info('[loop] Strudel ready');
    const score = await (await fetch('/score.js')).text();
    const pattern = await repl.evaluate(score, false);
    const cps = 1 / secondsPerCycle;
    if (Math.abs(repl.scheduler.cps - cps) > 1e-10) throw new Error('Unexpected score tempo');
    const onsetMap = offset => pattern.queryArc(offset, offset + cycles, { _cps: cps })
      .filter(event => event.hasOnset())
      .map(event => [event.whole.begin.sub(offset).show(), event.whole.end.sub(offset).show(), event.value]);
    const first = onsetMap(0);
    for (const offset of [cycles, 2 * cycles]) {
      if (JSON.stringify(first) !== JSON.stringify(onsetMap(offset))) {
        throw new Error(`Score does not repeat every ${cycles} cycles`);
      }
    }
    const orbitOnsets = {};
    for (const [, , event] of first) orbitOnsets[event.orbit] = (orbitOnsets[event.orbit] ?? 0) + 1;
    if (Object.keys(orbitOnsets).length !== 10) throw new Error('Missing voices in score');
    console.info(`[loop] ${first.length} events per period. Rendering the loop and pre-roll with Strudel.`);
    await renderPatternAudio(pattern, cps, cycles - warmupCycles, cycles * 2, sampleRate, 128, false, 'render');
    return { onsets: first.length, orbitOnsets };
  }, { cycles, warmupCycles, secondsPerCycle, sampleRate });
  if (errors.length) throw new Error(errors.join('\n'));

  const raw = join(temp, 'render.wav');
  for (let attempt = 0; attempt < 200 && !existsSync(raw); attempt++) await wait(50);
  const render = await readFile(raw);
  const sampleAt = (frame, channel) => render.readInt16LE(headerBytes + frame * frameBytes + 2 * channel);
  if (render.toString('ascii', 0, 4) !== 'RIFF' || render.toString('ascii', 36, 40) !== 'data'
      || render.readUInt16LE(20) !== 1 || render.readUInt16LE(22) !== 2
      || render.readUInt32LE(24) !== sampleRate || render.readUInt16LE(34) !== 16
      || render.length !== headerBytes + (prerollFrames + frames) * frameBytes) {
    throw new Error('Unexpected format or length from Strudel');
  }

  // Pre-roll includes the previous period's release and reverb tails.
  const pcm = Buffer.from(render.subarray(headerBytes + prerollFrames * frameBytes));
  const read = (frame, channel) => pcm.readInt16LE(frame * frameBytes + 2 * channel);
  const write = (frame, channel, value) => pcm.writeInt16LE(value, frame * frameBytes + 2 * channel);

  // Align the region just before wrap with the actual pre-roll leading into frame 0.
  // The raised cosine reaches unity before the cut and leaves the opening onset intact.
  const overlapFrames = Math.round(.06 * sampleRate);
  for (let index = 0; index < overlapFrames; index++) {
    const frame = frames - overlapFrames + index;
    const weight = .5 - .5 * Math.cos(Math.PI * index / (overlapFrames - 1));
    for (let channel = 0; channel < 2; channel++) {
      write(frame, channel, Math.round((1 - weight) * read(frame, channel)
        + weight * sampleAt(prerollFrames - overlapFrames + index, channel)));
    }
  }

  let peak = 0;
  for (let frame = 0; frame < frames; frame++) {
    peak = Math.max(peak, Math.abs(read(frame, 0)), Math.abs(read(frame, 1)));
  }
  if (peak < 1000 || peak >= 32767) throw new Error(`Invalid or clipped render (peak ${peak})`);
  const targetPeak = Math.round(32767 * 10 ** (-6 / 20));
  const gain = targetPeak / peak;
  for (let frame = 0; frame < frames; frame++) {
    for (let channel = 0; channel < 2; channel++) write(frame, channel, Math.round(read(frame, channel) * gain));
  }

  const channelAudit = [0, 1].map(channel => {
    const width = Math.round(.025 * sampleRate);
    const differences = [];
    let before = 0;
    let after = 0;
    let sum = 0;
    for (let index = 0; index < frames; index++) sum += read(index, channel) ** 2;
    for (let index = 0; index < width; index++) {
      before += read(frames - width + index, channel) ** 2;
      after += read(index, channel) ** 2;
      if (index > 0) {
        differences.push(Math.abs(read(frames - width + index, channel) - read(frames - width + index - 1, channel)));
        differences.push(Math.abs(read(index, channel) - read(index - 1, channel)));
      }
    }
    differences.sort((a, b) => a - b);
    const localP99 = differences[Math.floor(.99 * differences.length)];
    const wrapStep = Math.abs(read(0, channel) - read(frames - 1, channel));
    const continuousStep = Math.abs(Math.round(sampleAt(prerollFrames, channel) * gain)
      - Math.round(sampleAt(prerollFrames - 1, channel) * gain));
    if (wrapStep !== continuousStep || wrapStep > Math.max(1, localP99) || before === 0 || after === 0) {
      throw new Error(`Discontinuous or silent loop boundary on channel ${channel + 1}`);
    }
    const db = value => Number((20 * Math.log10(value / 32768)).toFixed(2));
    return {
      channel: channel === 0 ? 'L' : 'R', wrapStepPcm16: wrapStep, continuousStepPcm16: continuousStep,
      localStepP99Pcm16: localP99, rmsDbfs: db(Math.sqrt(sum / frames)),
      seamBeforeRmsDbfs: db(Math.sqrt(before / width)), seamAfterRmsDbfs: db(Math.sqrt(after / width)),
    };
  });

  const header = Buffer.from(render.subarray(0, headerBytes));
  header.writeUInt32LE(pcm.length + headerBytes - 8, 4);
  header.writeUInt32LE(pcm.length, 40);
  const wav = Buffer.concat([header, pcm]);
  const audit = {
    ...scoreAudit, bpm: 60 * 4 / secondsPerCycle, cycles, durationSeconds: frames / sampleRate,
    sampleRate, frames, channels: 2, format: 'signed 16-bit PCM', peakDbfs: -6,
    prerollSeconds: prerollFrames / sampleRate, seamOverlapMilliseconds: 1000 * overlapFrames / sampleRate, channelAudit,
    sha256: createHash('sha256').update(wav).digest('hex'),
  };
  await writeFile(output, wav);
  await writeFile(verification, JSON.stringify(audit, null, 2) + '\n');
  console.log(JSON.stringify(audit, null, 2));
} finally {
  await browser?.close();
  server.close();
  await rm(temp, { recursive: true, force: true });
}
