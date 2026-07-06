# cyphal-reassemble Platform Wheels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish a **platform wheel** (`py3-none-manylinux_2_28_x86_64`) to **public PyPI** so `pip install cyphal-reassemble` works on Linux x86_64 without a local C++ build. The wheel bundles the `cyphal-reassemble` executable and auditwheel-repaired `libarrow` shared libraries under `cyphal_reassemble/_bin/`.

**Architecture:** Reuse PR #2's subprocess wrapper unchanged. Release CI runs cibuildwheel inside `manylinux_2_28`: `make` → `auditwheel repair` → stage into `py/cyphal_reassemble/_bin/` → hatchling wheel → retag to `py3-none` → smoke pytest → trusted publish to PyPI on semver tags.

**Tech Stack:** cibuildwheel, auditwheel, hatchling (release builds), uv (dev + lockfile), GitHub Actions OIDC trusted publishing, existing CMake/Makefile C++ build, Apache Arrow apt repo (same as CI).

**Design spec:** `docs/superpowers/specs/2026-07-03-cyphal-reassemble-platform-wheels-design.md` (Approved)

**Prerequisites for the implementing agent:**

```bash
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
make && make test              # C++ must pass
uv sync && uv run pytest python_tests/ -v   # Python wrapper must pass
```

Manual one-time setup (repo owner — **you**):

