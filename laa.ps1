param([Parameter(Mandatory=$true)][string]$Path)
$ErrorActionPreference = 'Stop'
$Path = (Resolve-Path -LiteralPath $Path).Path

$fs = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'Read')
try {
    $br = New-Object System.IO.BinaryReader($fs)
    $bw = New-Object System.IO.BinaryWriter($fs)

    $fs.Position = 0x3C
    $pe = $br.ReadInt32()

    $fs.Position = $pe
    $sig = $br.ReadBytes(4)
    if ($sig[0] -ne 0x50 -or $sig[1] -ne 0x45 -or $sig[2] -ne 0 -or $sig[3] -ne 0) {
        throw "Invalid PE file"
    }

    $machine = $br.ReadUInt16()
    if ($machine -ne 0x014C) { throw ("not a 32-bit x86 exe (Machine=0x{0:X4})." -f $machine) }

    $fs.Position = $pe + 22
    $ch = $br.ReadUInt16()
    if ($ch -band 0x20) {
        Write-Host "LAA flag already set"
    } else {
        $bak = "$Path.bak"
        if (-not (Test-Path -LiteralPath $bak)) { 
            Copy-Item -LiteralPath $Path $bak
        }

        $fs.Position = $pe + 22
        $bw.Write([uint16]($ch -bor 0x20))
        Write-Host "LAA flag set"
    }
} finally {
    $fs.Close()
}
