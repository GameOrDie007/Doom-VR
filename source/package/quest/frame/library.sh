#!/bin/bash
# Game Or Die on the Steam Frame: adds or removes a game's library entry (library.py), with
# Steam stopped while the files are written (Steam rewrites shortcuts.vdf when it exits).
# Run ON THE FRAME as a job of its own, started by the port's setup-quest.ps1 (systemd-run --user):
# stopping Steam would end a job started from the SSH session.  The settings come from
# library.conf beside this file; the last line of library.log says DONE or FAILED.
#   bash library.sh add|remove
D=$(cd "$(dirname "$0")" && pwd)
. "$D/library.conf"
exec >> "$D/library.log" 2>&1
echo "=== $(date) $1 start"
was=0
if systemctl --user cat steam >/dev/null 2>&1 || pgrep -u "$USER" -x steam >/dev/null; then
  was=1
  systemctl --user stop steam
  for i in $(seq 1 60); do pgrep -u "$USER" -x steam >/dev/null || break; sleep 1; done
  if pgrep -u "$USER" -x steam >/dev/null; then
    echo "Steam is still running: nothing changed"
    systemctl --user start steam
    echo "=== $(date) FAILED"
    exit 1
  fi
  echo "Steam stopped after ${i}s"
else
  echo "Steam is not running"
fi
r=DONE
python3 "$D/library.py" "$1" --steam "$STEAM" --user "$USERID" --appid "$APPID" --name "$NAME" \
    --exe "$EXE" --dir "$D" --icon "$D/icon.png" --art "$D" --tool "${TOOL:-lepton}" || r=FAILED
[ "$was" = 1 ] && systemctl --user start steam && echo "Steam started again"
echo "=== $(date) $r"
[ "$r" = DONE ]
