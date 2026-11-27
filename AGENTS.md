# Agent instructions — cyphal-reassemble workspace

uv workspace with two packages under `packages/`:

- `packages/cyphal-reassemble` — Cyphal/CAN v1.0 transfer reassembler (C++
  binary via libcanard + Arrow IPC, thin Python wrapper)
- `packages/canedge-cyphal` — CANedge MF4 → frame hive → transfer parquet

## Setup (once per checkout)

```bash
git submodule update --init --recursive
uv sync --all-packages
make -C packages/cyphal-reassemble   # build/ binary + stages py/.../_bin/
```

Requires [uv](https://docs.astral.sh/uv/), CMake, a C++ toolchain, and
`libarrow-devel` (Fedora: `sudo dnf install -y libarrow-devel cmake gcc-c++`).

## Build and test

```bash
make -C packages/cyphal-reassemble              # configure + build (Release)
make -C packages/cyphal-reassemble test         # ctest (unit + golden)
make -C packages/cyphal-reassemble test-unit
make -C packages/cyphal-reassemble test-golden
make -C packages/canedge-cyphal test
uv run --package cyphal-reassemble pytest
uv run --package canedge-cyphal pytest
uv run ruff check .
```

Prefer `uv run …` / `uv run --package …` so the workspace venv is used.
Override the binary with `CYPHAL_REASSEMBLE_BIN` when needed. Details:
root `README.md`, `packages/cyphal-reassemble/README.md`,
`packages/canedge-cyphal/README.md`.

## Git worktrees

Use [Worktrunk](https://worktrunk.dev/) (`wt`) for all local worktree
create/switch. Cloud agents are exempt — they work in a single checkout and do
not use worktrees.

```bash
wt switch --create feat/my-change
# or switch to an existing branch/worktree:
wt switch feat/my-change
```

Default path template (sibling of the primary repo):

```text
{{ repo_path }}/../{{ repo }}.{{ branch | sanitize }}
```

| Branch         | Worktree directory                  |
|----------------|-------------------------------------|
| `feat/foo-bar` | `…/cyphal-reassemble.feat-foo-bar`  |
| `fix/bin-path` | `…/cyphal-reassemble.fix-bin-path` |

Hooks in `.config/wt.toml` set up each new worktree: init submodules, copy
gitignored paths selected by `.worktreeinclude` from the primary checkout
(`wt step copy-ignored --require-include` — currently
`packages/cyphal-reassemble/py/cyphal_reassemble/_bin/`), then
`uv sync --all-packages`. Worktrunk reads `.worktreeinclude` from the
**source** (primary) worktree, so the primary checkout must have that file
(e.g. after this lands on the branch primary uses). Do not invent seeded
files by hand or commit the binary / `.venv`. Build with
`make -C packages/cyphal-reassemble` in the worktree when the copied `_bin`
is missing or stale (including when primary and the new worktree have
different tree layouts).

## Agent practices

- Work on a dedicated branch/worktree via `wt` (skip for cloud agents); do not
  commit unrelated dirty state from another feature branch.
- Keep changes scoped; match existing style (`packages/cyphal-reassemble`: C++
  under `src/`, Python under `py/`, tests under `tests/`;
  `packages/canedge-cyphal`: Python under `src/`, tests under `tests/`).
- Prefer Conventional Commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).
- Do not amend pushed commits or force-push shared branches unless explicitly
  asked.
- After cyphal-reassemble C++ or Python changes, run
  `make -C packages/cyphal-reassemble test` and
  `uv run --package cyphal-reassemble pytest`; after canedge-cyphal changes,
  run `uv run --package canedge-cyphal pytest` (`make -C packages/canedge-cyphal`
  first when those tests read an MF4). Say what you could not run.
