"""Verify published evidence and recompute every throughput comparison; no GPU access."""
from pathlib import Path
import hashlib
import json
import statistics
import argparse

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--require-tested-source",action="store_true",help="Also require current source to match the hardware-tested 0.1.2 source")
args=parser.parse_args()

ROOT=Path(__file__).resolve().parents[1]
def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

comparison=load(ROOT/"results/20261025-220124-082/comparison.json")
for name,h in comparison["sources"].items():
    assert sha(ROOT/name)==h, name
assert sha(ROOT/"REPORT.TXT")==comparison["report_sha256"]
datasets=[load(ROOT/name) for name in comparison["sources"]]
for n in (2048,4096,8192):
    for dtype in ("FP32_STRICT","FP16","BF16","FP8_E4M3"):
        rows=[next(r for r in d["rows"] if r["size"]==n and r["dtype"]==dtype) for d in datasets]
        assert all(r["status"]=="OK" and r["kernel"]==rows[0]["kernel"] for r in rows)
        for key in ("python","pytorch","torch_cuda","gpu","precision_settings"):
            assert all(d[key]==datasets[0][key] for d in datasets),key
        values=[r["tflops"] for r in rows]
        before,after=statistics.mean(values[:2]),statistics.mean(values[2:])
        gain=round((after/before-1)*100,2)
        expected=next(r for r in comparison["rows"] if r["size"]==n and r["dtype"]==dtype)
        assert values[:2]==expected["stock_tflops"] and values[2:]==expected["after_tflops"]
        assert gain==expected["gain_percent"]
        print(f"{n:4} {dtype:12} {before:.4f} -> {after:.4f} TFLOPS {gain:+.2f}%")
manifest=load(ROOT/"evidence/tested-stage3-0.1.2/manifest.json")
assert sha(ROOT/"artifacts/stage3-0.1.2/BOOTX64.EFI")==manifest["efi_sha256"]
changed=[name for name,h in manifest["source_sha256"].items() if not (ROOT/name).is_file() or sha(ROOT/name)!=h]
added=[str(p.relative_to(ROOT)).replace("\\","/") for p in (ROOT/"src").rglob("*") if p.is_file() and str(p.relative_to(ROOT)).replace("\\","/") not in manifest["source_sha256"]]
print("PASS: four benchmark files, report and archived hardware-tested EFI hashes")
if changed or added:
    print("CURRENT SOURCE DIFFERS from tested 0.1.2: "+", ".join(changed+added))
    print("Historical hardware/performance results do NOT validate the current candidate.")
if args.require_tested_source:
    assert not changed and not added,"Current source is not the tested 0.1.2 source"
