#!/bin/bash
# run.sh — start the throwaway asyncssh server, run ssh_test against it, tear down.
cd "$(dirname "$0")" || exit 1
PY=.venv/bin/python
LOG=/tmp/sshsrv.log

start_server() {
  "$PY" server.py > "$LOG" 2>&1 &
  SRV=$!
  for _ in $(seq 1 60); do grep -q SERVER_READY "$LOG" 2>/dev/null && return 0; sleep 0.1; done
  echo "server failed to start:"; cat "$LOG"; return 1
}
stop_server() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }

RC=0

echo "=== TEST 1: password auth ==="
start_server || exit 1
./ssh_test 127.0.0.1 2222 testuser testpw || RC=1
stop_server

echo ""
echo "=== TEST 2: publickey auth ==="
./ssh_test --keygen > /dev/null
start_server || exit 1
./ssh_test 127.0.0.1 2222 testuser || RC=1
stop_server

echo ""
echo "=== server log tail ==="
tail -4 "$LOG"
exit $RC
