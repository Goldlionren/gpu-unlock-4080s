"""Verify published evidence and recompute every throughput comparison; no GPU access."""
from pathlib import Path
import hashlib
import json
import statistics

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
for name,h in manifest["source_sha256"].items():
    assert sha(ROOT/name)==h,name
print("PASS: four benchmark files, report, tested EFI and exact tested C source hashes")
