# Kirn from npm

<p align="center">
  <img src="https://raw.githubusercontent.com/rkriad585/kirn/main/kirn-logos/kirn-icon-on-light.png" alt="Kirn logo" width="180">
</p>

The npm package is a dependency-free launcher for the matching Kirn release.

## Install Kirn

```sh
# curl (Linux/macOS)
curl -fsSL https://raw.githubusercontent.com/rkriad585/kirn/main/installer.sh | sh -s -- --version v0.0.1-b3

# npm
npm install -g @kirn-lang/kirn

# PyPI
python -m pip install kirn
```

```powershell
# Chocolatey (Windows)
choco install kirn --version 0.0.1-b3 --yes
```

```sh
npm install -g @kirn-lang/kirn
# or
npx @kirn-lang/kirn run examples/01_hello.kn
```

It downloads the platform archive on first use. Set `KIRN_VERSION` to select a
different release or `KIRN_NPM_HOME` to choose the local binary cache.
