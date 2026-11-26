# Development, release and publishing

How to change this plugin, verify it, and ship a version.

> Every command here was run on this repository. If one stops working, fix the
> document — a runbook that lies is worse than none.

---

## 1. What this package is

A **DSH bundle**: an npm package whose `package.json` declares `dsh.bundle`, so
installing it adds a configuration layer to a DSH profile. This one connects a
FoloToy AI Passport (ESP32-C3) device over BLE.

```
package.json       name, exports, dsh.bundle / dsh.client, engines.dsh
cordis.patch.yml   the layer: one row plus its default config
lib/               host half (hand-written, no build step)
lib/client.js      browser half (hand-written classic script, no build step)
lib/ble/           the BLE bridge: a subprocess worker plus its host transport
locale/{en,zh}.json  display title/description for the Plugins page
icon.svg           Plugins-page artwork
```

**There is no build step.** `lib/` is the source of truth and is committed
as-is, because the consumer's pnpm blocks dependency build scripts and a Git
install must carry runnable output.

The firmware lives elsewhere in this monorepo; this directory is only the npm
package half.

---

## 2. The dependency shape, and why it looks odd

**The package has no `dependencies` at all.** Two packages it can use are
declared as *optional peers*:

```json
"peerDependencies": {
  "@deepseek-ai/schemastery": ">=3.18.0",
  "@stoprocent/noble": "^2.8.0"
},
"peerDependenciesMeta": {
  "@deepseek-ai/schemastery": { "optional": true },
  "@stoprocent/noble": { "optional": true }
}
```

Both are genuinely optional, and both used to break installation:

| Package | Why optional is correct | What broke before |
|---|---|---|
| `@stoprocent/noble` | loaded **inside the BLE bridge subprocess**, and `bridge-worker.js` already falls back through `@stoprocent/noble` → `@abandonware/noble` → `noble`; the host half never imports it | It carried `install: node-gyp-build`. pnpm refuses to run dependency build scripts, **exits non-zero**, and DSH treats the whole install as failed — so the bundle was never appended to `dsh.profile.bundles` and the plugin never loaded |
| `@deepseek-ai/schemastery` | supplied by the hosting Harness; `loadConfigSchema()` catches the failed import and returns `null`, after which the settings form simply validates less | — |

**Do not move noble back into `dependencies`.** A plain dependency puts its
build script on the install path, which fails the install, and DSH only appends
the bundle after pnpm succeeds. A plain `optionalDependencies` entry does not
help either: pnpm still installs optional dependencies and still refuses their
scripts.

Worth knowing: noble ships prebuilt binaries for ten platforms, so it *loads*
fine even when its build script never runs. The failure was never a missing
binary — it was pnpm's exit code.

---

## 3. Everyday development

```bash
npm test      # node --test
npm pack --dry-run
```

**Run the tests on Node 24.** The package's `engines.node` says `>=20` because
that is what the runtime needs, but the *test suite* needs 24:

| Node | Result |
|---|---|
| 24 | 212 pass, 0 fail, 0 cancelled |
| 22 | 201 pass, 0 fail, **11 cancelled** |

