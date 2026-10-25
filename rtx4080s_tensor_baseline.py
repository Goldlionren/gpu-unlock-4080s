#!/usr/bin/env python3
"""Windows RTX 4080 SUPER CUDA Tensor baseline; no unlock or firmware writes.

Uses the active PyTorch environment, CUDA events, and PyTorch/cuBLAS kernels.
FP8 is *optional* (torch._scaled_mm): unsupported API / kernel errors are logged,
not interpreted as a hardware limitation. Does not alter persistent settings.
"""
import argparse
import datetime as dt
import json
import math
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path

try:
    import torch
except Exception as exc:
    print(f"ERROR importing torch: {type(exc).__name__}: {exc}")
    sys.exit(2)


def smi():
    keys = ["name", "pci.device_id", "pci.sub_device_id", "driver_version",
            "vbios_version", "memory.total", "memory.free", "power.limit"]
    try:
        p = subprocess.run(
            ["nvidia-smi", "--query-gpu=" + ",".join(keys), "--format=csv,noheader"],
            capture_output=True, text=True, timeout=8, check=True)
        return p.stdout.strip()
    except Exception as exc:
        return f"unavailable: {type(exc).__name__}: {str(exc)[:150]}"


def benchmark(func, n, reps, trials, warmups):
    for _ in range(warmups):
        func()
    torch.cuda.synchronize()
    durations = []
    # Each trial batches multiple launches to amortize host scheduling jitter.
    for _ in range(trials):
        beg, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
        beg.record()
        for __ in range(reps):
            func()
        end.record()
        end.synchronize()
        ms = beg.elapsed_time(end) / reps
        durations.append(ms)
    med = statistics.median(durations)
    tflops = (2.0 * n**3) / (med * 1e-3) / 1e12
    return {
        "median_ms": round(med, 5),
        "tflops": round(tflops, 3),
        "repetitions_per_trial": reps,
        "trials": trials,
        "trial_ms": [round(x, 5) for x in durations],
    }


def fp8_callable(n, rand_a, rand_b):
    if not hasattr(torch, "float8_e4m3fn"):
        raise RuntimeError("torch.float8_e4m3fn is missing")
    if not hasattr(torch, "_scaled_mm"):
        raise RuntimeError("torch._scaled_mm is missing")
    a = rand_a.to(dtype=torch.float8_e4m3fn).contiguous()
    # Column-major B is supported by the widest range of CUDA FP8 GEMM paths.
    # B's mathematical values are intentionally transposed for this benchmark.
    b = rand_b.to(dtype=torch.float8_e4m3fn).transpose(0, 1)
    scale_a = torch.tensor(1., device="cuda", dtype=torch.float32)
    scale_b = torch.tensor(1., device="cuda", dtype=torch.float32)
    def op():
        return torch._scaled_mm(a, b, scale_a=scale_a, scale_b=scale_b,
                                out_dtype=torch.bfloat16, use_fast_accum=False)
    op()
    torch.cuda.synchronize()
    return op


