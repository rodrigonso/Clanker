'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { readNonempty, validateAssets, versionFrom } = require('./package.cjs');

module.exports = async function prepareDraft({ github, context, core }) {
  assert.equal(context.eventName, 'push', 'Only a version-tag push may prepare a draft');
  assert(context.ref.startsWith('refs/tags/v'), 'Drafts require a version tag');
  const source = path.resolve('source');
  const directory = path.resolve('out', 'release-assets');
  const version = versionFrom(source, context.ref);
  for (const notice of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'licenses/pi/LICENSE', 'licenses/jcode/LICENSE']) {
    readNonempty(path.join(source, notice));
  }
  const files = validateAssets(directory, version);
  const tag = `v${version}`;
  const repo = context.repo;
  async function requireSourceTag() {
    const { data } = await github.rest.git.getRef({ ...repo, ref: `tags/${tag}` });
    let object = data.object;
    const seen = new Set();
    while (object.type === 'tag') {
      assert(!seen.has(object.sha) && seen.size < 16, 'Invalid annotated tag chain');
      seen.add(object.sha);
      const { data: annotated } = await github.rest.git.getTag({ ...repo, tag_sha: object.sha });
      object = annotated.object;
    }
    assert.equal(object.type, 'commit', 'Version tag must resolve to a commit');
    assert.equal(object.sha, context.sha, 'Version tag moved away from the tested source commit');
  }
  await requireSourceTag();
  const releases = await github.paginate(github.rest.repos.listReleases, { ...repo, per_page: 100 });
  const matching = releases.filter(release => release.tag_name === tag);
  assert(matching.length <= 1, 'Multiple releases exist for this tag; resolve manually');
  let release = matching[0];
  async function requireDraft() {
    const { data } = await github.rest.repos.getRelease({ ...repo, release_id: release.id });
    assert(data.draft, 'Refusing to modify an already published release');
    assert.equal(data.tag_name, tag, 'Release tag changed during this run');
    return data;
  }
  const details = {
    ...repo, tag_name: tag, target_commitish: context.sha,
    name: `Clanker ${tag} - unsigned public-alpha preview`,
    draft: true, prerelease: true,
    body: [
      '## Unsigned public-alpha preview',
      '',
      'Standalone native Windows x86_64 and macOS 15+ arm64/x86_64 archives.',
      'These binaries are unsigned; macOS binaries are also unnotarized.',
      'SHA256 files detect corruption, not publisher identity or authenticity.',
      '',
      'All native CI builds, offline tests and isolated package --help checks passed in this workflow run.',
      'This does not qualify interactive terminal behavior, HTTPS, or every supported OS version.',
      'Review docs/RELEASING.md, source attribution, license and private-data/history checks before manual publication.',
      'No history-wide credential clearance is asserted by this automation.',
      'Wait until this workflow has finished before editing or publishing this draft.',
      '',
      `Source commit: ${context.sha}`,
      `Build: ${context.serverUrl}/${repo.owner}/${repo.repo}/actions/runs/${context.runId}`,
    ].join('\n'),
  };
  if (release) {
    await requireDraft();
    const existing = await github.paginate(github.rest.repos.listReleaseAssets,
      { ...repo, release_id: release.id, per_page: 100 });
    assert(existing.every(asset => files.includes(asset.name)),
      'Draft contains unexpected assets; review them manually before rerunning');
    await requireDraft();
    await github.rest.repos.updateRelease({ ...details, release_id: release.id });
    for (const asset of existing) {
      await requireDraft();
      await github.rest.repos.deleteReleaseAsset({ ...repo, asset_id: asset.id });
    }
  } else {
    ({ data: release } = await github.rest.repos.createRelease(details));
    assert(release.draft, 'GitHub did not create a draft');
  }
  for (const name of files) {
    await requireDraft();
    const data = fs.readFileSync(path.join(directory, name));
    await github.rest.repos.uploadReleaseAsset({
      ...repo, release_id: release.id, name, data,
      headers: { 'content-type': name.endsWith('.sha256') ? 'text/plain' : 'application/octet-stream',
        'content-length': data.length },
    });
  }
  await requireDraft();
  const uploaded = await github.paginate(github.rest.repos.listReleaseAssets,
    { ...repo, release_id: release.id, per_page: 100 });
  assert.deepEqual(uploaded.map(asset => asset.name).sort(), files, 'Draft asset set is incomplete');
  for (const asset of uploaded) {
    assert.equal(asset.size, fs.statSync(path.join(directory, asset.name)).size, `Uploaded size mismatch: ${asset.name}`);
  }
  await requireSourceTag();
  core.info(`Prepared draft ${tag}; publication is a separate manual decision.`);
};
