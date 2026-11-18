# Возвращает меню загрузки SynthOS / Windows: ставит «SynthOS» первым в меню загрузки компьютера и заводит задачу,
# которая при каждом запуске Windows снова ставит его первым (Windows после своих обновлений ставит себя вперёд).
param([switch]$Yes)

$ErrorActionPreference = 'Stop'
function Done([string]$text, [string]$color) {
    Write-Host ''
    Write-Host $text -ForegroundColor $color
    if (-not $Yes) { Read-Host 'Нажмите Enter, чтобы закрыть' | Out-Null }
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Done 'Нужны права администратора: запустите «Repair boot menu.cmd».' Red
    exit 1
}

$entry = $null
$text = (& bcdedit.exe /enum firmware) -join "`n"
foreach ($block in ($text -split "`n\s*`n")) {
    if ($block -match '(?m)\sSynthOS\s*$') {
        $entry = [regex]::Match($block, '\{[0-9a-fA-F-]{36}\}').Value
        if ($entry) { break }
    }
}
if (-not $entry) {
    $esp = $null
    foreach ($l in [char[]]'ZYXWVUTSRQPONMLKJIHG') { if (-not (Test-Path "$($l):\")) { $esp = "$($l):"; break } }
    $loader = $null
    if ($esp) {
        mountvol $esp /S | Out-Null
        if (Test-Path "$esp\EFI\SynthOS\shimx64.efi") { $loader = '\EFI\SynthOS\shimx64.efi' }
        elseif (Test-Path "$esp\EFI\SynthOS\synthos.efi") { $loader = '\EFI\SynthOS\synthos.efi' }
        mountvol $esp /D | Out-Null
    }
    if (-not $loader) {
        Done 'SynthOS не найден в разделе EFI: поставьте его заново («Install SynthOS.cmd»).' Red
        exit 1
    }
    $out = (& bcdedit.exe /copy '{bootmgr}' /d 'SynthOS') -join ' '
    $entry = [regex]::Match($out, '\{[0-9a-fA-F-]{36}\}').Value
    if (-not $entry) { Done "bcdedit: $out" Red; exit 1 }
    & bcdedit.exe /set $entry path $loader | Out-Null
}

& bcdedit.exe /set '{fwbootmgr}' displayorder $entry /addfirst | Out-Null

$action = New-ScheduledTaskAction -Execute 'bcdedit.exe' -Argument "/set {fwbootmgr} displayorder $entry /addfirst"
$trigger = New-ScheduledTaskTrigger -AtStartup
$who = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName 'SynthOS boot menu' -Action $action -Trigger $trigger -Principal $who -Force | Out-Null

Done 'Готово: при включении компьютера снова будет меню SynthOS / Windows.' Green