def main():
    parser = argparse.ArgumentParser(description="Read-only RTX 4080 SUPER tensor baseline")
    parser.add_argument("--sizes", type=int, nargs="+", default=[2048, 4096, 8192])
    parser.add_argument("--reps", type=int, default=15)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=8)
    args = parser.parse_args()
    if not torch.cuda.is_available():
        sys.exit("CUDA is not available in this PyTorch environment")
    if args.reps < 1 or args.trials < 1 or args.warmups < 0:
        sys.exit("invalid repetition/trial/warmup count")
    if any(n < 256 or n > 8192 or n % 256 for n in args.sizes):
        sys.exit("sizes must be multiples of 256 between 256 and 8192")

    torch.manual_seed(4080)
    torch.cuda.set_device(0)
    # The FP32 reference/control should never silently take TF32 shortcuts.
    torch.set_float32_matmul_precision("highest")
    torch.backends.cuda.matmul.allow_tf32 = False

    gpu = torch.cuda.get_device_properties(0)
    stamp = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    precision = {
        "float32_matmul_precision": torch.get_float32_matmul_precision(),
        "allow_tf32": bool(torch.backends.cuda.matmul.allow_tf32),
        "allow_fp16_reduced_precision_reduction": bool(torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction),
        "allow_bf16_reduced_precision_reduction": bool(torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction),
    }
    info = {
        "timestamp": stamp,
        "machine": platform.platform(),
        "python": sys.version.split()[0],
        "pytorch": torch.__version__,
        "torch_cuda": torch.version.cuda,
        "gpu": gpu.name,
        "compute_capability": list(torch.cuda.get_device_capability(0)),
        "gpu_mem_total_mib": round(gpu.total_memory / 1024**2),
        "smi": smi(),
        "precision_settings": precision,
        "benchmark": "PyTorch mm / torch._scaled_mm, CUDA events; no unlock, no altered model precision",
        "rows": [],
    }
    print("=== RTX 4080 SUPER STOCK TENSOR BASELINE ===", flush=True)
    print("Timestamp:", stamp, "GPU:", gpu.name, "CC:", torch.cuda.get_device_capability(0))
    print("Torch:", torch.__version__, "CUDA:", torch.version.cuda)
    print("nvidia-smi:", info["smi"])
    print("Precision:", precision)
    print("Each row: median per GEMM from", args.trials, "trials x", args.reps, "iterations")
    print("WARNING: BF16/FP16/FP8 compare only against the SAME dtype + kernel after unlock.", flush=True)

    try:
        for n in args.sizes:
            print("\n--- Square GEMM n =", n, "---", flush=True)
            a0 = torch.randn((n, n), dtype=torch.float32, device="cuda") * 0.125
            b0 = torch.randn((n, n), dtype=torch.float32, device="cuda") * 0.125
            for label, dtype in (("FP32_STRICT", torch.float32),
                                 ("FP16", torch.float16),
                                 ("BF16", torch.bfloat16)):
                a = a0.to(dtype)
                b = b0.to(dtype)
                out = torch.empty_like(a)
                try:
                    def fn():
                        torch.mm(a, b, out=out)
                        return out
                    r = benchmark(fn, n, args.reps, args.trials, args.warmups)
                    r.update({"size": n, "dtype": label, "status": "OK", "kernel": "torch.mm"})
                    print(f"{label:13s} {r['median_ms']:10.4f} ms {r['tflops']:10.2f} TFLOPS", flush=True)
                except (RuntimeError, ValueError, TypeError) as exc:
                    r = {"size": n, "dtype": label, "status": "ERROR", "error": str(exc)[:500]}
                    print(label, "ERROR:", str(exc)[:180], flush=True)
                info["rows"].append(r)
                del a, b, out
            try:
                op = fp8_callable(n, a0, b0)
                r = benchmark(op, n, args.reps, args.trials, args.warmups)
                r.update({"size": n, "dtype": "FP8_E4M3", "status": "OK",
                          "kernel": "torch._scaled_mm, BF16 output, fast_accum=False; includes cached output allocation"})
                print(f"{'FP8_E4M3':13s} {r['median_ms']:10.4f} ms {r['tflops']:10.2f} TFLOPS", flush=True)
                del op
            except (RuntimeError, ValueError, TypeError, AttributeError) as exc:
                r = {"size": n, "dtype": "FP8_E4M3", "status": "UNAVAILABLE",
                     "error": str(exc)[:800]}
                print("FP8_E4M3  UNAVAILABLE:", str(exc).splitlines()[0][:230], flush=True)
                # CUDA may have latched an async error. Do not force later tests.
                try:
                    torch.cuda.synchronize()
                except Exception:
                    print("CUDA reported a context error: stop instead of continuing.", flush=True)
                    info["rows"].append(r)
                    break
            info["rows"].append(r)
            del a0, b0
            torch.cuda.empty_cache()
    finally:
        filename = "rtx4080s_tensor_stock_" + dt.datetime.now().strftime("%Y%m%d_%H%M%S") + ".json"
        path = Path.cwd() / filename
        path.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
        print("\nRESULT FILE:", path, flush=True)
        print("Run a second untouched baseline later to verify measurement repeatability.", flush=True)


if __name__ == "__main__":
    main()
