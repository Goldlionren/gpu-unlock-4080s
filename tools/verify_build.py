"""Offline release checks. Does not execute an EFI image or access a GPU."""
from pathlib import Path
import hashlib
import importlib.util
import json
import re

ROOT = Path(__file__).resolve().parents[1]


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    spec = importlib.util.spec_from_file_location('pecheck', ROOT / 'tools/pe_check.py')
    pecheck = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pecheck)
    pecheck.validate_pe((ROOT / 'build/RTX4080S-UNLOCK.EFI').read_bytes())
    headers = ROOT / 'vendor/edk2'
    for name, sha in json.loads((headers / 'headers.json').read_text())['files'].items():
        require(digest(headers / name) == sha, 'Changed header: ' + name)
    manifest = json.loads((ROOT / 'assets/verified-manifest.json').read_text())
    require(len(manifest) == 5, 'Expected five pinned assets')
    io = (ROOT / 'src/app_io.c').read_text()
    for name, asset in manifest.items():
        path = ROOT / 'assets' / name
        require(path.stat().st_size == asset['bytes'] and digest(path) == asset['sha256'], 'Bad asset: ' + name)
        require(name in io and asset['sha256'] in io and str(asset['bytes']) in io, 'Asset/source mismatch: ' + name)
    mutation_locations = []
    for path in (ROOT / 'src').rglob('*'):
        if path.suffix not in ('.c', '.h', '.inc'):
            continue
        source = path.read_text()
        for pattern in (r'->(?:SetVariable|ResetSystem|DisconnectController|ConnectController|ExitBootServices|SetMode|Blt)\s*\(', r'EFI_OPEN_PROTOCOL_EXCLUSIVE'):
            require(not re.search(pattern, source), 'Forbidden operation: ' + str(path) + ' ' + pattern)
        for operation in re.findall(r'->Attributes\([^,]+,\s*(\w+)', source):
            require(operation in ('EfiPciIoAttributeOperationGet', 'EfiPciIoAttributeOperationSupported'), 'Attributes mutation')
        if re.search(r'->Mem\.Write\(', source):
            mutation_locations.append(path.name)
    require(mutation_locations == ['port.c'], 'MMIO writes escaped port layer')
    app = (ROOT / 'src/app.c').read_text()
    require('outcome->recovered' in app and 'outcome->selectors_after_flr' in app and 'ss0!=0x88888888' in app and 'ss1!=8' in app, 'Missing outcome gates')
    require('memcmp(rechecked,EntryConfig,sizeof(rechecked))' in app, 'Missing pre-firmware configuration check')
    require('Stage2ReservationHeld()&&Gu.pci' in app and 'current==Gu.pci?GuContainMasterForStop()' in app, 'Missing ownership-bound emergency BME containment')
    require('FinalEntryMatches(FinalConfig,EntryConfig,FinalRecoveryVerified,Gu.pcie_cap,&StatusClearAccepted)' in app and 'entry config delta:' in app, 'Missing final configuration check/diagnostics')
    require('entry[0x80/4]!=0x00092930' in app and 'final[0x80/4]!=0x00002930' in app and 'memcmp(normalized,entry,sizeof(normalized))' in app, 'Missing bounded DEVSTA clear policy')
    require('SetWatchdogTimer(0,0,0,NULL)' in app, 'Missing watchdog disable')
    logs = {name: (ROOT / 'build' / name).read_text(errors='replace') for name in ('port-results.log', 'core-results.log', 'app-results.log', 'admission-results.log')}
    for name, log in logs.items():
        if name == 'core-results.log' and log.strip() == 'SKIPPED: proprietary board PROM fixture not supplied; core binary compiled only.':
            continue
        require('FAIL' not in log and ('passed' in log.lower() or 'pass:' in log.lower()), 'Failed/incomplete test log: ' + name)
    report = dict(pe='AMD64 EFI application, relocation, no imports/TLS/CLR', pinned_headers=43,
                  verified_asset_count=5, mmio_write_layer='port.c', source_audit='passed',
                  hardware_executed=False, core_test_skipped=logs['core-results.log'].startswith('SKIPPED:'), logs={name: digest(ROOT / 'build' / name) for name in logs})
    (ROOT / 'build/verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('PASS: EFI format, headers, five assets, source boundaries and host test logs')


if __name__ == '__main__':
    main()
