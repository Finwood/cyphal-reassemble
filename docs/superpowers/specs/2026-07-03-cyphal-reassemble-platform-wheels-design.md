# cyphal-reassemble Platform Wheels — Architecture

**Status:** Approved (2026-07-03)
**Repo:** `cyphal-reassemble` (same repo as C++ executable + Python wrapper)
**Depends on:**
- `docs/superpowers/specs/2026-07-03-cyphal-reassemble-design.md` (C++ IPC contract)
- `docs/superpowers/specs/2026-07-03-cyphal-reassemble-python-wrapper-design.md` (Python wrapper, Phase 1 — merged in PR #2)

**Supersedes:** Phase 2 bullet in the Python wrapper spec (“CI-built manylinux wheels with binary in `_bin/`”).

---

## Background

PR #2 shipped the Python wrapper (`cyphal_reassemble`) as a **pure-Python** installable
package. Runtime resolution already supports a bundled binary slot:

```
CYPHAL_REASSEMBLE_BIN → py/cyphal_reassemble/_bin/ → PATH → repo build/
```

Today `uv build` produces a `py3-none-any` wheel containing **no executable**. Consumers must
build the C++ binary locally (`make`) or point `CYPHAL_REASSEMBLE_BIN` at an external build.

The frame-decoding pipeline and other downstream batch jobs need **`pip install` (or
`uv add`) to “just work”** on supported platforms without a compiler or system `libarrow-dev`.

This document specifies **Option 1**: ship **platform-specific wheels** that bundle the
`cyphal-reassemble` executable **and its shared-library dependencies** (notably `libarrow`),
repaired for portability with **auditwheel** (Linux) / **delocate** (macOS).

---

## Goals

- Publish installable wheels where `import cyphal_reassemble; reassemble(...)` works without a
  prior local C++ build on supported platforms.
- Bundle the existing CLI binary unchanged (same Arrow IPC stdin/stdout contract).
- Reuse the `_bin/` resolution slot already wired in `_backend.py` — no public API changes.
- Build wheels reproducibly in CI (GitHub Actions + cibuildwheel).
- Keep local dev workflow unchanged: `make` + `uv sync` + editable install with repo `build/`
  fallback.

## Non-goals

- Static linking / self-contained single-file executables (Option 3 — explicitly rejected).
- Windows wheels (defer until there is a concrete consumer; MSVC + Arrow C++ is a separate
  project).
- PyPI `py3-none-any` wheels (the binary is inherently platform-specific).
- Changing the subprocess IPC boundary or public Python API.
- Bundling or pinning **pyarrow** inside the wheel beyond the existing runtime dependency
  (Python and C++ Arrow remain independent at the IPC wire format).
- Split-package layout (`cyphal-reassemble` + `cyphal-reassemble-bin`) unless platform-matrix
  complexity later forces it.

---

## Design decisions

| Decision | Choice | Rationale |
| --- | --- | --- |
| Bundling strategy | Shared exe + vendored `.so` via auditwheel | Matches current `Arrow::arrow_shared` CMake link; minimal C++ change |
| Wheel layout | `cyphal_reassemble/_bin/` tree | Already second in `resolve_binary()`; one directory holds exe + repaired libs |
| Wheel tag | `py3-none-<platform>` | Python code is pure; only the bundled binary is platform-specific |
| Build orchestration | **cibuildwheel** in GitHub Actions | Standard matrix (manylinux / macOS), hooks for compile + repair |
| Build backend | **hatchling** (replace `uv_build` for releases) | `force-include` / artifacts for `_bin/`; `uv_build` has no custom hook story |
| Dev builds | Keep `make` + shared system Arrow | Fast iteration; no auditwheel in dev loop |
| Unsupported platforms | Git clone + `make` | No sdist published; full C++ source only in git |
| Binary RPATH | `$ORIGIN` (set by auditwheel repair) | `.so` files colocated with exe under `_bin/` |
| Versioning | Single package version (wrapper + binary lockstep) | Same repo, same tag; avoids ABI skew between Python facade and CLI |
| Publish target | **Public PyPI** via GitHub Actions **trusted publishing** | Standard install UX; no long-lived PyPI tokens in secrets |
| P0 platform | **`manylinux_2_28_x86_64` only** | Matches current pipeline; aarch64 / macOS deferred |
| Wheel ABI tag | **`py3-none`** (one wheel per platform, all Python 3.10–3.14) | Python code is pure; bundled binary is package data, not a `cp312` extension |
| Release cadence | Tag-driven publish on semver tags | `v*` tag → build wheels → trusted publish to PyPI |

---

## Architecture overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Consumer (frame-decoding pipeline, notebooks, scripts)                 │
│    pip install cyphal-reassemble   # platform wheel                     │
│    reassemble(frames_table)                                             │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
┌───────────────────────────────▼─────────────────────────────────────────┐
│  cyphal_reassemble (pure Python — unchanged from PR #2)                 │
│    reassemble.py  →  validate → stream IPC to subprocess                │
│    _backend.py    →  resolve_binary() → Popen                           │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │ subprocess, Arrow IPC stream
┌───────────────────────────────▼─────────────────────────────────────────┐
│  site-packages/cyphal_reassemble/_bin/   (platform wheel payload)       │
│    cyphal-reassemble          ← auditwheel-repaired executable          │
│    libarrow.so.* (+ deps)     ← vendored next to exe, RPATH=$ORIGIN     │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
┌───────────────────────────────▼─────────────────────────────────────────┐
│  C++ core (unchanged logic)                                             │
│    libcanard (static) + libarrow (shared, bundled at wheel build time)  │
└─────────────────────────────────────────────────────────────────────────┘
```

### Resolution order (unchanged)

1. `CYPHAL_REASSEMBLE_BIN` — override for debugging / custom builds
2. **`importlib.resources` path / `_bin/cyphal-reassemble`** — populated by platform wheel
3. `shutil.which("cyphal-reassemble")` — system install
4. `<repo>/build/cyphal-reassemble` — editable dev fallback

After wheel install, step 2 satisfies normal use. Step 4 remains for `uv sync` editable installs
from a git clone.

---

## Wheel contents

### Installed layout (example Linux x86_64)

```
site-packages/
└── cyphal_reassemble/
    ├── __init__.py
    ├── schema.py
    ├── reassemble.py
    ├── _backend.py
    └── _bin/
        ├── cyphal-reassemble          # ELF executable
        ├── libarrow.so.1800           # version varies with Arrow apt pin
        ├── libarrow.so -> libarrow.so.1800
        └── …                          # other auditwheel-pulled deps if any
```

`_bin/` is **gitignored** in the source tree (never committed). It exists only as a CI
build artifact staged immediately before `hatchling` / wheel assembly.

### Wheel filename (example)

```
cyphal_reassemble-0.1.0-py3-none-manylinux_2_28_x86_64.whl
```

#### What the filename tags mean

Every wheel name follows `{name}-{version}-{python tag}-{abi tag}-{platform tag}.whl`:

| Tag | Example | Meaning |
| --- | --- | --- |
| **python tag** | `py3` | Any CPython 3.x interpreter (3.10–3.14) |
| **abi tag** | `none` | No interpreter-specific native **Python extension** (`.so` importable as a module) |
| **platform tag** | `manylinux_2_28_x86_64` | Linux x86_64 with glibc ≥ manylinux_2_28 floor |

**Our choice: `py3-none-manylinux_2_28_x86_64`** — one wheel per Linux x86_64 platform, installable
on Python 3.10 through 3.14. The bundled `cyphal-reassemble` executable is **package data** under
`_bin/`, not a Python C extension, so it does not need a per-version tag like `cp312`.

**Alternative (not chosen):** cibuildwheel’s default emits **one wheel per Python version**,
e.g. `cp312-cp312-manylinux_2_28_x86_64`, `cp313-cp313-…`. Those wheels would contain
identical `_bin/` trees but pip only accepts the wheel whose `cp312` tag matches the running
interpreter. That multiplies CI time and PyPI uploads without benefit for this package.

---

## Build pipeline

High-level CI flow per platform job:

```
checkout (+ submodules)
    → install Arrow C++ + build tools (same apt source as existing CI)
    → cmake/make Release → build/cyphal-reassemble
    → auditwheel show build/cyphal-reassemble   (diagnostics)
    → auditwheel repair -w _wheelhouse/ build/cyphal-reassemble
    → stage: _wheelhouse/cyphal-reassemble/*  →  py/cyphal_reassemble/_bin/
    → hatchling build  →  dist/*.whl
    → smoke test: pip install dist/*.whl && uv run pytest python_tests/
    → upload artifact / publish to PyPI (on tag)
```

### cibuildwheel integration

Add `.github/workflows/wheels.yml` (or extend CI with a `release` job) driven by
**cibuildwheel**:

| Setting | Value (resolved) |
| --- | --- |
| `CIBW_BUILD` | `cp312-manylinux_x86_64` (single build job; wheel tagged `py3-none`) |
| Linux image | `manylinux_2_28` |
| Linux arch | **`x86_64` only** (v1) |
| macOS / aarch64 | **Deferred** |
| `before-build` | Install Arrow apt repo + `make` (reuse existing CI snippet) |
| `repair-wheel-command` | N/A — repair happens **before** Python packaging (see below) |

**Why repair before hatchling, not after:** auditwheel operates on ELF binaries. The Python
wheel build step only **copies an already-repaired tree** into `_bin/`. This avoids fighting
hatchling’s internal wheel layout and keeps repair logic in one shell script.

Proposed script: `scripts/prepare_wheel_bundle.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail
make
mkdir -p py/cyphal_reassemble/_bin
auditwheel repair -w /tmp/wheelhouse build/cyphal-reassemble
cp -a /tmp/wheelhouse/cyphal-reassemble/* py/cyphal_reassemble/_bin/
```

macOS equivalent: `delocate`-based script in `scripts/prepare_wheel_bundle_macos.sh`.

### Build backend change

Release builds switch from `uv_build` to **hatchling** with explicit inclusion of `_bin/`:

```toml
[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["py/cyphal_reassemble"]

[tool.hatch.build.targets.wheel.force-include]
"py/cyphal_reassemble/_bin" = "cyphal_reassemble/_bin"
```

**Dev workflow stays uv-native:** `uv sync` continues to install the package editable from
`py/`; only the release / cibuildwheel path runs `prepare_wheel_bundle.sh` before building.

---

## Platform matrix (phased)

| Phase | Platforms | Status |
| --- | --- | --- |
| **P0 (v1)** | `manylinux_2_28_x86_64` | **Approved** — frame-decoding pipeline |
| **P1** | `manylinux_2_28_aarch64` | Deferred |
| **P2** | `macosx_*` | Deferred |
| **—** | Windows | Out of scope until requested |

v1 ships **one wheel** per release: `py3-none-manylinux_2_28_x86_64`. PyPI serves it to
matching `pip install` clients on Linux x86_64.

---

## Arrow / libarrow coupling

The C++ binary is built against the **Arrow C++ version provided by the CI image** (today:
Apache Arrow apt repository on Ubuntu / manylinux). auditwheel copies the linked
`libarrow.so.*` (and transitive deps) into `_bin/`.

Implications:

- **Wheel-local Arrow ≠ user’s pyarrow version** — acceptable because IPC is a wire format
  between processes; the Python wrapper serializes with pyarrow, the binary deserializes with
  bundled libarrow.
- **Arrow apt pin drift** — if CI’s Arrow version jumps, rebuilt wheels bundle the new `.so`;
  golden tests in CI catch IPC regressions before publish.
- **manylinux glibc floor** — cibuildwheel’s manylinux_2_28 image defines minimum glibc for
  Linux wheels; do not build release binaries on a bare `ubuntu-latest` runner without
  cibuildwheel’s container.

Optional hardening (later): record `arrow --version` in wheel metadata or `cyphal_reassemble
__version_arrow__` for support diagnostics.

---

## Release workflow

```
tag v0.1.0 on main
    → GitHub Actions: wheels.yml
    → cibuildwheel builds manylinux_2_28_x86_64 wheel + smoke tests
    → upload to GitHub Release artifacts (optional)
    → publish to PyPI via trusted publishing (OIDC; no API token)
```

Configure PyPI trusted publishing once before the first release — see
**[PyPI trusted publishing setup (repo owner)](#pypi-trusted-publishing-setup-repo-owner)** below.

Version bump in `pyproject.toml` remains the single source of truth. C++ and Python share
that version; no separate `-bin` package version.

### PyPI trusted publishing setup (repo owner)

One-time manual setup. Do this **after** `.github/workflows/wheels.yml` is merged to `main`
(the workflow filename must exist on the default branch before PyPI will accept uploads).

#### 1. Confirm package name

The PyPI project name must match `pyproject.toml`:

```toml
[project]
name = "cyphal-reassemble"
```

Trusted publishing binds to this exact name.

#### 2. Create the PyPI project (if it does not exist yet)

**Option A — pending publisher (recommended for first release):** skip creating the project
manually; step 3 registers a *pending* trusted publisher and the **first successful workflow
upload creates the project**.

**Option B — create empty project first:**

1. Log in at [pypi.org](https://pypi.org).
2. Open [Register](https://pypi.org/account/register/) if you need an account (enable 2FA — PyPI requires it for publishing).
3. After login, you do **not** need to upload manually; the project appears on first trusted publish.

#### 3. Add the trusted publisher on PyPI

1. Open **[Account settings → Publishing](https://pypi.org/manage/account/publishing/)** (for a *new* project)  
   **or** your project → **Manage** → **Publishing** (if `cyphal-reassemble` already exists).
2. Click **Add a new pending publisher** (new project) or **Add trusted publisher** (existing).
3. Fill in **exactly**:

| Field | Value |
| --- | --- |
| PyPI project name | `cyphal-reassemble` |
| Owner | `Finwood` |
| Repository name | `cyphal-reassemble` |
| Workflow name | `wheels.yml` |
| Environment name | `pypi` |

4. Save.

The **Owner**, **Repository**, **Workflow name**, and **Environment name** must match GitHub
exactly. The workflow file is `.github/workflows/wheels.yml` → workflow name `wheels.yml`.

Official reference: [PyPI trusted publishers](https://docs.pypi.org/trusted-publishers/).

#### 4. Create the matching GitHub environment

1. Open `https://github.com/Finwood/cyphal-reassemble/settings/environments`.
2. **New environment** → name it `pypi` (must match PyPI step 3).
3. **Deployment branches:** restrict to `main` (or your release branch) if you want — recommended.
4. **Required reviewers:** optional; leave empty for fully automated tag releases, or add yourself for a manual approval gate before PyPI upload.
5. Save.

The workflow job uses `environment: pypi`; GitHub only mints the OIDC token for trusted publishing
when this environment exists and the run is allowed to use it.

#### 5. Verify workflow permissions

In `.github/workflows/wheels.yml` the job needs:

```yaml
permissions:
  id-token: write   # required for OIDC → PyPI

jobs:
  build-and-publish:
    environment: pypi
    ...
      - uses: pypa/gh-action-pypi-publish@release/v1
```

No `PYPI_API_TOKEN` secret is required — that is the point of trusted publishing.

If the repo uses restricted default workflow permissions: **Settings → Actions → General →
Workflow permissions → Read and write** is not required, but **Read repository contents** plus
allow **`id-token: write`** at workflow level (as above) is.

#### 6. Dry-run on TestPyPI (optional but recommended)

Repeat step 3 on **[TestPyPI](https://test.pypi.org/manage/account/publishing/)** with the same
GitHub fields but project name e.g. `cyphal-reassemble` on test.pypi.org.

Add a second publish step (or separate workflow) temporarily:

```yaml
- uses: pypa/gh-action-pypi-publish@release/v1
  with:
    repository-url: https://test.pypi.org/legacy/
    packages-dir: wheelhouse/
```

Run via **Actions → Wheels → Run workflow**. Then:

```bash
pip install --index-url https://test.pypi.org/simple/ cyphal-reassemble
```

Remove or disable TestPyPI publish before the real `v*` tag.

#### 7. First production release

After the workflow is on `main` and steps 3–4 are done:

```bash
# version in pyproject.toml is already 0.1.0
git tag v0.1.0
git push origin v0.1.0
```

Watch **Actions → Wheels**. On success:

```bash
pip install cyphal-reassemble==0.1.0
python -c "from cyphal_reassemble import resolve_binary; print(resolve_binary())"
```

Expected: path under `.../site-packages/cyphal_reassemble/_bin/cyphal-reassemble`.

#### Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `invalid-publisher` | Typo in owner/repo/workflow/environment; or workflow not on default branch yet |
| `Environment `pypi` not found` | GitHub environment not created (step 4) |
| `Resource not accessible by integration` | Workflow missing `id-token: write` |
| Upload ok but `pip install` finds no wheel | Platform mismatch — v1 wheel is Linux x86_64 only |
| Token / permission errors | Do not use API tokens; fix trusted publisher config instead |


### Consumer install (target UX)

```bash
uv add cyphal-reassemble        # or: pip install cyphal-reassemble
# no `make`, no CYPHAL_REASSEMBLE_BIN, no libarrow-dev
python -c "from cyphal_reassemble import reassemble; print(reassemble)"
```

Unsupported platform: no matching wheel on PyPI — clone the repo with submodules,
build locally (`make` + `uv sync`), or `pip install` fails with a clear “no wheel for
your platform” message.

---

## Testing strategy

| Layer | What | Where |
| --- | --- | --- |
| C++ unit + golden | Existing `make test` | Every push (unchanged) |
| Python wrapper | `uv run pytest python_tests/` against repo `build/` | Every push (unchanged) |
| **Wheel smoke** | `pip install dist/*.whl` in clean venv → pytest without repo `build/` | cibuildwheel `test-command` |
| **Bundled resolution** | Assert `resolve_binary()` points under `site-packages/.../_bin/` | New test in `python_tests/test_backend.py` |
| **ldd / load check** | `_bin/cyphal-reassemble` runs `--help`; no missing `.so` | CI script after repair |

Wheel smoke tests must **unset** `CYPHAL_REASSEMBLE_BIN` and ensure `build/cyphal-reassemble`
is not on PATH so `_bin/` is exercised.

---

## Repository changes (summary)

| Path | Change |
| --- | --- |
| `docs/superpowers/specs/2026-07-03-cyphal-reassemble-platform-wheels-design.md` | This document |
| `scripts/prepare_wheel_bundle.sh` | Build + auditwheel + stage into `_bin/` |
| `pyproject.toml` | hatchling build backend; `[tool.hatch.build.*]` |
| `.gitignore` | `py/cyphal_reassemble/_bin/` |
| `.github/workflows/wheels.yml` | cibuildwheel release job |
| `python_tests/test_backend.py` | Bundled-binary resolution test (wheel job) |
| `README.md` | Install from PyPI / platform requirements |
| Python wrapper spec | Mark Phase 2 complete; link here |

No C++ source changes required for P0.

---

## Resolved decisions (2026-07-03)

| # | Decision | Resolution |
| --- | --- | --- |
| 1 | Publish target | **Public PyPI** with **trusted publishing** from GitHub Actions |
| 2 | P0 platform | **`manylinux_2_28_x86_64` only** |
| 3 | macOS / aarch64 | **Deferred** (not in v1) |
| 4 | Wheel ABI tag | **`py3-none`** — one wheel per platform for all supported Python 3.x (see above) |
| 5 | Release cadence | **Tag-driven** — semver tag triggers build + PyPI publish |

---

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| auditwheel misses a dependency | `auditwheel show` + `ldd` gate in CI; smoke `--help` and golden pytest |
| Arrow apt breaking change | Pin apt source version or manylinux image digest; golden tests |
| Large wheel size (libarrow) | Accept for v1; document size; optional strip/debug split later |
| `_bin/` accidentally committed | `.gitignore` + CI checks no ELF in git tree |
| hatchling / uv dev drift | Document: dev uses `uv sync`; release uses hatchling via cibuildwheel only |
| Unsupported platform install | Clear README fallback: clone, `make`, editable install |

---

## Relationship to prior specs

- **C++ design spec** — IPC contract unchanged; this adds a deployment artifact.
- **Python wrapper spec (Phase 1)** — API and `_backend.py` resolution unchanged; Phase 2
  deferred item is now specified here.
- **Implementation plan** — `docs/superpowers/plans/2026-07-03-cyphal-reassemble-platform-wheels.md`

---

## Implementation status

| Phase | Scope | Status |
| --- | --- | --- |
| Spec | This document | **Approved** |
| Plan | Task breakdown | See implementation plan |
| P0 | `manylinux_2_28_x86_64` wheel + PyPI trusted publishing | Not started |
| P1+ | aarch64, macOS | Deferred |
