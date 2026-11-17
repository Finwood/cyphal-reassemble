# cyphal-reassemble workspace

Two packages share this repository. `packages/cyphal-reassemble` is the published single-bus Cyphal/CAN reassembler (C++ binary and Python API). `packages/canedge-cyphal` is added by a later change and is not in this tree yet.

```bash
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
uv sync --all-packages
make -C packages/cyphal-reassemble
make -C packages/cyphal-reassemble test
uv run --package cyphal-reassemble pytest
```

Build and wheel details are in `packages/cyphal-reassemble/README.md`.
