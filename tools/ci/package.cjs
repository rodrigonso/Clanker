'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const platforms = ['Windows-x86_64', 'Darwin-arm64', 'Darwin-x86_64'];
const notices = [
  'share/clanker/DISTRIBUTION.txt',
  'share/clanker/THIRD_PARTY_NOTICES.md',
  'share/clanker/licenses/clanker/LICENSE',
  'share/clanker/licenses/pi/LICENSE',
  'share/clanker/licenses/jcode/LICENSE',
  'share/clanker/licenses/curl/COPYING',
  'share/clanker/licenses/pcre2/LICENCE.md',
];

function readNonempty(file) {
  assert(fs.lstatSync(file).isFile(), `Not a regular file: ${file}`);
  const data = fs.readFileSync(file);
  assert(data.length > 0, `Empty required file: ${file}`);
  return data;
}

function versionFrom(source, ref = '') {
  const version = readNonempty(path.join(source, 'VERSION')).toString('utf8').trim();
  assert(/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(version),
    'VERSION must be a numeric X.Y.Z version accepted by CMake');
  if (ref.startsWith('refs/tags/')) {
    assert.equal(ref, `refs/tags/v${version}`, 'Tag must exactly match vVERSION');
  }
  return version;
}

function archiveName(version, platform) {
  assert(platforms.includes(platform), `Unsupported platform: ${platform}`);
  return `clanker-${version}-${platform}.${platform.startsWith('Windows') ? 'zip' : 'tar.gz'}`;
}

function verifyChecksum(directory, name) {
  const archive = path.join(directory, name);
  const data = readNonempty(archive);
  const sidecar = readNonempty(`${archive}.sha256`).toString('utf8');
  const match = /^([0-9a-f]{64})  ([^\r\n]+)\r?\n?$/.exec(sidecar);
  assert(match && match[2] === name, `Invalid SHA256 sidecar: ${name}`);
  const hash = crypto.createHash('sha256').update(data).digest('hex');
  assert.equal(hash, match[1], `Checksum mismatch: ${name}`);
}

function validateAssets(directory, version, selected = platforms) {
  const names = selected.map(platform => archiveName(version, platform));
  const expected = names.flatMap(name => [name, `${name}.sha256`]).sort();
  assert.deepEqual(fs.readdirSync(directory).sort(), expected,
    'Expected exactly the platform archives and their individual SHA256 files');
  for (const name of names) verifyChecksum(directory, name);
  return expected;
}

function run(executable, args, options = {}) {
  const result = spawnSync(executable, args, {
    encoding: 'utf8', timeout: 60000, maxBuffer: 4 * 1024 * 1024, ...options,
  });
  if (result.error) throw result.error;
  assert.equal(result.status, 0,
    `${path.basename(executable)} failed (${result.status}): ${result.stderr}`);
  return `${result.stdout || ''}${result.stderr || ''}`;
}

function expectedFiles(platform) {
  return [...notices, `bin/clanker${platform.startsWith('Windows') ? '.exe' : ''}`].sort();
}

function verifyEntries(entries, rootName, platform) {
  const expected = expectedFiles(platform).map(file => `${rootName}/${file}`);
  const directories = new Set();
  for (const file of expected) {
    for (let directory = path.posix.dirname(file); directory !== '.'; directory = path.posix.dirname(directory)) {
      directories.add(`${directory}/`);
    }
  }
  const files = [];
  const seen = new Set();
  for (const entry of entries) {
    assert(!seen.has(entry), `Duplicate archive entry: ${entry}`);
    seen.add(entry);
    assert(expected.includes(entry) || directories.has(entry), `Unexpected archive entry: ${entry}`);
    if (!entry.endsWith('/')) files.push(entry);
  }
  assert.deepEqual(files.sort(), expected.sort(), 'Archive is missing required runtime files/notices');
}

