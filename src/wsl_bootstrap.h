#pragma once

namespace poppy::wsl {
// Fixed program; distribution, version, hash and size are separate arguments.
inline constexpr const char* bootstrap = R"SH(set -eu
[ "$(uname -m)" = x86_64 ] || { echo 'Poppy supports Linux x86_64 only' >&2; exit 1; }
umask 077
base="$HOME"
for component in .local share yumayo Poppy helpers "$1-$2"; do
  base="$base/$component"
  [ ! -L "$base" ] || { echo 'Helper directory is a symbolic link' >&2; exit 1; }
  mkdir -p -- "$base"
  [ "$(stat -c %u -- "$base")" = "$(id -u)" ] || { echo 'Helper directory has another owner' >&2; exit 1; }
  mode=$(stat -c %a -- "$base")
  [ "$((0$mode & 0022))" = 0 ] || { echo 'Helper directory is writable by other users' >&2; exit 1; }
done
target="$base/poppy-wsl-helper"
valid=0
if [ -f "$target" ] && [ ! -L "$target" ] && [ -x "$target" ] && [ "$(stat -c %u -- "$target")" = "$(id -u)" ]; then
  digest=$(sha256sum < "$target")
  mode=$(stat -c %a -- "$target")
  if [ "${digest%% *}" = "$2" ] && [ "$((0$mode & 0022))" = 0 ]; then valid=1; fi
fi
if [ "$valid" = 1 ]; then
  head -c "$3" > /dev/null
else
  temporary=$(mktemp "$base/.install-XXXXXX")
  trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
  head -c "$3" > "$temporary"
  digest=$(sha256sum < "$temporary")
  [ "${digest%% *}" = "$2" ] || { echo 'Helper checksum mismatch' >&2; exit 1; }
  chmod 700 -- "$temporary"
  mv -f -- "$temporary" "$target"
  trap - EXIT HUP INT TERM
fi
exec "$target" --stdio
)SH";
}
