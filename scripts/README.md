# Kirn Scripts (`scripts/`)

<p align="center">
  <img src="https://raw.githubusercontent.com/rkriad585/kirn/main/kirn-logos/kirn-icon-on-light.png" alt="Kirn logo" width="180">
</p>

## Install Kirn

```sh
curl -fsSL https://raw.githubusercontent.com/rkriad585/kirn/main/installer.sh | sh -s -- --version v0.0.1-b3
npm install -g @kirn-lang/kirn
python -m pip install kirn
```

```powershell
choco install kirn --version 0.0.1-b3 --yes
```

PowerShell build/test/verification harnesses used during development and CI.

| Script | Purpose |
|--------|---------|
| `runall.ps1` | Run the full example corpus |
| `negative.ps1` | Run the `tests/negative/*.kn` failure suite |
| `types.ps1` | Run the `tests/types/*` suite |
| `bench.ps1` | Run benchmarks (`bench_fib.kn`) |
| `asanall.ps1` | Run suites under AddressSanitizer builds (`build-asan/`) |
| `vm_diff.ps1` | Diff VM output against the tree-walker oracle |

Run with PowerShell from the repo root, e.g.:

```powershell
powershell -File scripts/negative.ps1
```
