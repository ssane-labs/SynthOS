# Удаляет SynthOS, поставленный «Install SynthOS.cmd»: раздел SynthOS, файлы в разделе EFI и пункт меню загрузки;
# освободившееся место возвращается диску C:. Параметр -Yes — не задавать вопросов.
param([switch]$Yes)

$ErrorActionPreference = 'Stop'
$SynthType = '{53594e54-484f-4f53-8053-796e74684f53}'

function Fail([string]$text) {
    Write-Host "Не получилось: $text" -ForegroundColor Red
    if (-not $Yes) { Read-Host 'Нажмите Enter, чтобы закрыть' | Out-Null }
    exit 1
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'нужны права администратора: запустите «Uninstall SynthOS.cmd».'
}
$letter = $env:SystemDrive.Substring(0, 1)
$cpart = Get-Partition -DriveLetter $letter
$parts = @(Get-Partition -DiskNumber $cpart.DiskNumber | Where-Object { $_.GptType -eq $SynthType })
Write-Host 'SynthOS: удаление' -ForegroundColor White
foreach ($p in $parts) { Write-Host ("  раздел {0}: {1:N1} ГБ" -f $p.PartitionNumber, ($p.Size / 1GB)) }
Write-Host '  файлы \EFI\SynthOS и \SYNTHOS в разделе EFI, пункт «SynthOS» в меню загрузки'
if (-not $Yes) {
    $answer = Read-Host 'Все файлы SynthOS будут стёрты. Продолжить? [y/n]'
    if ($answer -notmatch '^(y|yes|д|да)$') { Fail 'отменено.' }
}

$text = (& bcdedit.exe /enum firmware) -join "`n"
foreach ($block in ($text -split "`n\s*`n")) {
    if ($block -match '(?m)\sSynthOS\s*$') {
        $entry = [regex]::Match($block, '\{[0-9a-fA-F-]{36}\}').Value
        if ($entry) { & bcdedit.exe /delete $entry | Out-Null }
    }
}

$esp = $null
foreach ($l in [char[]]'ZYXWVUTSRQPONMLKJIHG') { if (-not (Test-Path "$($l):\")) { $esp = "$($l):"; break } }
if ($esp) {
    mountvol $esp /S | Out-Null
    foreach ($dir in "$esp\EFI\SynthOS", "$esp\SYNTHOS") {
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    }
    mountvol $esp /D | Out-Null
}

foreach ($p in $parts) { Remove-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -Confirm:$false }
try {
    $max = (Get-PartitionSupportedSize -DriveLetter $letter).SizeMax
    if ($max -gt $cpart.Size) { Resize-Partition -DriveLetter $letter -Size $max }
} catch {
    Write-Host "Место не присоединилось к $($letter): само — это можно сделать в «Управлении дисками» (Расширить том)." -ForegroundColor Yellow
}
Write-Host 'SynthOS удалён.' -ForegroundColor Green
if (-not $Yes) { Read-Host 'Нажмите Enter, чтобы закрыть' | Out-Null }
