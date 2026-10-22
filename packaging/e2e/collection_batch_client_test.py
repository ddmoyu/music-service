"""无网络客户端验收：分页和提交边界、选择、重复、停止与应答丢失重放。"""
import json
from pathlib import Path
import tempfile
import unittest
import urllib.parse

from collection_batch_client import collect, replay, Failure


class FakeApi:
    def __init__(self):
        fixture = Path(__file__).resolve().parents[2] / "tests/fixtures/collections/230-tracks.json"
        self.items = [{"ref": item} for item in json.loads(fixture.read_text())["items"]]
        self.accepted, self.sizes, self.reads = {}, [], 0
        self.lose_response = False
        self.kind = "playlist"

    def json(self, method, path, body=None, headers=None, expect=None):
        if path == "/api/v1/links/parse":
            data = {"matched": True, "kind": self.kind, "ref": {"source": "both", "id": "230"}}
        elif method == "GET":
            self.reads += 1
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(path).query)
            offset, limit = int(query.get("cursor", [0])[0]), int(query["limit"][0])
            items = self.items[offset:offset + limit]
            end = offset + len(items)
            data = {"items": items, "page": {"next_cursor": str(end) if end < len(self.items) else None,
                                              "has_more": end < len(self.items)}}
        else:
            key = headers["Idempotency-Key"]
            if key not in self.accepted:
                self.accepted[key] = body
                self.sizes.append(len(body["items"]))
            elif self.accepted[key] != body:
                raise Failure("conflict")
            if self.lose_response:
                self.lose_response = False
                raise OSError("response lost after acceptance")
            data = {"batch_id": key, "created_count": len(body["items"])}
        return 200, {}, data


class ClientTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "manifest.json"
        self.api = FakeApi()

    def test_230_across_page_boundaries(self):
        report = collect(self.api, "test", self.path, page_size=50)
        self.assertEqual(self.api.sizes, [100, 100, 30])
        self.assertEqual(report["submitted_items"], 230)
        self.assertTrue(report["enumeration_finished"])
        actual = [item["track"] for batch in self.api.accepted.values() for item in batch["items"]]
        self.assertEqual(actual, [i["ref"] for i in self.api.items])

    def test_podcast_selection_uses_audio_reference_and_skips_unavailable(self):
        self.api.kind = "podcast"
        for i, item in enumerate(self.api.items):
            item["audio_ref"] = {"source": "both", "id": f"program:{i+1}:audio{i+1}"}
        self.api.items[0]["audio_ref"] = None
        self.api.items[1]["requires_purchase"] = True
        report = collect(self.api, "test", self.path, ids={"1", "2", "3"})
        self.assertEqual(report["unavailable_items"], 2)
        self.assertEqual(report["submitted_items"], 1)
        self.assertEqual(report["batches"][0]["request"]["items"][0]["track"]["id"], "program:3:audio3")

    def test_artist_hot_list_bound(self):
        self.api.kind = "artist"
        with self.assertRaises(ValueError):
            collect(self.api, "test", self.path, page_size=51)

    def test_selection_retains_order_and_duplicates(self):
        self.api.items.insert(2, self.api.items[0])
        report = collect(self.api, "test", self.path, ids={"1", "3"})
        actual = report["batches"][0]["request"]["items"]
        self.assertEqual([i["track"]["id"] for i in actual], ["1", "1", "3"])

    def test_stop_does_not_fetch_future_pages(self):
        report = collect(self.api, "test", self.path, stop_after_batches=1)
        self.assertEqual(self.api.reads, 2)
        self.assertEqual(report["submitted_items"], 100)
        self.assertFalse(report["enumeration_finished"])

    def test_lost_reply_can_replay_same_request_and_key(self):
        self.api.lose_response = True
        with self.assertRaises(OSError):
            collect(self.api, "test", self.path)
        replay(self.api, self.path)
        replay(self.api, self.path)
        self.assertEqual(self.api.sizes, [100])
        self.assertEqual(self.api.reads, 2)

    def test_invalid_bound_and_existing_manifest(self):
        with self.assertRaises(ValueError):
            collect(self.api, "test", self.path, batch_size=101)
        collect(self.api, "test", self.path, max_items=1)
        with self.assertRaises(Failure):
            collect(self.api, "test", self.path)


if __name__ == "__main__":
    unittest.main()
