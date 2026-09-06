#!/bin/sh
# autoflash-devuan -- from the NAND recovery ("trilobite"), wait for the latest
# build-devuan CI image, download it to USB storage, verify its checksum, write it
# to the SD card, verify the write, and optionally reboot into it.
#
# All prerequisites ship in the recovery image: curl, jq, xz, unzip,
# sha256sum, dd, blkid, blockdev/partx.
#
# AUTH: GitHub requires a token to download Actions artifacts, even for public repos.
#   Create a fine-grained PAT with repository "Actions: read-only" for this repo, then
#   either `export GH_TOKEN=...` or drop it in a file named gh_token on the USB key.
#
# USAGE
#   GH_TOKEN=... autoflash-devuan            # wait for latest build, flash, verify (no reboot)
#   GH_TOKEN=... REBOOT=1 autoflash-devuan   # ...then reboot into the new image
#   RUN_ID=123456 autoflash-devuan           # flash a specific workflow run's artifact
#   VERIFY=full autoflash-devuan             # full read-back compare (slow) instead of sig check
#
# ENV (defaults)
#   REPO=rggammon/beagle-b4-recovery  WORKFLOW=build-devuan.yml
#   ARTIFACT=beagle-sdcard-devuan     TARGET=/dev/mmcblk0
#   WORKDIR=(auto: an ext4/vfat USB partition, else /tmp)
#   WAIT_TIMEOUT=2400  POLL=30  REBOOT=0  VERIFY=sig
set -eu

REPO=${REPO:-rggammon/beagle-b4-recovery}
WORKFLOW=${WORKFLOW:-build-devuan.yml}
ARTIFACT=${ARTIFACT:-beagle-sdcard-devuan}
TARGET=${TARGET:-/dev/mmcblk0}
WAIT_TIMEOUT=${WAIT_TIMEOUT:-2400}
POLL=${POLL:-30}
REBOOT=${REBOOT:-0}
VERIFY=${VERIFY:-sig}
API=https://api.github.com

log() { echo "[autoflash] $*"; }
die() { echo "[autoflash] ERROR: $*" >&2; exit 1; }

# --- storage: the ~200 MB download needs the USB (root fs is read-only, /tmp is a small
#     tmpfs). The mount point lives on tmpfs so mkdir works on the read-only recovery root. ---
MNT=/tmp/autoflash.mnt
MOUNTED=
if [ -z "${WORKDIR:-}" ]; then
  WORKDIR=/tmp
  for p in /dev/sda1 /dev/sdb1 /dev/sda2 /dev/sdb2; do
    [ -b "$p" ] || continue
    case "$(blkid -o value -s TYPE "$p" 2>/dev/null || true)" in
      ext4|ext3|ext2|vfat)
        mkdir -p "$MNT"
        if mount "$p" "$MNT" 2>/dev/null; then
          WORKDIR=$MNT; MOUNTED=$p; log "using USB $p at $WORKDIR"; break
        fi ;;
    esac
  done
  [ -z "$MOUNTED" ] && log "no USB partition mounted; using $WORKDIR (tmpfs -- likely too small)"
