#!/usr/bin/env bash
# Sync modified files from the remote HAP project to the local project.
#
# Usage:
#   ./sync_from_remote.sh <list_file>          # paths read from a file (one per line)
#   ./sync_from_remote.sh -- path1 path2 ...   # paths passed as arguments
#   cat list.txt | ./sync_from_remote.sh       # paths read from stdin
#
# Each entry is a path relative to the project root
# (e.g. entry/src/main/ets/pages/Index.ets).
# Blank lines and lines starting with '#' are ignored.
# A path ending with '/' is fetched recursively as a directory.

set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "${SCRIPT_DIR}/env.sh"

SSH_PORT="${SSH_PORT:-22}"
SCP_OPTS=(-P "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
if [ -n "${SSH_KEY:-}" ]; then
  SCP_OPTS+=(-i "$SSH_KEY")
fi

REMOTE_HAP_ROOT="${REMOTE_HAP_ROOT:-~/project/ArkUI-CEF144}"
LOCAL_ROOT="${LOCAL_PROJECT_ROOT}"

usage() {
  cat <<EOF
Usage:
  $0 <list_file>          # paths from a file (one per line)
  $0 -- path1 path2 ...   # paths as arguments
  cat list.txt | $0       # paths from stdin

Each entry is relative to the project root. Lines starting with '#' and blank
lines are ignored. A trailing '/' means fetch as a directory (recursive).
EOF
}

# ---- collect paths -----------------------------------------------------------
paths=()
if [ "$#" -gt 0 ]; then
  if [ "$1" = "--" ]; then
    shift
    for p in "$@"; do paths+=("$p"); done
  elif [ -f "$1" ]; then
    while IFS= read -r line || [ -n "$line" ]; do paths+=("$line"); done < "$1"
  elif [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    usage; exit 0
  else
    usage; exit 1
  fi
else
  while IFS= read -r line || [ -n "$line" ]; do paths+=("$line"); done
fi

# ---- filter + normalize ------------------------------------------------------
clean=()
for raw in "${paths[@]}"; do
  line="${raw%$'\r'}"
  # trim surrounding whitespace
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  [[ -z "$line" || "$line" == \#* ]] && continue
  clean+=("$line")
done

if [ "${#clean[@]}" -eq 0 ]; then
  echo "No paths to sync." >&2
  usage >&2
  exit 1
fi

echo "Remote : ${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_HAP_ROOT}"
echo "Local  : ${LOCAL_ROOT}"
echo "Files  : ${#clean[@]}"
echo "------------------------------------------------------------"

remote_base="${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_HAP_ROOT}"
ok=0
fail=0
failed_list=()

for entry in "${clean[@]}"; do
  is_dir=0
  rel="${entry}"
  [[ "$rel" == */ ]] && is_dir=1

  local_target="${LOCAL_ROOT}/${rel}"
  remote_source="${remote_base}/${rel}"

  # ensure local parent dir exists
  parent="$(dirname -- "$local_target")"
  mkdir -p "$parent"

  if [ "$is_dir" -eq 1 ]; then
    printf '%-60s -> %s\n' "[dir ] ${rel}" "$parent/"
    if scp -r "${SCP_OPTS[@]}" "$remote_source" "$parent/"; then
      ok=$((ok+1))
    else
      fail=$((fail+1)); failed_list+=("$rel")
    fi
  else
    printf '%-60s -> %s\n' "[file] ${rel}" "$local_target"
    if scp "${SCP_OPTS[@]}" "$remote_source" "$local_target"; then
      ok=$((ok+1))
    else
      fail=$((fail+1)); failed_list+=("$rel")
    fi
  fi
done

echo "------------------------------------------------------------"
echo "Done: ${ok} ok, ${fail} failed"
if [ "$fail" -gt 0 ]; then
  echo "Failed paths:" >&2
  for p in "${failed_list[@]}"; do echo "  - $p" >&2; done
  exit 1
fi
