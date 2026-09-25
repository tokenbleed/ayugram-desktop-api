#!/bin/sh
# Watch an AyuGram bulk-download job.
# Usage: watch_dl.sh [job_id] [token] [port]
job_id="${1:-1}"
token="${2:-mysecret}"
port="${3:-48123}"
prog='
import json, sys
try:
    d = json.loads(sys.stdin.read())
except Exception:
    print("(query failed)")
    raise SystemExit
p = d.get("progress", {})
print("%9s files %d/%d  %.2f/%.2f GB  %.1f MB/s  eta %ds  skipped %d" % (
    d.get("status", "?"),
    p.get("files_finished", 0), p.get("files_total", 0),
    p.get("bytes_loaded_total", 0) / 1073741824,
    p.get("bytes_total_all", 0) / 1073741824,
    p.get("bytes_per_second", 0) / 1048576,
    p.get("eta_seconds", 0),
    p.get("files_skipped", 0)))
'
while true; do
	v=$(curl -s -H "X-Api-Token: $token" \
		"http://127.0.0.1:$port/api/v1/jobs/$job_id" 2>/dev/null) || v=""
	printf '%s' "$v" | python3 -c "$prog" || true
	sleep 5
done