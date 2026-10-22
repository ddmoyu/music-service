#!/usr/bin/env python3
"""Phase A live acceptance: real NetEase API download, ID3 readback, configuration and restart.

Standard-library test client only; the production service remains native C++.
Usage: python packaging/e2e/metadata_e2e.py --binary PATH [--port 18852]
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import uuid

from kuwo_e2e import OwnService, Http, check, get_url, ticket_for, wait_task


def synchsafe(raw):
    check(len(raw) == 4 and all(b < 128 for b in raw), "ID3 size is synchsafe")
    return sum(b << (7 * (3 - i)) for i, b in enumerate(raw))


def frames(data):
    check(data[:4] == b"ID3\x04" and data[5] == 0, "published file has plain ID3v2.4")
    end = 10 + synchsafe(data[6:10])
    result = {}
    at = 10
    while at + 10 <= end and data[at] != 0:
        name = data[at:at + 4].decode("ascii")
        length = synchsafe(data[at + 4:at + 8])
        check(at + 10 + length <= end, f"{name} frame is within the tag")
        result.setdefault(name, []).append(data[at + 10:at + 10 + length])
        at += 10 + length
    return result


def text(frame):
    check(frame[0] == 3, "written text uses UTF-8")
    return frame[1:].decode("utf-8").rstrip("\0")


def fetch_task(api, track, key):
    request = {"track": track, "metadata_policy": "required"}
    _, _, created = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": key}, expect=(200, 202))
    task = wait_task(api, created["task_id"], ("completed", "failed"), 180)
    check(task["status"] == "completed", f"download completed: {task.get('error')}")
    status, _, body = get_url(ticket_for(api, task["result"]["file_id"]))
    check(status == 200, "published audio is readable through the file API")
    return task, body


def run(service):
    track = {"source": "netease", "id": "5257138"}
    api = Http(service.api, service.token)
    query = "?source=netease&id=5257138"
    detail = api.json("GET", "/api/v1/tracks/detail" + query)[2]
    lyrics = api.json("GET", "/api/v1/lyrics" + query)[2]
    check(bool(detail["artwork"]) and bool(lyrics["lrc"]), "real song has cover and lyrics")
    key = "metadata-a-" + uuid.uuid4().hex
    task, body = fetch_task(api, track, key)
    tags = frames(body)
    check(text(tags["TIT2"][0]) == detail["title"], "title matches the platform")
    check(text(tags["TRCK"][0]).split("/")[0] == str(detail["track_number"]), "track number matches the album metadata")
    check(len(tags["APIC"]) == 1 and len(tags["USLT"]) == 1, "one cover and one lyrics frame")
    picture = tags["APIC"][0]
    check(picture[0] in (0, 3), "cover description uses a single-byte terminator")
    mime, rest = picture[1:].split(b"\0", 1)
    check(rest[0] == 3, "picture is a front cover")
    _, image = rest[1:].split(b"\0", 1)
    with urllib.request.urlopen(detail["artwork"], timeout=30) as response:
        original_image = response.read(8 * 1024 * 1024 + 1)
    check(image == original_image, "embedded cover bytes equal the real upstream image")
    check(mime == b"image/jpeg" and image.startswith(b"\xff\xd8"), "cover MIME matches JPEG bytes")
    lyric_frame = tags["USLT"][0]
    check(lyric_frame[0] == 3, "lyrics use UTF-8")
    _, lyric_bytes = lyric_frame[4:].split(b"\0", 1)
    expected = lyrics["lrc"]
    if lyrics.get("translated_lrc"):
        expected += "\n\n[translation]\n" + lyrics["translated_lrc"]
    check(lyric_bytes.decode("utf-8").rstrip("\0") == expected, "embedded lyrics equal the platform text")
    fields = task["result"]["metadata_fields"]
    check(fields["cover"] == fields["lyrics"] == "written", "task reports both fields written")
    original_files = list(Path(service.downloads).glob("*.mp3"))
    check(len(original_files) == 1, "default output is flat")

    # New settings after a restart must keep old idempotency results, but not reuse
    # an old file for a new intent with a different name or metadata policy.
    config_path = Path(service.config)
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["downloads"] = {"output": {"name_format": "title", "folder_layout": "artist_album",
                                     "embed_cover": False, "embed_lyrics": False}}
    config_path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    service.restart()
    api = Http(service.api, service.token)
    replay, replay_body = fetch_task(api, track, key)
    check(replay["task_id"] == task["task_id"] and replay_body == body, "restart and new defaults preserve idempotency")
    changed, changed_body = fetch_task(api, track, "metadata-a-new-" + uuid.uuid4().hex)
    check(changed["task_id"] != task["task_id"], "new settings do not reuse the old product")
    check(changed["result"]["metadata_fields"]["cover"] == "disabled", "cover switch is applied")
    check(changed["result"]["metadata_fields"]["lyrics"] == "disabled", "lyrics switch is applied")
    paths = [p for p in Path(service.downloads).rglob("*.mp3") if p not in original_files]
    check(len(paths) == 1 and len(paths[0].relative_to(service.downloads).parts) == 3, "artist/album folders are used")
    check(paths[0].name == detail["title"] + ".mp3", "title-only name is applied")
    check(paths[0].read_bytes() == changed_body, "nested publication equals the file API response")
    return {"source": "netease", "track": "5257138", "title": detail["title"],
            "metadata_fields": fields, "cover_mime": mime.decode(), "cover_bytes": len(image),
            "cover_sha256": hashlib.sha256(image).hexdigest(), "lyrics_chars": len(expected),
            "file_bytes": len(body), "file_sha256": hashlib.sha256(body).hexdigest(),
            "nested_path": paths[0].relative_to(service.downloads).as_posix(),
            "restart_idempotency": True, "changed_configuration_isolates_reuse": True}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--port", type=int, default=18852)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    try:
        print(json.dumps(run(service), ensure_ascii=False, indent=2))
    finally:
        service.close()