function verifyExtracted(prefix, source, version, platform) {
  const actual = [];
  function walk(directory, relative = '') {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const name = relative ? `${relative}/${entry.name}` : entry.name;
      assert(!entry.isSymbolicLink(), `Links are not allowed in runtime archives: ${name}`);
      if (entry.isDirectory()) walk(path.join(directory, entry.name), name);
      else {
        assert(entry.isFile(), `Unexpected special file: ${name}`);
        readNonempty(path.join(directory, entry.name));
        actual.push(name);
      }
    }
  }
  walk(prefix);
  assert.deepEqual(actual.sort(), expectedFiles(platform), 'Unexpected extracted package contents');
  for (const [packaged, original] of [
    ['share/clanker/licenses/clanker/LICENSE', 'LICENSE'],
    ['share/clanker/licenses/pi/LICENSE', 'licenses/pi/LICENSE'],
    ['share/clanker/licenses/jcode/LICENSE', 'licenses/jcode/LICENSE'],
    ['share/clanker/THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'],
  ]) {
    assert.deepEqual(readNonempty(path.join(prefix, packaged)), readNonempty(path.join(source, original)),
      `Packaged notice differs from source: ${original}`);
  }
  const metadata = readNonempty(path.join(prefix, notices[0])).toString('utf8');
  const [os, arch] = platform.split('-');
  assert.equal(metadata.split(/\r?\n/)[0], `Clanker ${version} - ${os} ${arch}`);
  for (const text of ['libcurl 8.11.1 is statically linked',
    'PCRE2 10.48 (8-bit) is statically linked', 'unsigned', 'corruption, not publisher identity',
    os === 'Windows' ? 'native Schannel' : 'native SecureTransport']) {
    assert(metadata.includes(text), `Missing distribution metadata: ${text}`);
  }
  if (os === 'Darwin') {
    assert(metadata.includes('unnotarized'), 'macOS preview must disclose missing notarization');
  }
}

async function smoke(source, directory, platform) {
  source = fs.realpathSync(source);
  directory = fs.realpathSync(directory);
  const version = versionFrom(source, process.env.GITHUB_REF || '');
  const nativePlatform = process.platform === 'win32' ? 'Windows' : process.platform === 'darwin' ? 'Darwin' : '';
  const nativeArch = process.arch === 'x64' ? 'x86_64' : process.arch;
  assert.equal(platform, `${nativePlatform}-${nativeArch}`, 'Smoke must run on the native target platform');
  const names = validateAssets(directory, version, [platform]);
  const archive = names.find(name => !name.endsWith('.sha256'));
  const rootName = archive.replace(/\.(zip|tar\.gz)$/, '');
  // With checkout under source/, this relative directory is outside the checkout.
  const work = path.join('out', `ci-smoke-${crypto.randomBytes(8).toString('hex')}`);
  const relative = path.relative(source, path.resolve(work));
  assert(relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative),
    'Run smoke from outside the source checkout');
  fs.mkdirSync(work, { recursive: true });
  try {
    const tar = process.platform === 'win32' ? path.join(process.env.SystemRoot, 'System32', 'tar.exe') : '/usr/bin/tar';
    const entries = run(tar, ['-tf', path.join(directory, archive)]).trim().split(/\r?\n/);
    verifyEntries(entries, rootName, platform);
    run(tar, ['-xf', path.join(directory, archive), '-C', path.resolve(work)]);
    const prefix = path.resolve(work, rootName);
    verifyExtracted(prefix, source, version, platform);
    const empty = path.resolve(work, 'empty');
    const home = path.resolve(work, 'home');
    fs.mkdirSync(empty);
    fs.mkdirSync(home);
    const env = process.platform === 'win32'
      ? { SystemRoot: process.env.SystemRoot, WINDIR: process.env.SystemRoot,
        PATH: path.join(process.env.SystemRoot, 'System32'), USERPROFILE: home, HOME: home,
        LOCALAPPDATA: home, APPDATA: home, TEMP: empty, TMP: empty }
      : { PATH: '/usr/bin:/bin', HOME: home, LANG: 'C', LC_ALL: 'C' };
    const executable = path.join(prefix, 'bin', process.platform === 'win32' ? 'clanker.exe' : 'clanker');
    const output = run(executable, ['--help'], { cwd: empty, env, timeout: 15000 });
    assert(/usage:/i.test(output) && output.includes('clanker'), 'Packaged --help returned unexpected output');
    assert.deepEqual(fs.readdirSync(home), [], '--help must not modify the user profile');
    console.log(`Verified ${archive}: checksum, notices, metadata and isolated --help`);
  } finally {
    await fs.promises.rm(work, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
}

module.exports = { archiveName, expectedFiles, notices, platforms, readNonempty, smoke,
  validateAssets, verifyChecksum, verifyEntries, verifyExtracted, versionFrom };

if (require.main === module) {
  (async () => {
    const [command, source, directory, platform] = process.argv.slice(2);
    if (command === 'version') console.log(versionFrom('.', process.env.GITHUB_REF || ''));
    else if (command === 'smoke' && source && directory && platform) await smoke(source, directory, platform);
    else throw new Error('Usage: node package.cjs version | smoke <source> <packages> <platform>');
  })().catch(error => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
