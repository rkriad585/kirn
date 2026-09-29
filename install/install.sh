#!/bin/sh
# Kirn installer, in the style of rustup-init:
#
#   curl --proto '=https' --tlsv1.2 -sSf \
#     https://github.com/rkriad585/kirn/releases/latest/download/install.sh | sh
#
# Detects the platform, downloads the matching release archive, verifies its
# SHA-256 against the release checksum file, and installs into ~/.kirn.
#
# Deliberately POSIX sh (not bash) so it runs on Alpine/BusyBox, and it needs
# nothing but curl or wget, tar, and a sha256 tool.
set -eu

REPO="rkriad585/kirn"
PREFIX="${KIRN_PREFIX:-$HOME/.kirn}"
MODIFY_PATH=1
UNINSTALL=0
VERSION=""

usage() {
  cat <<'EOF'
Install the Kirn toolchain.

Usage: install.sh [options]

Options:
  --prefix DIR     install into DIR (default: $HOME/.kirn)
  --no-modify-path do not edit your shell profile
  --version TAG    install a specific release tag (default: latest)
  --uninstall      remove an existing installation
  -h, --help       show this message

Environment:
  KIRN_PREFIX      same as --prefix
  KIRN_RELEASE_BASE  override the download host (mirrors, testing)
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a value}"; shift 2 ;;
    --prefix=*) PREFIX="${1#*=}"; shift ;;
    --version) VERSION="${2:?--version needs a value}"; shift 2 ;;
    --version=*) VERSION="${1#*=}"; shift ;;
    --no-modify-path) MODIFY_PATH=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }

# ---- platform detection ----------------------------------------------------
uname_s=$(uname -s)
uname_m=$(uname -m)

case "$uname_s" in
  Linux)  os=linux ;;
  Darwin) os=darwin ;;
  *) die "unsupported OS: $uname_s (Kirn ships linux and darwin builds; on Windows use install.ps1)" ;;
esac

case "$uname_m" in
  x86_64|amd64)  arch=amd64 ;;
  arm64|aarch64) arch=arm64 ;;
  *) die "unsupported CPU architecture: $uname_m" ;;
esac

target="$os-$arch"

# ---- uninstall -------------------------------------------------------------
if [ "$UNINSTALL" -eq 1 ]; then
  if [ -d "$PREFIX" ]; then
    rm -rf "$PREFIX"
    echo "removed $PREFIX"
  else
    echo "nothing installed at $PREFIX"
  fi
  exit 0
fi

# ---- helpers ---------------------------------------------------------------
fetch() {
  # fetch <url> <output>; curl preferred, wget fallback. TLS is pinned for
  # https, which is what every real download uses. A plain-http host is only
  # reachable when KIRN_RELEASE_BASE explicitly points at one (a mirror or a
  # test); the default GitHub path can never downgrade.
  f_url="$1"
  f_out="$2"
  if command -v curl >/dev/null 2>&1; then
    case "$f_url" in
      https://*) curl --proto '=https' --tlsv1.2 -sSfL "$f_url" -o "$f_out" ;;
      http://*) curl -sSfL "$f_url" -o "$f_out" ;;
      *) die "refusing to fetch a non-http(s) URL: $f_url" ;;
    esac
  elif command -v wget >/dev/null 2>&1; then
    wget -q "$f_url" -O "$f_out"
  else
    die "need curl or wget"
  fi
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | sed 's/^.*= *//'
  else
    die "need sha256sum, shasum, or openssl to verify the download"
  fi
}

# ---- resolve the release ---------------------------------------------------
# KIRN_RELEASE_BASE lets a mirror (or a test) stand in for GitHub, the way
# RUSTUP_DIST_SERVER does for rustup.
if [ -n "${KIRN_RELEASE_BASE:-}" ]; then
  base="$KIRN_RELEASE_BASE"
  tag="${KIRN_RELEASE_TAG:-$VERSION}"
  [ -n "$tag" ] || tag="latest"
elif [ -n "$VERSION" ]; then
  tag="$VERSION"
  base="https://github.com/$REPO/releases/download/$tag"
else
  tag="latest"
  base="https://github.com/$REPO/releases/latest/download"
fi

tmp=$(mktemp -d 2>/dev/null || mktemp -d -t kirn)
trap 'rm -rf "$tmp"' EXIT INT TERM

archive="kirn-$target.tar.gz"
echo "Kirn installer: $target ($tag)"

echo "  downloading $archive"
fetch "$base/$archive" "$tmp/$archive" || die "download failed: $base/$archive"

# Verify integrity when the release publishes checksums.txt. A missing file is
# not fatal: older releases may not have one.
if fetch "$base/checksums.txt" "$tmp/checksums.txt" 2>/dev/null; then
  # Compare the filename as an exact string rather than a regex - the name is
  # full of dots, and a ./ prefix must be tolerated because some releases were
  # generated from inside the download directory.
  expected=$(awk -v f="$archive" '
    { n = $2; sub(/^\.\//, "", n); if (n == f) { print $1; exit } }
  ' "$tmp/checksums.txt")
  if [ -n "${expected:-}" ]; then
    actual=$(sha256_of "$tmp/$archive")
    if [ "$expected" != "$actual" ]; then
      die "checksum mismatch for $archive
  expected $expected
  actual   $actual"
    fi
    echo "  checksum ok"
  else
    echo "  warning: no checksum entry for $archive, skipping verification"
  fi
else
  echo "  warning: checksums.txt not published, skipping verification"
fi

# ---- install ---------------------------------------------------------------
echo "  extracting to $PREFIX"
mkdir -p "$PREFIX"
tar -xzf "$tmp/$archive" -C "$PREFIX" --strip-components=1

if [ ! -x "$PREFIX/bin/kirn" ]; then
  die "archive did not contain bin/kirn at $PREFIX"
fi

# ---- PATH ------------------------------------------------------------------
bindir="$PREFIX/bin"
if [ "$MODIFY_PATH" -eq 1 ]; then
  rc=''
  for candidate in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
    if [ -f "$candidate" ]; then rc="$candidate"; break; fi
  done
  if [ -z "$rc" ]; then
    rc="$HOME/.profile"
  fi
  if ! grep -Fq "$bindir" "$rc" 2>/dev/null; then
    {
      echo ""
      echo "# added by the Kirn installer"
      echo "export PATH=\"\$PATH:$bindir\""
    } >> "$rc"
    echo "  added $bindir to PATH in $rc"
  else
    echo "  $bindir already on PATH in $rc"
  fi
fi

cat <<EOF

Kirn installed to $PREFIX

  export PATH="\$PATH:$bindir"

Note: the standard library is resolved relative to the script you run, or via
KIRN_STDLIB. Add this to your profile so imports work from any directory:

  export KIRN_STDLIB="$PREFIX/stdlib"

Try it:
  $bindir/kirn run https://github.com/$REPO/raw/main/examples/01_hello.kn
EOF
