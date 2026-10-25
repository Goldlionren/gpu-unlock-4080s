"""Fetch unchanged firmware assets from a pinned upstream revision; never execute them."""
from pathlib import Path
import argparse
import hashlib
import json
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REVISION = "51df9c17ec89eb8486df52cc9079dde4bd1f9974"
BASE = "https://huggingface.co/TOTORONG/AD103_win_unlocker/resolve/" + REVISION + "/USB/RTX4080-EXPERIMENT/ASSETS/"

def valid(path, spec):
    return path.is_file() and path.stat().st_size == spec["bytes"] and hashlib.sha256(path.read_bytes()).hexdigest() == spec["sha256"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    manifest = json.loads((ROOT/"assets/verified-manifest.json").read_text(encoding="utf-8"))
    for name, spec in manifest.items():
        path = ROOT/"assets"/name
        if not valid(path, spec):
            if args.check_only:
                raise SystemExit("Missing or mismatched asset: " + name)
            partial = path.with_suffix(".bin.partial")
            try:
                request = urllib.request.Request(BASE+name, headers={"User-Agent":"gpu-unlock-4080s-asset-fetch/1"})
                with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as output:
                    total = 0
                    while chunk := response.read(1024*1024):
                        total += len(chunk)
                        if total > spec["bytes"]:
                            raise ValueError("Asset exceeds pinned length: "+name)
                        output.write(chunk)
                if not valid(partial, spec):
                    raise ValueError("Asset SHA256/length mismatch: "+name)
                partial.replace(path)
            finally:
                partial.unlink(missing_ok=True)
        print("VERIFIED " + name + " " + spec["sha256"])

if __name__ == "__main__":
    main()
