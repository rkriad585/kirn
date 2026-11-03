# Kirn from npm

The npm package is a dependency-free launcher for the matching Kirn release.

```sh
npm install -g @kirn-lang/kirn
# or
npx @kirn-lang/kirn run examples/01_hello.kn
```

It downloads the platform archive on first use. Set `KIRN_VERSION` to select a
different release or `KIRN_NPM_HOME` to choose the local binary cache.
