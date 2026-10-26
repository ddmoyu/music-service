"""Check generated OpenAPI structure, references and implementation route coverage."""
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("openapi_generator", ROOT / "packaging/generate_openapi.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
source = (ROOT / "docs/api/openapi.json").read_text(encoding="utf-8")
if source != generator.serialized():
    raise SystemExit("Run python -I packaging/generate_openapi.py after contract/version changes")
document = json.loads(source)


def walk(value):
    if isinstance(value, dict):
        if "$ref" in value:
            target = document
            for part in value["$ref"].removeprefix("#/").split("/"):
                target = target[part.replace("~1", "/").replace("~0", "~")]
        for child in value.values():
            walk(child)
    elif isinstance(value, list):
        for child in value:
            walk(child)


walk(document)
implementation = "\n".join(p.read_text(encoding="utf-8") for directory in ("src/api", "src/media_server", "src/event_server")
                           for p in (ROOT / directory).rglob("*.cpp"))
ids = set()
for path, methods in document["paths"].items():
    prefix = path.split("{")[0]
    assert prefix in implementation, f"route missing from implementation: {path}"
    for method, operation in methods.items():
        assert method in ("get", "post", "head")
        assert operation["operationId"] not in ids
        ids.add(operation["operationId"])
        assert operation["responses"]
        actual = {p["name"] for p in operation["parameters"] if p["in"] == "path" and p["required"]}
        assert actual == set(re.findall(r"{([^}]+)}", path)), path
        for parameter in operation["parameters"]:
            assert "schema" in parameter and "name" in parameter and "in" in parameter
assert document["components"]["schemas"]["DownloadRequest"]["properties"]["metadata_policy"]["enum"] == [
    "best_effort", "required", "skip"]
print(f"Core OpenAPI: {len(document['paths'])} paths, {len(ids)} operations, references and source routes checked")
