"""Download the classic Khronos/Crytek Sponza, including every glTF dependency."""

import concurrent.futures
import datetime
import hashlib
import json
from pathlib import Path
import time
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[1] / "assets/sponza"
BASE = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/"
MODEL = "Models/Sponza/"


def download(remote, local):
    target = (ROOT / local).resolve()
    if not target.is_relative_to(ROOT.resolve()):
        raise ValueError(f"Path outside asset directory: {local}")
    target.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(4):
        try:
            request = urllib.request.Request(BASE + remote, headers={"User-Agent": "SponzaAssetDownloader/1.0"})
            with urllib.request.urlopen(request, timeout=45) as response:
                data = response.read()
            if not data or data.startswith(b"version https://git-lfs.github.com/spec/"):
                raise ValueError(f"Empty asset or unresolved Git LFS pointer: {remote}")
            temporary = target.with_name(target.name + ".partial")
            temporary.write_bytes(data)
            temporary.replace(target)
            return {"path": local, "source": BASE + remote, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        except Exception:
            if attempt == 3:
                raise
            time.sleep(attempt + 1)


def main():
    entries = [download(MODEL + "glTF/Sponza.gltf", "glTF/Sponza.gltf")]
    model = json.loads((ROOT / "glTF/Sponza.gltf").read_text(encoding="utf-8"))
    uris = sorted({item["uri"] for kind in ("buffers", "images") for item in model.get(kind, []) if "uri" in item})
    tasks = []
    for uri in uris:
        parsed = urllib.parse.urlsplit(uri)
        if parsed.scheme or parsed.netloc or parsed.query or parsed.fragment:
            raise ValueError(f"Unexpected external/data URI: {uri}")
        tasks.append((MODEL + "glTF/" + uri, "glTF/" + urllib.parse.unquote(parsed.path)))
    tasks += [(MODEL + name, name) for name in ("README.md", "LICENSE.md", "metadata.json")]
    tasks.append(("LICENSES/LicenseRef-CRYENGINE-Agreement.txt", "licenses/LicenseRef-CRYENGINE-Agreement.txt"))
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
        futures = [executor.submit(download, *task) for task in tasks]
        for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
            entry = future.result()
            entries.append(entry)
            print(f"[{index}/{len(tasks)}] {entry['path']} ({entry['bytes']} bytes)", flush=True)
    for buffer in model.get("buffers", []):
        actual = (ROOT / "glTF" / urllib.parse.unquote(buffer["uri"])).stat().st_size
        if actual != buffer["byteLength"]:
            raise ValueError(f"Buffer length mismatch: {actual} != {buffer['byteLength']}")
    manifest = {
        "retrieved_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source": "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza",
        "revision": "main (commit lookup unavailable; exact downloaded content recorded by SHA-256)",
        "asset": "Classic Crytek Sponza, glTF 2.0",
        "file_count": len(entries),
        "total_bytes": sum(item["bytes"] for item in entries),
        "files": sorted(entries, key=lambda item: item["path"]),
    }
    (ROOT / "download-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Complete: {manifest['file_count']} files, {manifest['total_bytes'] / 1048576:.2f} MiB", flush=True)


if __name__ == "__main__":
    main()
