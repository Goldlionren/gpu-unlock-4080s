"""Host-only checks for measured 16GB layout and rejected malformed geometries."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
evidence = json.loads((ROOT / 'evidence/assets-layout.json').read_text())
base = evidence['layout_input']
exe = ROOT / 'offline_layout_build/offline_layout.exe'


def run(values):
    result = subprocess.run([str(exe), *[str(int(v)) for v in values.values()]], capture_output=True, text=True, timeout=20)
    data = json.loads(result.stdout)
    assert (result.returncode == 0) == (data['return_code'] == 0)
    return data


measured = run(base)
assert measured == evidence['layout']
assert measured['output'] == {
    'reserved_top': 0x3ff700000, 'wpr_end': 0x3ff700000, 'frts_offset': 0x3ff600000,
    'boot_offset': 0x3ff5f7000, 'gsp_offset': 0x3fa5a0000, 'heap_offset': 0x3f4d00000,
    'heap_size': 0x5800000, 'wpr_start': 0x3f4c00000, 'nonwpr_start': 0x3f4b00000,
}
bad = [
    {'usable_fb': 0}, {'vga_base': 0}, {'frts_size': 0}, {'boot_size': 0}, {'image_size': 0},
    {'vga_base': base['usable_fb'] + 0x100000}, {'frts_size': base['usable_fb']},
    {'boot_size': base['usable_fb']}, {'image_size': base['usable_fb']},
    {'heap_request': (1 << 64) - 1}, {'nonwpr_size': (1 << 64) - 1},
    {'mmu_lock_valid': True, 'mmu_lock_low': 0x10000},
]
for changes in bad:
    assert run({**base, **changes})['return_code'] != 0, changes
constrained = run({**base, 'mmu_lock_valid': True, 'mmu_lock_low': base['vga_base'] - 0x100000})
assert constrained['return_code'] == 0
assert constrained['output']['wpr_end'] == base['vga_base'] - 0x100000
report = dict(measured_layout_passed=True, malformed_layout_cases_rejected=len(bad),
              top_lock_case_passed=True, hardware_execution=False)
(ROOT / 'evidence/assets-layout-validation.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report))
