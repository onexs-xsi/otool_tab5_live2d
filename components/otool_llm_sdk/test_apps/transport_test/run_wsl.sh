#!/bin/bash
# transport_test runner for WSL (output goes to a file, avoids pipe issues)
set -u
BASE=/mnt/d/work/otool_tab5_live2d/components/otool_llm_sdk/test_apps
OUT=$BASE/transport_test/run_result.txt
: > "$OUT"
{
  echo "=== server ==="
  pkill -f local_sse_server 2>/dev/null
  cd "$BASE" || exit 1
  nohup python3 local_sse_server.py 18080 >/tmp/lss5.log 2>&1 &
  SRV=$!
  sleep 2
  curl --max-time 5 -s -o /dev/null -w "server_http=%{http_code}\n" http://127.0.0.1:18080/ok
  cat /tmp/lss5.log | head -3

  echo "=== build ==="
  cd "$BASE/transport_test" || exit 1
  source ~/esp/esp-idf/export.sh >/dev/null 2>&1 || { echo "export.sh failed"; exit 1; }
  idf.py set-target linux 2>&1 | tail -1
  idf.py build 2>&1 | tail -4

  echo "=== run ==="
  if [ -x ./build/transport_test.elf ]; then
    ./build/transport_test.elf 2>&1 | tail -30
    echo "ELF_RC=${PIPESTATUS[0]}"
  else
    echo "ELF NOT BUILT"
  fi
} > "$OUT" 2>&1
echo "DONE" >> "$OUT"