The cancelled tests are all in `lib/ble/bridge-process.test.js`, the file that
drives real child processes. Node 22's test runner resolves the event loop while
those subtests are still waiting and cancels them
(`cancelledByParent` — "Promise resolution is still pending but the event loop
has already resolved"). The same sequence run outside `node --test` works, so it
is the runner, not the code.

Use Node 24 for the suite and for CI. The workflow pins it, and so should you.

### The suite reads the firmware sources

`test/protocol-parity.test.js` parses `vendor/ai-passport/main/app_proto.h` to
assert the plugin's constants still match the device firmware. A checkout
without `vendor/` fails with `ENOENT` — that is not a flaky test, it is a real
dependency on the submodule contents. Git tracks ~17 MB of it; the local build
output around it is not tracked and is not needed.

There is no `npm ci` step for the package itself: with zero dependencies there is
nothing to install. The workspace root does own a lockfile, so CI installs from
there.

### Editing the host half

`lib/*.js` is plain ESM. Keep the plugin shape intact: `export const name`,
optional `export const inject`, `export function apply`.

### Editing the browser half

`lib/client.js` must keep the shape the Harness module table expects:
`window.__ModuleLoader__.load({ id, factory })`, where **`id` must equal the
package name**. The test reads the expected id from `package.json` rather than
hardcoding it, so a rename cannot drift silently.

### Editing the bundle patch

`cordis.patch.yml` inserts one row and carries its default config. Two
identifiers, only one of which follows the package name:

| Field | Follows the package name? | Why |
|---|---|---|
| `name` | **yes** | Node resolves the module by package name |
| `id` | **no** | the profile override key; renaming orphans user overrides |

`name:` must be **quoted** when it starts with `@`: `@` is a YAML reserved
indicator and an unquoted value fails to parse, silently disabling the bundle.

The config defaults in that file are deliberate, not arbitrary. `autoConnect`
is `false` because starting the BLE bridge during host startup once left the
desktop app unable to answer RPC at all — see the comments in the file before
changing it.

### Editing display metadata

`locale/en.json` and `locale/zh.json` carry `meta.title` and
`meta.description`, read by the Plugins page **without activating the plugin**.
Icons: SVG/PNG/JPEG/WebP, under 256 KiB, inside the package.

---

## 4. What must stay consistent

**Module identifiers follow the package name. On-disk and on-wire state does
not.**

| Identifier | Scope | Why |
|---|---|---|
| package `name` | scoped | consumed by npm and Node |
| row `name` in the patch | scoped | resolved as a module |
| client bundle `id` | scoped | must equal the package name |
| row `id` in the patch | unscoped | profile override key |
| the BLE protocol version | unscoped | the firmware on the device speaks it |

---

## 5. Versioning and `engines.dsh`

**Prereleases need an explicit branch.** node-semver only lets a prerelease
satisfy a range when a comparator on the *same* `major.minor.patch` tuple carries
a prerelease tag:

```
>=0.2.0-rc.1 <0.3.0                            ✗ rejects 0.2.3-alpha.2
>=0.2.0-rc.1 <0.3.0 || >=0.2.3-alpha.2 <0.3.0  ✓
```

This package uses the second form. When `engines.dsh` is present it alone
decides compatibility — the optional peer ranges above are ignored by that
check, which is why adding them did not make this package incompatible.

---

## 6. Release checklist

```bash
npm test
npm pack --dry-run          # expect 30 files
```

Confirm the pack list has `lib/` (including `lib/ble/`), `cordis.patch.yml`,
`locale/{en,zh}.json`, `icon.svg`, `README.md`, `LICENSE` — and **no**
`*.test.js`, no `test/`, no credentials. Tests are excluded through a negative
`files` entry (`"!lib/**/*.test.js"`) rather than a `prepack` script, because a
`prepack` that deletes files also fires on `npm pack --dry-run` and would eat
the working copy.

Then bump, commit, tag:

```bash
npm version patch --no-git-tag-version
# ← also update the version the browser half reports, see below
git add -A && git commit -m "release: v2.0.1"
git tag v2.0.1
git push origin main --tags
```

**The browser half carries its own copy of the version.** `lib/client.js` has

```js
const VERSION = '2.0.1'
```

and a test asserts it equals `package.json` `version`. There is no build step
and no generator, so a bump has to touch both — miss it and the suite fails with
`客户端 VERSION 必须与 package.json 一致`, which at least names the problem.

**The tag must equal `package.json` `version`**; the workflow refuses to publish
on a mismatch. Note the tag lives in the monorepo root, and the publish workflow
must `cd packages/dsh-ai-passport` before building or publishing.

---

## 7. Installing

```bash
dsh plugin --profile desktop add @alfredhuang211/dsh-ai-passport
```

One command, one pass: the plugin installs and is enabled with nothing else
pulled in.

For BLE, add noble to the same profile:

```bash
dsh plugin --profile desktop add @stoprocent/noble
```

This second command **will** report `ERR_PNPM_IGNORED_BUILDS` on a fresh
profile. Approve the listed packages in the profile's `pnpm-workspace.yaml`
(`true` instead of the `set this to true or false` placeholder) and run it once
more. The plugin stays enabled throughout — unlike before, a failed noble
install no longer means a failed plugin install.

Noble ships prebuilds, so it loads even without running its script; approving is
only needed to stop pnpm from failing the command.

**Verify with `--dump-config`**, which proves the layer loaded:

```bash
dsh --profile desktop --dump-config | grep '== @alfredhuang211/'
```

---

## 8. Automated publishing (OIDC)

Publishing uses npm **trusted publishing**: GitHub signs a short-lived OIDC
token, npm exchanges it for a publish token. **No `NPM_TOKEN` secret exists, and
none should be added.**

Two workflow settings are load-bearing, and both were discovered the hard way on
a sibling plugin:

**No `registry-url` on `setup-node`.** Setting it writes
`_authToken=${NODE_AUTH_TOKEN}` into the runner's `.npmrc` and exports
`GITHUB_TOKEN` in its place; npm then prefers that credential and never performs
the OIDC exchange. The symptom is a signed provenance statement followed by
`E404 Not Found - PUT`.

**Node 24, not 22.** OIDC needs npm ≥ 11.5.1; Node 22 ships npm 10.x.

### `repository` must point here

`--provenance` signs the tarball and the registry cross-checks the claim against
the workflow that built it. `package.json` therefore needs:

```json
"repository": {
  "type": "git",
  "url": "git+https://github.com/alfredhuang211/dsh-ai-passport.git",
  "directory": "packages/dsh-ai-passport"
}
```

`directory` matters because the package sits below the repository root. Without
the field the publish is rejected with `E422 ... "repository.url" is ""` — after
the provenance statement was already signed, so the log looks like it got further
than it did.

### Requirements that are not in this repository

| Field | Value |
|---|---|
| Publisher | GitHub Actions |
| Organization or user | `alfredhuang211` |
| Repository | `dsh-ai-passport` |
| Workflow filename | the monorepo workflow's own filename |
| Environment name | empty |
| Allowed actions | **must include "Allow npm publish"** |

---

## 9. When things fail

| Symptom | Cause |
|---|---|
| `ERR_PNPM_IGNORED_BUILDS` on `add @alfredhuang211/dsh-ai-passport` | noble is back in `dependencies`. See §2. |
| Plugin installed but never appears / not in `dsh.profile.bundles` | the install exited non-zero after writing the dependency but before the bundle was appended. Re-read §2. |
| `E422 ... Failed to validate repository information: "repository.url" is ""` | provenance needs `package.json` `repository.url` to match the repo the workflow ran in. A monorepo subpackage also needs `repository.directory`. |
| `E404 Not Found - PUT` **and** provenance signed fine | a static credential suppressed OIDC (§8) |
| `ENEEDAUTH` / `Unable to authenticate` | trusted-publisher config mismatch |
| `You cannot publish over the previously published versions` | **authentication worked**; the version exists. Bump it. |
| BLE panel shows noble not loaded | noble is not installed in this profile. §7. |
| `客户端 VERSION 必须与 package.json 一致` | you bumped `package.json` but not `lib/client.js` `VERSION`. §6. |
| `cancelledByParent` on many bridge tests | you are on Node 22. Use Node 24. §3. |
| `ENOENT ... vendor/ai-passport/main/app_proto.h` | the checkout has no `vendor/`; the parity test needs it. §3. |
