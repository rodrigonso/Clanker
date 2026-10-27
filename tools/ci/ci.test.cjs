'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');
const policy = require('./package.cjs');
const prepareDraft = require('./draft.cjs');

const originalRoot = process.cwd();
const fixtureNotice = 'Test fixture notice; not a license grant.\n';
const version = '0.1.0';

function write(file, data) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, data);
}

function fixture(callback) {
  return async () => {
    const directory = path.join('out', `ci-tests-${crypto.randomBytes(8).toString('hex')}`);
    fs.mkdirSync(directory, { recursive: true });
    process.chdir(directory);
    const ref = process.env.GITHUB_REF;
    delete process.env.GITHUB_REF;
    try {
      write('source/VERSION', `${version}\n`);
      for (const file of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'licenses/pi/LICENSE', 'licenses/jcode/LICENSE']) {
        write(path.join('source', file), fixtureNotice);
      }
      await callback();
    } finally {
      if (ref === undefined) delete process.env.GITHUB_REF;
      else process.env.GITHUB_REF = ref;
      process.chdir(originalRoot);
      fs.rmSync(directory, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
    }
  };
}

function assets(platforms = policy.platforms, directory = 'out/release-assets') {
  for (const platform of platforms) {
    const name = policy.archiveName(version, platform);
    const data = Buffer.from(`fixture ${platform}`);
    write(path.join(directory, name), data);
    write(path.join(directory, `${name}.sha256`),
      `${crypto.createHash('sha256').update(data).digest('hex')}  ${name}\n`);
  }
  return directory;
}

function extracted(platform = 'Windows-x86_64') {
  const prefix = 'extracted';
  for (const name of policy.expectedFiles(platform)) write(path.join(prefix, name), fixtureNotice);
  const [os, arch] = platform.split('-');
  write(path.join(prefix, policy.notices[0]), [
    `Clanker ${version} - ${os} ${arch}`,
    'libcurl 8.11.1 is statically linked',
    'PCRE2 10.48 (8-bit) is statically linked',
    'unsigned unnotarized',
    'corruption, not publisher identity',
    os === 'Windows' ? 'native Schannel' : 'native SecureTransport',
  ].join('\n'));
  return prefix;
}

function mockGithub(initial, options = {}) {
  let release = initial;
  let uploaded = (options.assets || []).map((name, index) => ({ name, id: index + 1 }));
  const writes = [];
  let checks = 0;
  const repos = {
    listReleases: async () => ({ data: release ? [release] : [] }),
    getRelease: async () => {
      checks++;
      if (options.publishAt === checks) release = { ...release, draft: false };
      return { data: { ...release } };
    },
    listReleaseAssets: async () => ({ data: uploaded }),
    createRelease: async params => {
      writes.push(['create', params]);
      release = { ...params, id: 42 };
      return { data: release };
    },
    updateRelease: async params => {
      writes.push(['update', params]);
      release = { ...release, ...params };
      return { data: release };
    },
    deleteReleaseAsset: async params => {
      writes.push(['delete', params]);
      uploaded = uploaded.filter(asset => asset.id !== params.asset_id);
    },
    uploadReleaseAsset: async params => {
      writes.push(['upload', params]);
      uploaded.push({ id: 100 + uploaded.length, name: params.name, size: params.data.length });
      return { data: uploaded.at(-1) };
    },
  };
  const git = {
    getRef: async () => {
      if (options.missingTag) throw new Error('Tag not found');
      return { data: { object: options.annotated
        ? { type: 'tag', sha: 'b'.repeat(40) }
        : { type: 'commit', sha: options.tagSha || 'a'.repeat(40) } } };
    },
    getTag: async () => ({ data: { object: { type: 'commit', sha: options.tagSha || 'a'.repeat(40) } } }),
  };
  return {
    github: { rest: { repos, git }, paginate: async (method, params) => (await method(params)).data },
    writes,
  };
}

function draftOptions(mock, overrides = {}) {
  return {
    github: mock.github,
    context: {
      eventName: 'push', ref: `refs/tags/v${version}`, sha: 'a'.repeat(40),
      repo: { owner: 'fixture', repo: 'fixture' },
      serverUrl: 'https://github.com', runId: 123, ...overrides,
    },
    core: { info: () => {} },
  };
}

test('version accepts exact tag and CRLF', fixture(() => {
  write('source/VERSION', `${version}\r\n`);
  assert.equal(policy.versionFrom('source', `refs/tags/v${version}`), version);
}));

