#!/bin/sh
# Official root installer for Linux, macOS, and Termux.
set -eu

REPO="rkriad585/kirn"
TERMUX_PREFIX="${PREFIX:-}"
PREFIX="${KIRN_PREFIX:-$HOME/.kirn}"
VERSION=""
MODIFY_PATH=1
UNINSTALL=0

usage() { cat <<'EOF'
Install Kirn from GitHub Releases.

Options: --prefix DIR  --version TAG  --no-modify-path  --uninstall  -h
Environment: KIRN_PREFIX, KIRN_RELEASE_BASE, KIRN_RELEASE_TAG
EOF
}
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a value}"; shift 2;;
    --prefix=*) PREFIX="${1#*=}"; shift;;
    --version) VERSION="${2:?--version needs a value}"; shift 2;;
    --version=*) VERSION="${1#*=}"; shift;;
    --no-modify-path) MODIFY_PATH=0; shift;;
    --uninstall) UNINSTALL=1; shift;;
    -h|--help) usage; exit 0;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1;;
  esac
done
die() { echo "error: $*" >&2; exit 1; }
[ "$UNINSTALL" -eq 1 ] && { rm -rf "$PREFIX"; echo "removed $PREFIX"; exit 0; }

os_name=$(uname -s)
machine=$(uname -m)
case "$os_name" in
  Darwin) os=darwin;;
  Linux)
    if [ -n "${TERMUX_VERSION:-}" ] || [ "$TERMUX_PREFIX" = "/data/data/com.termux/files/usr" ] || [ "$(uname -o 2>/dev/null || true)" = Android ]; then os=android; else os=linux; fi;;
  *) die "unsupported OS: $os_name (use installer.ps1 on Windows)";;
esac
case "$machine" in
  x86_64|amd64) arch=amd64;; arm64|aarch64) arch=arm64;; *) die "unsupported CPU: $machine";;
esac
[ "$os" = android ] && [ "$arch" != arm64 ] && die "Termux source builds currently support arm64"
target="$os-$arch"

# Termux uses Android's bionic libc and cannot execute the glibc Linux asset.
# Build the same two release executables with Termux's native toolchain instead.
if [ "$os" = android ]; then
  command -v pkg >/dev/null 2>&1 || die "Termux pkg command is required"
  echo "Installing Termux build dependencies"
  pkg install -y clang cmake make openssl zlib sqlite libsqlite >/dev/null
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT INT TERM
  fetch() { if command -v curl >/dev/null 2>&1; then curl --proto '=https' --tlsv1.2 -sSfL "$1" -o "$2"; elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"; else die "need curl or wget"; fi; }
  fetch "https://github.com/$REPO/archive/refs/heads/main.tar.gz" "$tmp/kirn.tar.gz"
  mkdir "$tmp/source"; tar -xzf "$tmp/kirn.tar.gz" -C "$tmp/source" --strip-components=1
  cmake -S "$tmp/source" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$tmp/build" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
  mkdir -p "$PREFIX/bin"; cp "$tmp/build/kirn" "$tmp/build/kirnc" "$PREFIX/bin/"
  cp -R "$tmp/source/stdpet" "$tmp/source/src" "$PREFIX/"
  echo "Kirn built and installed to $PREFIX (Termux/$arch)"
  echo "Run: $PREFIX/bin/kirn --version"
  exit 0
fi
if [ -n "${KIRN_RELEASE_BASE:-}" ]; then
  base="$KIRN_RELEASE_BASE"; tag="${KIRN_RELEASE_TAG:-$VERSION}"; [ -n "$tag" ] || tag=latest
elif [ -n "$VERSION" ]; then tag="$VERSION"; base="https://github.com/$REPO/releases/download/$tag"
else tag=latest; base="https://github.com/$REPO/releases/latest/download"; fi
fetch() { if command -v curl >/dev/null 2>&1; then curl --proto '=https' --tlsv1.2 -sSfL "$1" -o "$2"; elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"; else die "need curl or wget"; fi; }
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1; else openssl dgst -sha256 "$1" | sed 's/^.*= *//'; fi; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT INT TERM
archive="kirn-$target.tar.gz"; fetch "$base/$archive" "$tmp/$archive" || die "download failed: $base/$archive"
if fetch "$base/checksums.txt" "$tmp/checksums.txt" 2>/dev/null; then
  expected=$(awk -v f="$archive" '$2 == f {print $1; exit}' "$tmp/checksums.txt")
  [ -z "$expected" ] || [ "$expected" = "$(sha256 "$tmp/$archive")" ] || die "checksum mismatch for $archive"
fi
mkdir -p "$PREFIX"; tar -xzf "$tmp/$archive" -C "$PREFIX" --strip-components=1
[ -x "$PREFIX/bin/kirn" ] && [ -x "$PREFIX/bin/kirnc" ] || die "archive did not contain Kirn executables"
if [ "$MODIFY_PATH" -eq 1 ]; then
  rc="$HOME/.profile"; [ -f "$HOME/.bashrc" ] && rc="$HOME/.bashrc"
  grep -Fq "$PREFIX/bin" "$rc" 2>/dev/null || { printf '\n# Kirn\nexport PATH="$PATH:%s/bin"\n' "$PREFIX" >> "$rc"; }
fi
echo "Kirn installed to $PREFIX ($target, $tag)"; echo "Run: $PREFIX/bin/kirn --version"
