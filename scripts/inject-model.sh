#!/bin/bash
# SomnoTrace - inject a locally-generated SomnoStage dev model into the build
#
# Usage: scripts/inject-model.sh <training-dev-artifact-dir>
#   e.g.: scripts/inject-model.sh /opt/shared/somnostage-training/.ai/somno-dev-model
#
# Copies model.enc into components/somno_ml/ (gitignored), derives the split
# key halves key_a.inc/key_b.inc from the dev key + mask, and verifies the
# manifest hash.  Everything stays local: no commits, no uploads, no secrets
# leave this machine.  Remove the injected files to return to a stub build:
#   rm components/somno_ml/model.enc components/somno_ml/key_*.inc

set -euo pipefail

SRC="${1:-}"
DST="components/somno_ml"

if [[ -z "$SRC" || ! -d "$SRC" ]]; then
    echo "usage: $0 <training-dev-artifact-dir>" >&2
    exit 2
fi

for f in model.enc manifest.json key.dev.h mask.dev.h; do
    if [[ ! -f "$SRC/$f" ]]; then
        echo "missing $SRC/$f — run export_deployment.py --dev-out first" >&2
        exit 1
    fi
done

# verify model.enc matches the manifest
python3 - "$SRC" <<'PY'
import hashlib, json, sys, pathlib
src = pathlib.Path(sys.argv[1])
manifest = json.loads((src / "manifest.json").read_text())
want = manifest["model_enc_sha256"]
got = hashlib.sha256((src / "model.enc").read_bytes()).hexdigest()
if got != want:
    sys.exit(f"model.enc sha256 mismatch: {got} != {want}")
print(f"manifest verified: payload {manifest['payload_bytes']} B, "
      f"semver {manifest['model_semver']}")
PY

# split the dev key into the two injected halves: key_a = key XOR mask, key_b = mask
python3 - "$SRC" "$DST" <<'PY'
import re, sys, pathlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])

def read_bytes(path, name):
    m = re.findall(r"0x([0-9a-fA-F]{2})", path.read_text())
    if len(m) != 32:
        sys.exit(f"{path}: expected 32 bytes, got {len(m)}")
    return bytes(int(x, 16) for x in m)

key = read_bytes(src / "key.dev.h", "key")
mask = read_bytes(src / "mask.dev.h", "mask")
ka = bytes(a ^ b for a, b in zip(key, mask))
kb = mask

dst.mkdir(parents=True, exist_ok=True)
for name, data in (("key_a.inc", ka), ("key_b.inc", kb)):
    (dst / name).write_text(
        ",".join(f"0x{b:02x}" for b in data) + "\n")
print("wrote key_a.inc + key_b.inc (dev split halves)")
PY

cp "$SRC/model.enc" "$DST/model.enc"
cp "$SRC/manifest.json" "$DST/model.manifest.json"
echo "injected model.enc ($(stat -c%s "$DST/model.enc") bytes)"

# safety: the injected artifacts must stay untracked (the component's C
# sources are new-but-committable; only model/key files must be ignored)
if git status --porcelain -- "$DST/model.enc" "$DST/model.manifest.json" \
        "$DST/key_a.inc" "$DST/key_b.inc" | grep -q .; then
    echo "WARNING: injected artifacts show up in git status — check .gitignore" >&2
    exit 1
fi
echo "OK — build with: ./scripts/build-dist.sh   (dev image, do not commit)"
