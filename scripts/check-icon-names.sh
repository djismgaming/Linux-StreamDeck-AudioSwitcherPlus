#!/usr/bin/env bash
# Regression guard: every icon in shared/AudioDevicesIcons must be reachable by the exact path
# the property inspector requests. The PI builds <img src="../AudioDevicesIcons/${icon.id}.png">
# where icon.id is the *lowercased* file stem (listIconSets / GetAvailableIconSets), so a file
# whose name differs from its lowercase stem (e.g. the old "Earbuds.png") 404s on case-sensitive
# Linux (OpenDeck) and shows the fallback image. Windows/macOS are case-insensitive and hide the
# bug, so this check is the only thing that catches it there.
#
# Usage: scripts/check-icon-names.sh [dir ...]   (defaults: shared/AudioDevicesIcons, plus the
# installed OpenDeck copy if present)
set -u
cd "$(dirname "$0")/.."
dirs=()
for arg in "$@"; do dirs+=("$arg"); done
if [ ${#dirs[@]} -eq 0 ]; then
  dirs=("shared/AudioDevicesIcons")
  od_dir="$HOME/.config/opendeck/plugins/com.morganscruggs.audioswitcherplus.sdPlugin/AudioDevicesIcons"
  [ -d "$od_dir" ] && dirs+=("$od_dir")
fi
fail=0
for dir in "${dirs[@]}"; do
  for f in "$dir"/*.png; do
    stem="$(basename "$f" .png)"
    id="$(printf '%s' "$stem" | tr '[:upper:]' '[:lower:]')"
    case "$id" in *@2x) continue ;; esac
    if [ ! -f "$dir/$id.png" ]; then
      echo "FAIL: $dir/$id.png (the path the PI requests) does not exist - actual file: $f"
      fail=1
    fi
  done
done
if [ $fail -eq 0 ]; then
  echo "OK: every icon name matches its lowercased-stem path"
else
  echo "Icon names must equal their own lowercase form (no mixed-case stems)."
fi
exit $fail
