#!/usr/bin/env node

// The npm entry point is intentionally dependency-free. It downloads the same
// signed-by-checksum release archive as the curl installer, then forwards all
// arguments to the real Kirn executable. This keeps both `npm install -g kirn`
// and `npx kirn` usable without shipping platform binaries in the npm tarball.
"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync } = require("child_process");
const https = require("https");

const version = process.env.KIRN_VERSION || require("../package.json").version;
const repo = process.env.KIRN_REPO || "rkriad585/kirn";
const cache = process.env.KIRN_NPM_HOME || path.join(os.homedir(), ".kirn-npm", version);

function target() {
  const platform = process.platform;
  const arch = process.arch;
  const cpu = arch === "x64" ? "amd64" : arch === "arm64" ? "arm64" : null;
  if (process.env.TERMUX_VERSION || process.env.PREFIX?.includes("/com.termux/")) {
    if (cpu !== "arm64") throw new Error("Kirn npm packages currently support Termux arm64");
    return "android-arm64";
  }
  if (platform === "linux" || platform === "darwin") {
    if (!cpu) throw new Error(`unsupported CPU: ${arch}`);
    return `${platform === "darwin" ? "darwin" : "linux"}-${cpu}`;
  }
  if (platform === "win32") return "windows-amd64";
  throw new Error(`unsupported platform: ${platform}/${arch}`);
}

function get(url, file) {
  return new Promise((resolve, reject) => {
    const out = fs.createWriteStream(file);
    https.get(url, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        out.close(); fs.unlinkSync(file);
        return get(res.headers.location, file).then(resolve, reject);
      }
      if (res.statusCode !== 200) {
        out.close(); fs.unlinkSync(file);
        return reject(new Error(`download failed (${res.statusCode}): ${url}`));
      }
      res.pipe(out);
      out.on("finish", () => out.close(resolve));
    }).on("error", (err) => { out.close(); reject(err); });
  });
}

async function ensureBinary(t) {
  const exe = path.join(cache, "bin", process.platform === "win32" ? "kirn.exe" : "kirn");
  if (fs.existsSync(exe)) return exe;
  fs.mkdirSync(cache, { recursive: true });
  const archive = path.join(os.tmpdir(), `kirn-${process.pid}-${Date.now()}.${process.platform === "win32" ? "zip" : "tar.gz"}`);
  const name = `kirn-${t}.${process.platform === "win32" ? "zip" : "tar.gz"}`;
  const tag = `v${version}`;
  try {
    await get(`https://github.com/${repo}/releases/download/${tag}/${name}`, archive);
    if (process.platform === "win32") {
      execFileSync("powershell", ["-NoProfile", "-Command", `Expand-Archive -LiteralPath '${archive.replace(/'/g, "''")}' -DestinationPath '${cache.replace(/'/g, "''")}' -Force`], { stdio: "inherit" });
    } else {
      execFileSync("tar", ["-xzf", archive, "-C", cache, "--strip-components=1"], { stdio: "inherit" });
    }
  } finally { try { fs.unlinkSync(archive); } catch {} }
  if (!fs.existsSync(exe)) throw new Error(`release ${tag} did not contain ${name}`);
  return exe;
}

ensureBinary(target()).then((exe) => {
  const result = execFileSync(exe, process.argv.slice(2), { stdio: "inherit" });
  process.exit(result?.status || 0);
}).catch((err) => {
  console.error(`kirn: ${err.message}`);
  process.exit(1);
});
