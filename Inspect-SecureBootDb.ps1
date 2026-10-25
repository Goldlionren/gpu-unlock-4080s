[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$CaptureDirectory)
$ErrorActionPreference = 'Stop'
$CaptureDirectory = (Resolve-Path -LiteralPath $CaptureDirectory).Path
$preflight = Get-Content -LiteralPath (Join-Path $CaptureDirectory 'preflight.json') -Raw | ConvertFrom-Json
$result = @(
    foreach ($name in @('PK','KEK','db','dbx','PKDefault','KEKDefault','dbDefault','dbxDefault')) {
        $path = Join-Path $CaptureDirectory ($name + '.bin')
        $record = @($preflight.variables | Where-Object name -eq $name)
        if ($record.Count -ne 1 -or $record[0].error) { throw ('Missing captured variable: ' + $name) }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record[0].sha256) { throw ('Capture hash mismatch: ' + $name) }
        [byte[]]$bytes = [IO.File]::ReadAllBytes($path)
        [long]$offset = 0
        $entries = @(
            while ($offset -lt $bytes.Length) {
                if ($bytes.Length - $offset -lt 28) { throw ('Truncated signature list: ' + $name) }
                $type = [Guid]::new([byte[]]$bytes[$offset..($offset+15)]).ToString()
                [long]$listSize = [BitConverter]::ToUInt32($bytes, $offset+16)
                [long]$headerSize = [BitConverter]::ToUInt32($bytes, $offset+20)
                [long]$sigSize = [BitConverter]::ToUInt32($bytes, $offset+24)
                if ($listSize -lt 28 -or $listSize -gt $bytes.Length-$offset -or $headerSize -gt $listSize-28 -or $sigSize -le 16 -or (($listSize-28-$headerSize) % $sigSize)) { throw ('Malformed signature list: ' + $name) }
                for ($pos=$offset+28+$headerSize; $pos -lt $offset+$listSize; $pos+=$sigSize) {
                    $owner = [Guid]::new([byte[]]$bytes[$pos..($pos+15)]).ToString()
                    [byte[]]$data = $bytes[($pos+16)..($pos+$sigSize-1)]
                    $hasher = [Security.Cryptography.SHA256]::Create()
                    try { $hash = [BitConverter]::ToString($hasher.ComputeHash($data)).Replace('-','').ToLowerInvariant() } finally { $hasher.Dispose() }
                    $entry = [ordered]@{ type=$type; owner=$owner; bytes=$data.Length; data_sha256=$hash }
                    if ($type -eq 'a5c059a1-94e4-4aa7-87b5-ab155c2bf072') {
                        $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($data)
                        try {
                            $entry.subject=$cert.Subject; $entry.issuer=$cert.Issuer
                            $entry.thumbprint=$cert.Thumbprint
                            $entry.not_before=$cert.NotBefore.ToUniversalTime().ToString('o')
                            $entry.not_after=$cert.NotAfter.ToUniversalTime().ToString('o')
                        } finally { $cert.Dispose() }
                    }
                    [pscustomobject]$entry
                }
                $offset += $listSize
            }
        )
        [pscustomobject]@{ name=$name; signature_count=$entries.Count; entries=$entries }
    }
)
$differences = @(
    foreach ($name in @('PK','KEK','db','dbx')) {
        $current = @((($result | Where-Object name -eq $name).entries) | ForEach-Object { $_.type + ':' + $_.data_sha256 })
        $defaults = @((($result | Where-Object name -eq ($name+'Default')).entries) | ForEach-Object { $_.type + ':' + $_.data_sha256 })
        [pscustomobject]@{ name=$name; added_vs_default=@($current | Where-Object { $_ -notin $defaults }); absent_vs_default=@($defaults | Where-Object { $_ -notin $current }) }
    }
)
$dest = Join-Path $CaptureDirectory 'trust-analysis.json'
[ordered]@{ variables=$result; differences=$differences; note='Offline signature-list parsing only; no chain verification or firmware writes.' } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $dest -Encoding utf8
$result | ForEach-Object {
    Write-Output ($_.name + ': ' + $_.signature_count + ' entries')
    $_.entries | Where-Object subject | ForEach-Object { Write-Output ('  ' + $_.subject) }
}
$differences | ForEach-Object { Write-Output ($_.name + ': added=' + $_.added_vs_default.Count + ' absent=' + $_.absent_vs_default.Count) }
Write-Output ('Report: ' + $dest)
