# cyphal-reassemble workspace

Two packages share this repository. `packages/cyphal-reassemble` is the published single-bus Cyphal/CAN reassembler (C++ binary and Python API). `packages/canedge-cyphal` converts CANedge MF4 files and reassembles the frame hive. See its README.

```bash
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
uv sync --all-packages
make -C packages/cyphal-reassemble
make -C packages/cyphal-reassemble test
uv run --package cyphal-reassemble pytest
```

Build and wheel details are in `packages/cyphal-reassemble/README.md`.