fi
mkdir -p "$WORKDIR"
cleanup() { [ -n "$MOUNTED" ] && umount "$MNT" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# --- token: needed ONLY for the artifact download; public-repo polling is unauthenticated.
#     Looked up in $GH_TOKEN, then gh_token on the USB / in /tmp / in root's home. ---
if [ -z "${GH_TOKEN:-}" ]; then
  for f in "$WORKDIR/gh_token" /tmp/gh_token /root/gh_token ./gh_token; do
    [ -r "$f" ] && { GH_TOKEN=$(tr -d ' \t\r\n' < "$f"); log "read token from $f"; break; }
  done
fi

# send the auth header only when we have a token (public reads work without it)
api() {
  if [ -n "${GH_TOKEN:-}" ]; then
    curl -fsSL --retry 4 --retry-delay 3 --retry-all-errors -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" -H "X-GitHub-Api-Version: 2022-11-28" "$@"
  else
    curl -fsSL --retry 4 --retry-delay 3 --retry-all-errors -H "Accept: application/vnd.github+json" -H "X-GitHub-Api-Version: 2022-11-28" "$@"
  fi
}

# --- find / wait for the workflow run ---
if [ -n "${RUN_ID:-}" ]; then
  run=$RUN_ID
else
  log "waiting for latest $WORKFLOW run to succeed (timeout ${WAIT_TIMEOUT}s)..."
  deadline=$(( $(date +%s) + WAIT_TIMEOUT ))
  while :; do
    j=$(api "$API/repos/$REPO/actions/workflows/$WORKFLOW/runs?per_page=1")
    run=$(printf '%s' "$j" | jq -r '.workflow_runs[0].id // empty')
    st=$(printf '%s' "$j" | jq -r '.workflow_runs[0].status // "?"')
    cc=$(printf '%s' "$j" | jq -r '.workflow_runs[0].conclusion // "?"')
    [ -n "$run" ] || die "no runs found for $WORKFLOW"
    log "run $run: status=$st conclusion=$cc"
    if [ "$st" = completed ]; then
      [ "$cc" = success ] && break || die "latest run concluded '$cc' (not success)"
    fi
    [ "$(date +%s)" -ge "$deadline" ] && die "timeout waiting for build to finish"
    sleep "$POLL"
  done
fi
log "using run $run"

# --- resolve + download the artifact zip ---
url=$(api "$API/repos/$REPO/actions/runs/$run/artifacts" \
      | jq -r --arg n "$ARTIFACT" '.artifacts[] | select(.name==$n and .expired==false) | .archive_download_url' | head -1)
[ -n "$url" ] || die "artifact '$ARTIFACT' not found or expired on run $run"
[ -n "${GH_TOKEN:-}" ] || die "artifact download needs a GitHub token (fine-grained PAT, Actions: read). export GH_TOKEN, or write it to /tmp/gh_token or gh_token on the USB, then re-run."
zip="$WORKDIR/$ARTIFACT.zip"
log "downloading artifact -> $zip"
curl -fL --retry 4 --retry-delay 3 --retry-all-errors -H "Authorization: Bearer $GH_TOKEN" -o "$zip" "$url"

# --- unzip + checksum ---
d="$WORKDIR/extract"; rm -rf "$d"; mkdir -p "$d"
unzip -o "$zip" -d "$d" >/dev/null
img=$(ls "$d"/*.img.xz 2>/dev/null | head -1)
[ -n "$img" ] || die "no .img.xz inside the artifact"
sums=$(ls "$d"/SHA256SUMS* 2>/dev/null | head -1)
if [ -n "$sums" ]; then
  log "verifying sha256 of $(basename "$img")..."
  ( cd "$d" && grep -F "$(basename "$img")" "$(basename "$sums")" | sha256sum -c - ) \
    || die "sha256 mismatch -- refusing to flash a corrupt download"
else
  log "WARN: no SHA256SUMS in artifact; skipping checksum"
fi

# --- safety checks before we overwrite the SD ---
[ -b "$TARGET" ] || die "$TARGET is not a block device"
mount | grep -q "^$TARGET" && die "$TARGET has mounted partitions; unmount first"
case "$(awk '$2=="/"{print $1}' /proc/mounts)" in
  *mmcblk0*) die "root is on the SD -- boot NAND recovery before flashing the SD" ;;
esac

# --- flash (stream: decompress on the fly, no full raw staging) ---
log "writing $(basename "$img") -> $TARGET ..."
xz -dc "$img" | dd of="$TARGET" bs=4M
sync
blockdev --rereadpt "$TARGET" 2>/dev/null || partx -u "$TARGET" 2>/dev/null || true

# --- verify ---
if [ "$VERIFY" = full ]; then
  log "full verify: decompress + byte-compare against $TARGET (slow)..."
  out=$(xz -dc "$img" | cmp - "$TARGET" 2>&1 || true)
  case "$out" in
    *differ*) die "read-back verify FAILED: $out" ;;
    *"EOF on -"*|"") log "read-back verify OK" ;;   # device longer than image = expected
    *) log "read-back verify: $out" ;;
  esac
else
  log "verifying partition signatures..."
  p1=$(blkid -o value -s TYPE "${TARGET}p1" 2>/dev/null || true)
  p2=$(blkid -o value -s TYPE "${TARGET}p2" 2>/dev/null || true)
  log "  ${TARGET}p1=$p1  ${TARGET}p2=$p2"
  case "$p1:$p2" in
    vfat:ext4) log "signatures OK" ;;
    *) die "unexpected partition signatures after write (p1=$p1 p2=$p2)" ;;
  esac
fi

log "SUCCESS: $TARGET flashed from run $run."
if [ "$REBOOT" = 1 ]; then
  log "rebooting into the new SD image..."; sync; sleep 1; reboot
else
  log "reboot to boot it (default U-Boot menu auto-boots SD, falls back to NAND)."
fi