→ Follow **[PyPI trusted publishing setup (repo owner)](../../specs/2026-07-03-cyphal-reassemble-platform-wheels-design.md#pypi-trusted-publishing-setup-repo-owner)** in the design spec (steps 1–7: PyPI publisher, GitHub `pypi` environment, dry-run, first tag).

Summary checklist:

- [ ] `wheels.yml` merged to `main`
- [ ] PyPI trusted publisher: `Finwood` / `cyphal-reassemble` / `wheels.yml` / environment `pypi`
- [ ] GitHub environment **`pypi`** created on the repo
- [ ] Optional: TestPyPI pending publisher + manual `workflow_dispatch`
- [ ] First tag `v0.1.0` → verify `pip install cyphal-reassemble`

---

## File map

| Path | Action | Purpose |
| --- | --- | --- |
| `scripts/prepare_wheel_bundle.sh` | Create | Build C++, auditwheel repair, stage `_bin/` |
| `scripts/build_wheel.sh` | Create | Bundle + hatchling + `py3-none` retag |
| `pyproject.toml` | Modify | hatchling build backend + `[tool.hatch.build.*]` |
| `.gitignore` | Modify | Ignore `py/cyphal_reassemble/_bin/`, `dist/` |
| `py/cyphal_reassemble/_bin/.gitkeep` | Optional | Document slot (gitignored contents) |
| `.github/workflows/wheels.yml` | Create | Tag-triggered cibuildwheel + PyPI publish |
| `python_tests/test_backend.py` | Modify | Bundled-binary resolution test |
| `python_tests/conftest.py` | Modify | `bundled_only` skip helper for wheel smoke |
| `Makefile` | Modify | `wheel-test` / `wheel-build` convenience targets |
| `README.md` | Modify | PyPI install + platform notes |
| `docs/superpowers/specs/2026-07-03-cyphal-reassemble-python-wrapper-design.md` | Modify | Mark Phase 2 platform wheels as planned |

C++ sources unchanged.

---

## Task 1: Gitignore and bundle staging directory

**Files:**
- Modify: `.gitignore`

- [ ] **Step 1: Ignore staged wheel artifacts**

Append to `.gitignore`:

```gitignore
# Platform wheel build artifacts (never commit)
py/cyphal_reassemble/_bin/
dist/
wheelhouse/
```

- [ ] **Step 2: Commit**

```bash
git add .gitignore
git commit -m "chore: gitignore wheel bundle staging directory"
```

---

## Task 2: `prepare_wheel_bundle.sh`

**Files:**
- Create: `scripts/prepare_wheel_bundle.sh`

- [ ] **Step 1: Write the script**

Create `scripts/prepare_wheel_bundle.sh`:

```bash
#!/usr/bin/env bash
# Build cyphal-reassemble and stage an auditwheel-repaired bundle into py/cyphal_reassemble/_bin/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGING="${ROOT}/py/cyphal_reassemble/_bin"
WHEELHOUSE="${ROOT}/wheelhouse"

cd "${ROOT}"

make

if ! command -v auditwheel >/dev/null 2>&1; then
  echo "auditwheel not found; install with: pip install auditwheel" >&2
  exit 1
fi

rm -rf "${STAGING}" "${WHEELHOUSE}"
mkdir -p "${STAGING}" "${WHEELHOUSE}"

echo "==> auditwheel show"
auditwheel show "${ROOT}/build/cyphal-reassemble"

echo "==> auditwheel repair"
auditwheel repair -w "${WHEELHOUSE}" "${ROOT}/build/cyphal-reassemble"

BUNDLE_DIR="${WHEELHOUSE}/cyphal-reassemble"
if [[ ! -d "${BUNDLE_DIR}" ]]; then
  # auditwheel 5.x may flatten output; fall back to first subdirectory
  BUNDLE_DIR="$(find "${WHEELHOUSE}" -mindepth 1 -maxdepth 1 -type d | head -1)"
fi

cp -a "${BUNDLE_DIR}/." "${STAGING}/"
chmod +x "${STAGING}/cyphal-reassemble"

echo "==> staged bundle"
ls -la "${STAGING}"
ldd "${STAGING}/cyphal-reassemble"
"${STAGING}/cyphal-reassemble" --help
```

- [ ] **Step 2: Make executable**

```bash
chmod +x scripts/prepare_wheel_bundle.sh
```

- [ ] **Step 3: Smoke test locally (Linux with Arrow + auditwheel installed)**

Run on a Linux host with existing CI deps:

```bash
pip install auditwheel
./scripts/prepare_wheel_bundle.sh
test -x py/cyphal_reassemble/_bin/cyphal-reassemble
```

Expected: `--help` prints usage; `ldd` shows bundled `libarrow` via `$ORIGIN`, not system paths.

- [ ] **Step 4: Commit**

```bash
git add scripts/prepare_wheel_bundle.sh
git commit -m "feat: add auditwheel bundle staging script for platform wheels"
```

---

## Task 3: Switch release build backend to hatchling

**Files:**
- Modify: `pyproject.toml`
- Create: `scripts/build_wheel.sh`

- [ ] **Step 1: Update `pyproject.toml` build-system**

Replace the `[build-system]` block and remove `[tool.uv.build-backend]` (hatchling takes over packaging; uv still manages the dev venv):

```toml
[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
sources = ["py"]
ignore-vcs = true
```

Keep `[project]`, `[dependency-groups]`, `[tool.pytest.*]`, `[tool.ruff.*]` unchanged.

**Note:** `uv sync` continues to work for editable dev installs after this change. Re-run `uv sync` locally to verify.

- [ ] **Step 2: Write `scripts/build_wheel.sh`**

```bash
#!/usr/bin/env bash
# Build a py3-none-manylinux platform wheel (run inside cibuildwheel manylinux container).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

./scripts/prepare_wheel_bundle.sh

pip install build wheel hatchling
python -m build --wheel --outdir dist/

# cibuildwheel builds with one cp312 interpreter; retag to py3-none for all Python 3.x.
WHEEL=(dist/cyphal_reassemble-*.whl)
wheel tags --remove --python-tag=py3 --abi-tag=none "${WHEEL[@]}"
wheel tags --platform-tag=manylinux_2_28_x86_64 "${WHEEL[@]}"

echo "==> built wheel"
ls -la dist/
unzip -l dist/*.whl | grep '_bin/cyphal-reassemble'
```

- [ ] **Step 3: Verify editable dev still works**

```bash
uv sync
uv run pytest python_tests/test_schema.py -v
```

Expected: PASS (uses repo `build/` fallback when `_bin/` absent).

- [ ] **Step 4: Commit**

```bash
chmod +x scripts/build_wheel.sh
git add pyproject.toml scripts/build_wheel.sh
git commit -m "feat: hatchling wheel build with _bin force-include"
```

---

## Task 4: Wheel smoke tests

**Files:**
- Modify: `python_tests/conftest.py`
- Modify: `python_tests/test_backend.py`

- [ ] **Step 1: Add bundled-binary test**

Append to `python_tests/test_backend.py`:

```python
def test_resolve_binary_prefers_bundled_over_repo_build(monkeypatch):
    """When _bin/ is populated (platform wheel), it wins over repo build/."""
    monkeypatch.delenv("CYPHAL_REASSEMBLE_BIN", raising=False)
    bundled = Path(__file__).resolve().parents[1] / "py" / "cyphal_reassemble" / "_bin" / "cyphal-reassemble"
    if not bundled.is_file():
        pytest.skip("bundled binary not staged; run scripts/prepare_wheel_bundle.sh")
    resolved = resolve_binary()
    assert resolved == bundled
    assert "_bin" in str(resolved)
```

- [ ] **Step 2: Add wheel-smoke marker in conftest**

Append to `python_tests/conftest.py`:

```python
@pytest.fixture(scope="session")
def require_bundled_binary() -> Path:
    """Skip unless _bin/cyphal-reassemble exists (wheel install or staged bundle)."""
    bundled = Path(__file__).resolve().parents[1] / "py" / "cyphal_reassemble" / "_bin" / "cyphal-reassemble"
    if not bundled.is_file():
        pytest.skip("bundled binary not present")
    return bundled
```

- [ ] **Step 3: Run tests with staged bundle**

```bash
./scripts/prepare_wheel_bundle.sh
uv run pytest python_tests/test_backend.py -v
uv run pytest python_tests/test_reassemble.py -v
```

Expected: all PASS including golden tests via bundled binary.

- [ ] **Step 4: Commit**

```bash
git add python_tests/conftest.py python_tests/test_backend.py
git commit -m "test: cover bundled _bin binary resolution"
```

---

## Task 5: cibuildwheel + PyPI trusted publishing workflow

**Files:**
- Create: `.github/workflows/wheels.yml`

- [ ] **Step 1: Create workflow**

Create `.github/workflows/wheels.yml`:

```yaml
name: Wheels

on:
  push:
    tags:
      - "v*"
  workflow_dispatch:

permissions:
  contents: read
  id-token: write   # OIDC for PyPI trusted publishing

jobs:
  build-and-publish:
    runs-on: ubuntu-latest
    environment: pypi
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - uses: pypa/cibuildwheel@v2.23.2
        env:
          CIBW_BUILD: cp312-manylinux_x86_64
          CIBW_ARCHS_LINUX: x86_64
          CIBW_BEFORE_BUILD: |
            yum install -y wget
            # Arrow C++ (manylinux_2_28 / AlmaLinux-based image)
            wget -q https://apache.jfrog.io/artifactory/arrow/almalinux/apache-arrow-apt-source-latest-alma-9.rpm || \
              wget -q https://apache.jfrog.io/artifactory/arrow/centos/8/apache-arrow-apt-source-latest-el-8.noarch.rpm
            # Fallback: install from existing CI pattern adapted for manylinux — see Task 5 notes
            pip install auditwheel
          CIBW_BUILD_FRONTEND: build
          CIBW_BUILD_COMMAND: bash scripts/build_wheel.sh
          CIBW_TEST_COMMAND: |
            pip install pytest pyarrow
            unset CYPHAL_REASSEMBLE_BIN
            export PATH="/usr/bin:/bin"
            pytest {project}/python_tests/ -v
          CIBW_TEST_REQUIRES: pytest pyarrow

      - uses: pypa/gh-action-pypi-publish@release/v1
        with:
          packages-dir: wheelhouse/
```

**Task 5 notes — Arrow on manylinux:** The exact Arrow install inside the cibuildwheel manylinux_2_28 image may require iteration. Preferred order:

1. Reuse the Apache Arrow yum/rpm repo if available for the manylinux base image.
2. If not, vendor a static install step matching what works in `ci.yml` (wget `.deb` + `alien`, or build Arrow IPC-only via script).
3. Gate the workflow with a manual `workflow_dispatch` first before tagging.

Adjust `CIBW_BEFORE_BUILD` during implementation until `make` succeeds inside the container. Document the final commands in the workflow comment.

- [ ] **Step 2: Configure PyPI trusted publisher (repo owner manual steps)**

**→ Full walkthrough:** [PyPI trusted publishing setup (repo owner)](../../specs/2026-07-03-cyphal-reassemble-platform-wheels-design.md#pypi-trusted-publishing-setup-repo-owner) in the design spec.

Quick reference — must match the workflow in Step 1:

| Where | Field | Value |
| --- | --- | --- |
| PyPI | Project name | `cyphal-reassemble` |
| PyPI | Owner | `Finwood` |
| PyPI | Repository | `cyphal-reassemble` |
| PyPI | Workflow filename | `wheels.yml` |
| PyPI | Environment | `pypi` |
| GitHub | Environment name | `pypi` (repo **Settings → Environments**) |

Do **not** create a `PYPI_API_TOKEN` secret. Trusted publishing uses OIDC (`id-token: write`).

Complete PyPI + GitHub setup **before** pushing the first `v*` tag. Optional: TestPyPI dry-run
first (same publisher fields on test.pypi.org).

- [ ] **Step 3: Dry-run without publish**

Temporarily comment out the `gh-action-pypi-publish` step; push a test branch; run `workflow_dispatch`; verify `wheelhouse/*.whl` artifact contains `_bin/cyphal-reassemble` and passes `CIBW_TEST_COMMAND`.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/wheels.yml
git commit -m "ci: add cibuildwheel release workflow with PyPI trusted publishing"
```

---

## Task 6: Makefile and README

**Files:**
- Modify: `Makefile`
- Modify: `README.md`

- [ ] **Step 1: Add Makefile targets**

Append to `Makefile`:

```makefile
.PHONY: wheel-bundle wheel-build wheel-test

wheel-bundle:
	./scripts/prepare_wheel_bundle.sh

wheel-build:
	./scripts/build_wheel.sh

wheel-test: wheel-bundle
	uv run pytest python_tests/ -v
```

Update `help` target to list these.

- [ ] **Step 2: Update README Python section**

Add after the existing Python package section:

````markdown
### Install from PyPI (Linux x86_64)

```bash
pip install cyphal-reassemble
# or: uv add cyphal-reassemble
```

Requires **Linux x86_64** with glibc ≥ manylinux_2_28. The wheel bundles the
`cyphal-reassemble` binary and `libarrow`; no local C++ build needed.

Other platforms: clone this repo, run `make`, then `uv sync` (editable install).

Override binary path: `CYPHAL_REASSEMBLE_BIN=/path/to/cyphal-reassemble`.
````

- [ ] **Step 3: Commit**

```bash
git add Makefile README.md
git commit -m "docs: PyPI install and wheel build Makefile targets"
```

---

## Task 7: Tag and release v0.1.0

**Files:**
- Confirm: `pyproject.toml` (version already `0.1.0`)

- [ ] **Step 1: Confirm version**

Ensure `pyproject.toml` has `version = "0.1.0"` (matches the existing Python wrapper; first
PyPI release ships the platform wheel at the same version — no bump required).

- [ ] **Step 2: Tag and publish**

```bash
git tag v0.1.0
git push origin v0.1.0
```

The `wheels.yml` workflow runs on `v*` tags, builds the platform wheel, and publishes to PyPI
via trusted publishing.

- [ ] **Step 3: Verify PyPI**

```bash
pip install cyphal-reassemble==0.1.0
python -c "from cyphal_reassemble import resolve_binary; print(resolve_binary())"
```

Expected: path under `site-packages/cyphal_reassemble/_bin/cyphal-reassemble`.

- [ ] **Step 4: Integrate downstream**

In frame-decoding pipeline: replace local build / `CYPHAL_REASSEMBLE_BIN` with PyPI dependency on `cyphal-reassemble>=0.1.0`.

---

## Verification checklist (before marking complete)

| Check | Command / criterion |
| --- | --- |
| C++ tests | `make test` — all pass |
| Dev Python tests | `uv run pytest python_tests/ -v` — all pass (repo `build/`) |
| Bundle script | `./scripts/prepare_wheel_bundle.sh` — `--help` OK, `ldd` clean |
| Wheel contents | `unzip -l dist/*.whl` lists `cyphal_reassemble/_bin/cyphal-reassemble` |
| Wheel tag | Filename matches `*-py3-none-manylinux_2_28_x86_64.whl` |
| Wheel smoke | `pip install dist/*.whl && pytest python_tests/` with `CYPHAL_REASSEMBLE_BIN` unset |
| Golden parity | `test_reassemble.py` passes against bundled binary |
| PyPI publish | Tag triggers workflow; package installable from PyPI |

---

## Self-review notes (for the implementer)

| Spec requirement | Plan task |
| --- | --- |
| auditwheel repair before hatchling | Task 2 `prepare_wheel_bundle.sh` |
| `_bin/` resolution slot (PR #2) | Unchanged; Task 4 tests it |
| `py3-none-manylinux_2_28_x86_64` tag | Task 3 `wheel tags` retag step |
| hatchling force-include `_bin/` | Task 3 `pyproject.toml` |
| PyPI trusted publishing | Task 5 workflow + manual PyPI setup |
| x86_64 only v1 | `CIBW_ARCHS_LINUX: x86_64` |
| Dev workflow unchanged | `uv sync` + `make` fallback preserved |
| No C++ changes | Entire plan is packaging/CI only |

**Known implementation risk:** Installing Arrow C++ inside the cibuildwheel manylinux container is the likeliest iteration point. Resolve in Task 5 before first tag; do not publish until `CIBW_TEST_COMMAND` passes.
