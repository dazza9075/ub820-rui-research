#!/usr/bin/env bash
# Install a complete hard-float ARM (armhf) cross toolchain for the UB820 native-code POC.
# Uses zig (self-contained; lets us target arm-linux-gnueabihf with glibc 2.20, matching the device).
# Touches only /opt/zig and /usr/local/bin/zig. Verifies the download SHA-256 from ziglang.org.
#
#   Run:  sudo bash ~/ub820-research/poc/setup-toolchain.sh
#
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo:  sudo bash $0"; exit 1
fi

echo "[1/5] Ensuring base tools (curl, tar, xz, python3)..."
dnf install -y curl tar xz python3 >/dev/null 2>&1 || true

WORK="$(mktemp -d /tmp/zigsetup.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "[2/5] Resolving latest stable zig for linux-x86_64..."
curl -fsSL -o index.json https://ziglang.org/download/index.json
# Parse newest stable tagged release (x.y.z), print "<tarball-url> <sha256> <version>"
INFO="$(python3 - index.json <<'PY'
import json, sys, re
d = json.load(open(sys.argv[1]))
vers = [k for k in d if re.match(r'^\d+\.\d+\.\d+$', k)]
vers.sort(key=lambda v: tuple(int(x) for x in v.split('.')), reverse=True)
for v in vers:
    b = d[v].get("x86_64-linux")
    if b and b.get("tarball"):
        print(b["tarball"], b.get("shasum", ""), v)
        break
PY
)"
ZURL="$(echo "$INFO" | awk '{print $1}')"
ZSHA="$(echo "$INFO" | awk '{print $2}')"
ZVER="$(echo "$INFO" | awk '{print $3}')"
[ -n "$ZURL" ] || { echo "Could not resolve a zig download URL."; exit 1; }
echo "    version = $ZVER"
echo "    url     = $ZURL"

echo "[3/5] Downloading and verifying SHA-256..."
curl -fSL -o zig.tar.xz "$ZURL"
if [ -n "$ZSHA" ]; then
  echo "${ZSHA}  zig.tar.xz" | sha256sum -c - || { echo "SHA-256 MISMATCH — aborting."; exit 1; }
else
  echo "    (no shasum in index; integrity check skipped)"
fi

echo "[4/5] Installing to /opt/zig ..."
tar xf zig.tar.xz
DIR="$(find . -maxdepth 1 -type d -name 'zig-*' | head -1)"
[ -n "$DIR" ] || { echo "Extracted dir not found."; exit 1; }
rm -rf /opt/zig
mv "$DIR" /opt/zig
ln -sf /opt/zig/zig /usr/local/bin/zig
chmod -R a+rX /opt/zig

echo "[5/5] Verifying cross-compile (hard-float armhf, glibc 2.20)..."
cat > t.c <<'EOF'
#include <stdio.h>
int main(void){ printf("hello from armhf\n"); return 0; }
EOF
/usr/local/bin/zig cc -target arm-linux-gnueabihf.2.20 t.c -o t.arm
echo -n "    "; file t.arm
echo -n "    ELF flags: "; readelf -h t.arm 2>/dev/null | awk -F: '/Flags/{print $2}' | sed 's/^ *//'

echo
echo "==================================================================="
echo " TOOLCHAIN READY"
echo "   zig        : $(command -v zig)"
echo "   zig version: $(/usr/local/bin/zig version)"
echo "   build cmd  : zig cc -target arm-linux-gnueabihf.2.20 <src> -o <out> -ldl"
echo "==================================================================="
