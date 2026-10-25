"""Measure current Tensor performance against this board's archived profiles.

This is an empirical indicator, not a register read or proof of unlock state.
Runs the unchanged baseline twice at n=8192; writes only LOCAL-unlock-status.
"""
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
DTYPES = ('FP16', 'BF16')
ENV_KEYS = ('python', 'pytorch', 'torch_cuda', 'gpu', 'compute_capability',
            'gpu_mem_total_mib', 'precision_settings')


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def row(data, dtype):
    rows = [r for r in data['rows'] if r['size'] == 8192 and r['dtype'] == dtype]
    if len(rows) != 1 or rows[0]['status'] != 'OK':
        raise ValueError('Missing or unsuccessful 8192 ' + dtype)
    r = rows[0]
    if not math.isfinite(r['tflops']) or r['tflops'] <= 0:
        raise ValueError('Invalid throughput')
    return r


def hardware(data):
    # Ignore only free VRAM; retain PCI identity, driver, VBIOS and power limit.
    parts = [p.strip() for p in data['smi'].split(',')]
    if len(parts) != 8 or '\n' in data['smi']:
        raise ValueError('Requires exactly one GPU and successful nvidia-smi capture')
    return parts[:6] + parts[7:]


def classify(current, stock, unlocked):
    reasons = []
    table = []
    if len(current) != 2 or len(stock) != 2 or len(unlocked) != 2:
        raise ValueError('Two samples per profile required')
    reference = stock[0]
    for data in stock + unlocked + current:
        for key in ENV_KEYS:
            if data.get(key) != reference.get(key):
                reasons.append('Environment differs: ' + key)
        if hardware(data) != hardware(reference):
            reasons.append('GPU/driver/VBIOS/power-limit context differs')
    matches = {'stock': True, 'unlocked': True}
    for dtype in DTYPES:
        rr = row(reference, dtype)
        for data in stock + unlocked + current:
            r = row(data, dtype)
            for key in ('kernel', 'repetitions_per_trial', 'trials'):
                if r[key] != rr[key]:
                    reasons.append(dtype + ' benchmark configuration differs: ' + key)
        values = [row(d, dtype)['tflops'] for d in current]
        mean = statistics.mean(values)
        if max(values) / min(values) > 1.10:
            reasons.append(dtype + ' differs by more than 10% between runs')
        base = statistics.mean(row(d, dtype)['tflops'] for d in stock)
        after = statistics.mean(row(d, dtype)['tflops'] for d in unlocked)
        matches['stock'] &= all(abs(v / base - 1) <= .15 for v in values)
        matches['unlocked'] &= all(abs(v / after - 1) <= .15 for v in values)
        table.append({'dtype': dtype, 'current_tflops': values,
                      'mean_tflops': mean, 'stock_mean_tflops': base,
                      'unlocked_mean_tflops': after,
                      'gain_vs_stock_percent': (mean / base - 1) * 100})
    status = 'INCONCLUSIVE'
    if not reasons and matches['unlocked'] and not matches['stock']:
        status = 'UNLOCKED_PERFORMANCE_LIKELY'
    elif not reasons and matches['stock'] and not matches['unlocked']:
        status = 'STOCK_PERFORMANCE_LIKELY'
    elif not reasons:
        reasons.append('Performance does not uniquely match one historical profile')
    return {'status': status, 'hardware_unlock_state_verified': False,
            'method': 'Both runs of FP16 and BF16 within 15% of one profile; run spread <=10%. Empirical thresholds, not calibrated confidence.',
            'reasons': sorted(set(reasons)), 'rows': table,
            'limitations': 'No current SS0/SS1 read. Load, throttling or kernel selection may alter results; stock-like speed cannot prove locked state. Same API label does not prove identical machine kernel.'}


def main():
    output = ROOT / 'LOCAL-unlock-status' / (
        dt.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    output.mkdir(parents=True)
    print('Results: ' + str(output), flush=True)
    print('Measuring current GPU twice; close ComfyUI jobs/games first. No firmware, driver or boot changes.', flush=True)
    try:
        comparison = load(ROOT / 'results/20261025-220124-082/comparison.json')
        for name, digest in comparison['sources'].items():
            if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
                raise ValueError('Historical reference hash mismatch: ' + name)
        stock = [load(ROOT / ('rtx4080s_tensor_stock_20261025_' + t + '.json'))
                 for t in ('171931', '172003')]
        unlocked = [load(ROOT / f'results/20261025-220124-082/benchmark-{i}.json') for i in (1, 2)]
        current = []
        for i in (1, 2):
            run = output / f'run-{i}'
            run.mkdir()
            command = [sys.executable, str(ROOT / 'rtx4080s_tensor_baseline.py'), '--sizes', '8192']
            with (run / 'benchmark.log').open('w', encoding='utf-8') as log:
                result = subprocess.run(command, cwd=run, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=180)
            files = list(run.glob('rtx4080s_tensor_stock_*.json'))
            if result.returncode != 0 or len(files) != 1:
                raise RuntimeError('Benchmark failed; inspect ' + str(run / 'benchmark.log'))
            current.append(load(files[0]))
            print(f'Run {i}/2 completed.', flush=True)
        verdict = classify(current, stock, unlocked)
        verdict['measurement_started_at'] = current[0]['timestamp']
        verdict['benchmark_sha256'] = hashlib.sha256((ROOT / 'rtx4080s_tensor_baseline.py').read_bytes()).hexdigest()
        (output / 'status.json').write_text(json.dumps(verdict, indent=2) + '\n', encoding='utf-8')
        print('STATUS: ' + verdict['status'])
        for r in verdict['rows']:
            print(f"{r['dtype']}: current={r['mean_tflops']:.2f}, stock={r['stock_mean_tflops']:.2f}, unlocked-reference={r['unlocked_mean_tflops']:.2f} TFLOPS")
        for reason in verdict['reasons']:
            print('Reason: ' + reason)
        print('Empirical performance indication only; hardware unlock bit/register not read.')
        print('Status file: ' + str(output / 'status.json'))
    except Exception as exc:
        (output / 'error.txt').write_text(str(exc) + '\n', encoding='utf-8')
        print('ERROR: ' + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
