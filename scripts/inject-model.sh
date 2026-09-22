#!/bin/bash
# SomnoTrace - inject a locally-generated SomnoStage dev model into the build
#
# Usage: scripts/inject-model.sh <artifact-dir>
#   e.g.: scripts/inject-model.sh .somnostage-distrib/v4.2.3-dev
#   The dir must contain model.enc, manifest.json, key.dev.h, mask.dev.h.
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

# verify model.enc matches the manifest, and that the key in this dir actually
# decrypts it.  The second check is the one that catches a bundle assembled by
# hand from mismatched exports: hashes can all be self-consistent while the key
# is from a different model, which builds fine and fails on the device.
python3 - "$SRC" <<'PY'
import hashlib, json, re, struct, subprocess, sys, pathlib

src = pathlib.Path(sys.argv[1])
manifest = json.loads((src / "manifest.json").read_text())
enc = (src / "model.enc").read_bytes()
got = hashlib.sha256(enc).hexdigest()
if got != manifest["model_enc_sha256"]:
    sys.exit(f"model.enc sha256 mismatch: {got} != {manifest['model_enc_sha256']}")

# SSTG envelope: 4s magic, u16 version, u16 header_size, 32s semver, 32s build_id,
# 40s target_commit, 32s release_id, u32 plaintext_size, 32s plaintext_sha256, 16s iv
if enc[:4] != b"SSTG":
    sys.exit("model.enc: bad magic (not an SSTG envelope)")
header_size = struct.unpack_from("<H", enc, 6)[0]
plain_size = struct.unpack_from("<I", enc, 144)[0]
plain_sha = enc[148:180]
iv = enc[180:196]

key = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", (src / "key.dev.h").read_text()))
if len(key) != 32:
    sys.exit(f"key.dev.h: expected 32 bytes, got {len(key)}")

# openssl rather than a python crypto dependency: present on every dev machine
# and on the CI runner, and this is a one-shot decrypt of a 493 KB blob.
try:
    pt = subprocess.run(
        ["openssl", "enc", "-d", "-aes-256-cbc", "-K", key.hex(),
         "-iv", iv.hex(), "-nopad"],
        input=enc[header_size:], capture_output=True, check=True).stdout
except FileNotFoundError:
    sys.exit("openssl not found — required to verify the key decrypts model.enc")
except subprocess.CalledProcessError as e:
    sys.exit(f"key does not decrypt model.enc: {e.stderr.decode().strip()}")

pad = pt[-1] if pt else 0
if not 1 <= pad <= 16:
    sys.exit(f"key does not decrypt model.enc: bad PKCS7 padding byte {pad}")
payload = pt[:-pad]
if len(payload) != plain_size:
    sys.exit(f"key does not decrypt model.enc: got {len(payload)} B, header says {plain_size}")
if hashlib.sha256(payload).digest() != plain_sha:
    sys.exit("key does not decrypt model.enc: payload sha256 does not match the header")

print(f"manifest verified: payload {manifest['payload_bytes']} B, "
      f"semver {manifest['model_semver']}")
print(f"key verified: decrypts model.enc to {len(payload)} B, sha256 matches header")
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