test('version rejects tag mismatch and unsafe VERSION', fixture(() => {
  assert.throws(() => policy.versionFrom('source', 'refs/tags/v9.9.9'), /Tag must exactly match/);
  for (const value of ['../escape', '0.1.0;echo', '', 'v0.1.0', '01.1.0',
    '0.1.0-alpha', '0.1.0+build', '0.1', '0.1.0.1']) {
    write('source/VERSION', value);
    assert.throws(() => policy.versionFrom('source'));
  }
}));

test('exactly three archives and three checksum files validate', fixture(() => {
  assert.equal(policy.validateAssets(assets(), version).length, 6);
}));

test('tampered archive fails checksum validation', fixture(() => {
  const directory = assets();
  write(path.join(directory, policy.archiveName(version, policy.platforms[0])), 'changed');
  assert.throws(() => policy.validateAssets(directory, version), /Checksum mismatch/);
}));

test('wrong checksum filename cannot redirect validation', fixture(() => {
  const directory = assets();
  const name = policy.archiveName(version, policy.platforms[0]);
  const sidecar = path.join(directory, `${name}.sha256`);
  write(sidecar, fs.readFileSync(sidecar, 'utf8').replace(name, '../elsewhere'));
  assert.throws(() => policy.validateAssets(directory, version), /Invalid SHA256 sidecar/);
}));

test('unexpected files and missing architectures fail closed', fixture(() => {
  const directory = assets(['Windows-x86_64']);
  assert.throws(() => policy.validateAssets(directory, version), /Expected exactly/);
  assets();
  write(path.join(directory, 'extra.txt'), 'extra');
  assert.throws(() => policy.validateAssets(directory, version), /Expected exactly/);
}));

test('archive entry allowlist rejects traversal, duplicate and missing notices', () => {
  const root = `clanker-${version}-Windows-x86_64`;
  const entries = policy.expectedFiles('Windows-x86_64').map(name => `${root}/${name}`);
  policy.verifyEntries([`${root}/`, ...entries], root, 'Windows-x86_64');
  for (const extra of ['../escape', '/absolute', `${root}/bin/helper.dll`, `${root}/../../escape`]) {
    assert.throws(() => policy.verifyEntries([...entries, extra], root, 'Windows-x86_64'), /Unexpected/);
  }
  assert.throws(() => policy.verifyEntries([...entries, entries[0]], root, 'Windows-x86_64'), /Duplicate/);
  assert.throws(() => policy.verifyEntries(entries.slice(1), root, 'Windows-x86_64'), /missing required/);
});

test('extracted notices and metadata match every target', fixture(() => {
  for (const platform of policy.platforms) {
    const prefix = extracted(platform);
    policy.verifyExtracted(prefix, 'source', version, platform);
    fs.rmSync(prefix, { recursive: true });
  }
}));

test('missing project license blocks extracted packages', fixture(() => {
  const prefix = extracted();
  fs.rmSync(path.join(prefix, 'share/clanker/licenses/clanker/LICENSE'));
  assert.throws(() => policy.verifyExtracted(prefix, 'source', version, 'Windows-x86_64'), /contents/);
}));

test('notice mismatch and inaccurate static TLS metadata fail', fixture(() => {
  const prefix = extracted();
  write(path.join(prefix, 'share/clanker/licenses/clanker/LICENSE'), 'wrong notice');
  assert.throws(() => policy.verifyExtracted(prefix, 'source', version, 'Windows-x86_64'), /differs from source/);
  extracted();
  const metadata = path.join(prefix, policy.notices[0]);
  write(metadata, fs.readFileSync(metadata, 'utf8').replace('native Schannel', 'other TLS'));
  assert.throws(() => policy.verifyExtracted(prefix, 'source', version, 'Windows-x86_64'), /metadata/);
}));

test('smoke refuses a non-native target', fixture(async () => {
  assets();
  const platform = process.platform === 'win32' ? 'Darwin-arm64' : 'Windows-x86_64';
  await assert.rejects(policy.smoke('source', 'out/release-assets', platform), /native target/);
}));

