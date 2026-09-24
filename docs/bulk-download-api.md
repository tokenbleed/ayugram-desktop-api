# Built-in Bulk Download API

AyuGram now ships with an opt-in loopback HTTP/JSON API that lets you run **fast,
resumable, programmatic bulk archive downloads** straight from the desktop client
process. It reuses the same native MTProto engine the app uses for "Export
Telegram Data" — so you get the client's own connection pool, chunked/resumable
file downloads and takeout-based rate-limit handling, instead of going through
Telethon / TDLib or a second client.

The server only listens on `127.0.0.1` and requires a bearer token.

## Enabling

Start AyuGram with a port (and optionally a token):

```bash
AyuGram -api-port=48123 -api-token=mysecret
# or with a fixed token
AYUGRAM_API_PORT=48123 AYUGRAM_API_TOKEN=mysecret AyuGram
```

If no token is given, a random one is generated at startup and printed to the log
(`DownloadApi Info: Listening on 127.0.0.1:48123 with token ...`).

One logged-in account is enough. Unauthenticated requests get `401`.

## Quick start

```bash
TOKEN=mysecret
BASE=http://127.0.0.1:48123

curl -H "X-Api-Token: $TOKEN" $BASE/api/v1/health

# Archive a supergroup called "Network Team" (HTML + all media types)
curl -H "X-Api-Token: $TOKEN" -H "Content-Type: application/json" \
  -d '{"dest":"/home/key/archives/network-team","peer":"Network Team","format":"html","media":["photo","video","file","voice"]}' \
  $BASE/api/v1/download
# -> {"ok":true,"job_id":1}

# Poll progress
curl -H "X-Api-Token: $TOKEN" $BASE/api/v1/jobs/1
```

There is a ready-made Python client in `scripts/bulk_dl.py`.

## Endpoints

All paths are under `/api/v1`. Auth: `X-Api-Token` header, `Authorization:
Bearer ...` header, or `?token=` query parameter.

| Endpoint | Method | Description |
| --- | --- | --- |
| `/health` | GET | Logged-in state, active/running job counts. |
| `/list` | POST | Resolve dialogs to ids/names (a short-lived listing job). |
| `/download` | POST | Start a bulk archive job. |
| `/jobs` | GET | All jobs (id, status, options). |
| `/jobs/{id}` | GET | Full state: progress, result stats, chat list. |
| `/jobs/{id}/cancel` | POST | Cancel a running/queued job. |
| `/jobs/{id}/skip` | POST | Skip a stuck file. Body: `{"random_id": 123}`. |

### `/download`

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `dest` | string | required | Existing output folder. Re-running a job with the same `dest` resumes (already-downloaded/partial files are reused). |
| `format` | string | `"html"` | `html`, `json`, `html_and_json`, or `media` (files only, no index). |
| `chats` | string | `"all"` | `all`, `personal`, `bot`, `groups`, `channels`, `supergroups`, `saved`. |
| `peer` | string | — | Substring (case-insensitive) of the chat name or a numeric chat id. Must match exactly one dialog, otherwise the job errors with `candidates`. |
| `limit` | int | 0 | Cap the number of dialogs processed (0 = no limit). |
| `media` | string[] | all types | `photo`, `video`, `voice`, `video_message`, `sticker`, `gif`, `file`. |
| `size_limit_mb` | int | 0 (unlimited) | Skip media larger than N MB. |
| `since` / `until` | unix seconds or ISO-8601 | — | Date range filter. |

Jobs that would exceed one concurrent export wait in `pending` and start
automatically when the running job finishes (Telegram allows one takeout session
per account at a time).

### Job lifecycle

`pending` → `running` → `finished | cancelled | error`

The final state includes the usual archive layout:
`dest/chat_files/chat_<id>/messages.html` + `media/` tree, the `files`/`bytes`
counters and — for the HTML format — the index at the path shown in `main_file`.

## Notes and limitations

- Uses the account **takeout** session (same as the built-in export), which is
  Telegram's sanctioned way to bulk-archive histories without flood waits. Only
  one takeout job runs at a time; other jobs queue automatically.
- The `media` format downloads all selected media into the same folder layout as
  the built-in export, without generating HTML/JSON indexes.
- Fine-grained per-file progress (`progress.bytes_loaded/total`,
  `message_index/count`, `bytes_random_id`) is updated per slice; use `skip`
  with the reported `bytes_random_id` if a single huge file stalls the queue.
- **Roadmap**: parallel multi-file downloads inside a job (the engine currently
  streams files sequentially per slice, matching the built-in export), a
  `GET /api/v1/chats` listing endpoint backed by the cache, and chunked
  transfer support for streaming raw files out.

## Build

The module is a new OBJECT library `td_download_api` (sources in
`Telegram/SourceFiles/download_api/`). No new runtime dependencies — it uses
`QTcpServer`/`QTcpSocket`, the existing `Export::ApiWrap` engine and a small
self-contained JSON codec. The server starts only when `-api-port=` /
`AYUGRAM_API_PORT` is configured.

```bash
cmake --build out --config Debug --target Telegram
```