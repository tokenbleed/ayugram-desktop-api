#!/usr/bin/env python3
"""Minimal client for the AyuGram built-in bulk download API.

Usage:
  export AYUGRAM_API_URL=http://127.0.0.1:48123
  export AYUGRAM_API_TOKEN=mysecret
  ./bulk_dl.py health
  ./bulk_dl.py list --peer "Network Team"
  ./bulk_dl.py download --dest /home/key/archives/nt --peer "Network Team" \
      --format html --media photo video file --poll
  ./bulk_dl.py status 1
  ./bulk_dl.py cancel 1
"""
import argparse
import json
import os
import sys
import time
import urllib.request
import urllib.error


def http(method, path, body=None, token=None, base=None):
    base = base or os.environ.get("AYUGRAM_API_URL", "http://127.0.0.1:48123")
    token = token or os.environ.get("AYUGRAM_API_TOKEN", "")
    url = base + path
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("X-Api-Token", token)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        raw = error.read()
        try:
            return error.code, json.loads(raw)
        except Exception:
            return error.code, {"raw": raw.decode(errors="replace")}


def wait_finished(job_id, interval=2.0):
    while True:
        status, data = http("GET", f"/api/v1/jobs/{job_id}")
        if status != 200:
            raise SystemExit(f"job query failed: {status} {data}")
        print(f"job {job_id}: {data['status']} "
              f"({data['progress']['message_index']}/{data['progress']['message_count']} "
              f"msgs, {data['progress']['bytes_loaded']}/{data['progress']['bytes_total']} B)")
        if data["status"] in ("finished", "cancelled", "error"):
            return data
        time.sleep(interval)


def cmd_health(args):
    status, data = http("GET", "/api/v1/health")
    print(status, json.dumps(data, indent=2))


def cmd_list(args):
    body = {}
    if args.peer:
        body["peer"] = args.peer
    status, data = http("POST", "/api/v1/list", body)
    if status != 200:
        raise SystemExit(f"list failed: {status} {data}")
    job = wait_finished(data["job_id"])
    if job["status"] != "finished":
        raise SystemExit(f"list job ended: {job['status']} {job.get('error')}")
    for chat in job.get("chats", []):
        print(f"{chat['id']}\t{chat['type']}\t{chat['name']}")


def cmd_download(args):
    body = {"dest": args.dest, "format": args.format}
    if args.peer:
        body["peer"] = args.peer
    if args.chats:
        body["chats"] = args.chats
    if args.media:
        body["media"] = args.media
    if args.limit:
        body["limit"] = args.limit
    if args.size_limit_mb:
        body["size_limit_mb"] = args.size_limit_mb
    status, data = http("POST", "/api/v1/download", body)
    if status != 200:
        raise SystemExit(f"download failed: {status} {data}")
    print(f"job {data['job_id']} created")
    if args.poll:
        result = wait_finished(data["job_id"])
        if result["status"] == "finished":
            print(f"OK: {result['files']} files, {result['bytes']} bytes")
            if result.get("main_file"):
                print("index:", result["main_file"])
        else:
            raise SystemExit(f"job ended: {result['status']} {result.get('error')}")


def cmd_status(args):
    status, data = http("GET", f"/api/v1/jobs/{args.job_id}")
    print(status, json.dumps(data, indent=2))


def cmd_cancel(args):
    status, data = http("POST", f"/api/v1/jobs/{args.job_id}/cancel")
    print(status, json.dumps(data, indent=2))


def cmd_skip(args):
    status, data = http("POST", f"/api/v1/jobs/{args.job_id}/skip",
                        {"random_id": str(args.random_id)})
    print(status, json.dumps(data, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    health = sub.add_parser("health")
    health.set_defaults(func=cmd_health)

    listing = sub.add_parser("list")
    listing.add_argument("--peer")
    listing.set_defaults(func=cmd_list)

    download = sub.add_parser("download")
    download.add_argument("--dest", required=True)
    download.add_argument("--peer")
    download.add_argument("--chats", default="all")
    download.add_argument("--format", default="html",
                          choices=["html", "json", "html_and_json", "media"])
    download.add_argument("--media", nargs="*", default=None)
    download.add_argument("--limit", type=int, default=0)
    download.add_argument("--size-limit-mb", type=int, default=0)
    download.add_argument("--poll", action="store_true")
    download.set_defaults(func=cmd_download)

    status = sub.add_parser("status")
    status.add_argument("job_id", type=int)
    status.set_defaults(func=cmd_status)

    cancel = sub.add_parser("cancel")
    cancel.add_argument("job_id", type=int)
    cancel.set_defaults(func=cmd_cancel)

    skip = sub.add_parser("skip")
    skip.add_argument("job_id", type=int)
    skip.add_argument("--random-id", required=True)
    skip.set_defaults(func=cmd_skip)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()