test('draft creation always requests draft and prerelease', fixture(async () => {
  assets();
  const mock = mockGithub();
  await prepareDraft(draftOptions(mock));
  assert.equal(mock.writes[0][0], 'create');
  assert.equal(mock.writes[0][1].draft, true);
  assert.equal(mock.writes[0][1].prerelease, true);
  assert.match(mock.writes[0][1].body, /unsigned/);
  assert.equal(mock.writes.filter(([name]) => name === 'upload').length, 6);
}));

test('existing drafts replace the six known assets without publishing', fixture(async () => {
  const directory = assets();
  const mock = mockGithub({ id: 42, tag_name: `v${version}`, draft: true },
    { assets: fs.readdirSync(directory) });
  await prepareDraft(draftOptions(mock));
  assert.equal(mock.writes[0][0], 'update');
  assert.equal(mock.writes[0][1].draft, true);
  assert.equal(mock.writes.filter(([name]) => name === 'delete').length, 6);
  assert.equal(mock.writes.filter(([name]) => name === 'upload').length, 6);
}));

test('annotated version tag is resolved to the tested source commit', fixture(async () => {
  assets();
  const mock = mockGithub(undefined, { annotated: true });
  await prepareDraft(draftOptions(mock));
  assert.equal(mock.writes.filter(([name]) => name === 'upload').length, 6);
}));

test('moved or deleted version tags cannot create a release or recreate a tag', fixture(async () => {
  assets();
  for (const options of [{ tagSha: 'c'.repeat(40) }, { annotated: true, tagSha: 'c'.repeat(40) }, { missingTag: true }]) {
    const mock = mockGithub(undefined, options);
    await assert.rejects(prepareDraft(draftOptions(mock)), /tag moved|Tag not found/);
    assert.equal(mock.writes.length, 0);
  }
}));

test('published release cannot be changed', fixture(async () => {
  assets();
  const mock = mockGithub({ id: 42, tag_name: `v${version}`, draft: false });
  await assert.rejects(prepareDraft(draftOptions(mock)), /already published/);
  assert.equal(mock.writes.length, 0);
}));

test('published-during-run release stops further writes', fixture(async () => {
  assets();
  const mock = mockGithub({ id: 42, tag_name: `v${version}`, draft: true }, { publishAt: 2 });
  await assert.rejects(prepareDraft(draftOptions(mock)), /already published/);
  assert.equal(mock.writes.length, 0);
}));

test('drafts with unexpected manual assets are left alone', fixture(async () => {
  assets();
  const mock = mockGithub({ id: 42, tag_name: `v${version}`, draft: true }, { assets: ['manual-notes.txt'] });
  await assert.rejects(prepareDraft(draftOptions(mock)), /unexpected assets/);
  assert.equal(mock.writes.length, 0);
}));

test('PRs, manual events, branches and mismatched tags cannot prepare drafts', fixture(async () => {
  assets();
  for (const context of [
    { eventName: 'pull_request' }, { eventName: 'workflow_dispatch' },
    { ref: 'refs/heads/main' }, { ref: 'refs/tags/v9.9.9' }, { ref: 'refs/tags/v0.1.0;echo' },
  ]) {
    const mock = mockGithub();
    await assert.rejects(prepareDraft(draftOptions(mock, context)));
    assert.equal(mock.writes.length, 0);
  }
}));

test('missing LICENSE and corrupt downloaded assets block all release writes', fixture(async () => {
  const directory = assets();
  fs.rmSync('source/LICENSE');
  const mock = mockGithub();
  await assert.rejects(prepareDraft(draftOptions(mock)));
  write('source/LICENSE', fixtureNotice);
  write(path.join(directory, policy.archiveName(version, policy.platforms[0])), 'corruption');
  await assert.rejects(prepareDraft(draftOptions(mock)), /Checksum mismatch/);
  assert.equal(mock.writes.length, 0);
}));

test('local native archive smoke', { skip: !process.env.CLANKER_CI_PACKAGE_DIR }, fixture(async () => {
  for (const file of ['VERSION', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'licenses/pi/LICENSE', 'licenses/jcode/LICENSE',
    'tools/install/install.ps1', 'tools/install/install.sh']) {
    write(path.join('source', file), fs.readFileSync(path.join(originalRoot, file)));
  }
  const os = process.platform === 'win32' ? 'Windows' : 'Darwin';
  const arch = process.arch === 'x64' ? 'x86_64' : process.arch;
  await policy.smoke('source', path.resolve(originalRoot, process.env.CLANKER_CI_PACKAGE_DIR), `${os}-${arch}`);
}));
