# Ставит SynthOS рядом с Windows прямо из Windows: без флешки и без настроек прошивки.
#   - уменьшает диск C: и создаёт в освободившемся месте раздел SynthOS (система, Chromium, ваши файлы);
#   - кладёт загрузчик в раздел EFI Windows (\EFI\SynthOS) рядом с ядром (\SYNTHOS);
#   - добавляет «SynthOS» в меню загрузки компьютера и один раз запускает его при следующей перезагрузке.
# Параметры: -SizeGB N (сколько гигабайт отдать SynthOS), -Yes (не задавать вопросов), -NoRestart.
param([int]$SizeGB = 0, [switch]$Yes, [switch]$NoRestart)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$SynthType = '{53594e54-484f-4f53-8053-796e74684f53}'
$EspType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$BootArea = 8192
$MinData = 8192

function Say([string]$text) { Write-Host $text }
function Step([string]$text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Fail([string]$text) {
    Write-Host ''
    Write-Host "Установка не выполнена: $text" -ForegroundColor Red
    if (-not $Yes) { Read-Host 'Нажмите Enter, чтобы закрыть' | Out-Null }
    exit 1
}
function Ask([string]$question) {
    if ($Yes) { return $true }
    $answer = Read-Host "$question [y/n]"
    return $answer -match '^(y|yes|д|да)$'
}

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class SynthDisk {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition,
                                            uint flags, IntPtr template);
    public static FileStream Open(int disk) {
        SafeFileHandle h = CreateFile(@"\\.\PhysicalDrive" + disk, 0xC0000000u, 3u, IntPtr.Zero, 3u, 0u, IntPtr.Zero);
        if (h.IsInvalid)
            throw new IOException("PhysicalDrive" + disk + ": error " + Marshal.GetLastWin32Error());
        return new FileStream(h, FileAccess.ReadWrite, 1);
    }
}
'@

function Read-Exact($stream, [byte[]]$buffer, [int]$count) {
    $got = 0
    while ($got -lt $count) {
        $n = $stream.Read($buffer, $got, $count - $got)
        if ($n -le 0) { throw 'файл system.img короче, чем нужно' }
        $got += $n
    }
}

function Copy-Region($src, $dst, [long]$from, [long]$to, [long]$bytes, [string]$what) {
    $buffer = New-Object byte[] (4MB)
    $src.Position = $from
    $dst.Position = $to
    $total = $bytes
    while ($bytes -gt 0) {
        $n = [int][Math]::Min($bytes, [long]$buffer.Length)
        Read-Exact $src $buffer $n
        $dst.Write($buffer, 0, $n)
        $bytes -= $n
        Write-Progress -Activity 'Копирование SynthOS' -Status $what -PercentComplete ([int](100 * ($total - $bytes) / $total))
    }
    Write-Progress -Activity 'Копирование SynthOS' -Completed
}

Write-Host 'SynthOS: установка рядом с Windows' -ForegroundColor White

# 1. Проверки
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'нужны права администратора: запустите «Install SynthOS.cmd» (он сам их попросит).'
}
if ($env:firmware_type -and $env:firmware_type -ne 'UEFI') {
    Fail 'Windows запущена в старом режиме BIOS (Legacy/CSM). Так можно поставить SynthOS только с флешки на отдельный диск (docs/INSTALL.md).'
}
$files = @('system.img', 'EFI\KERNEL.BIN', 'EFI\grubx64.efi')
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $here $f))) { Fail "рядом со скриптом нет $f — распакуйте архив целиком." }
}
$shim = Test-Path (Join-Path $here 'EFI\shimx64.efi')
Get-ChildItem -Path $here -Recurse -File | Unblock-File -ErrorAction SilentlyContinue

$letter = $env:SystemDrive.Substring(0, 1)
$cpart = Get-Partition -DriveLetter $letter
$disk = Get-Disk -Number $cpart.DiskNumber
if ($disk.PartitionStyle -ne 'GPT') { Fail 'диск с Windows размечен как MBR, а не GPT.' }
if ($disk.LogicalSectorSize -ne 512) { Fail "у диска сектор $($disk.LogicalSectorSize) байт; SynthOS пока работает только с 512." }

$secure = $false
try { $secure = Confirm-SecureBootUEFI } catch { $secure = $false }

# 2. Образ системы: загрузочная область, раздел EFI, пакет программ
$img = [IO.File]::OpenRead((Join-Path $here 'system.img'))
$sector0 = New-Object byte[] 512
Read-Exact $img $sector0 512
if ([Text.Encoding]::ASCII.GetString($sector0, 416, 4) -ne 'SYID') { Fail 'system.img повреждён (нет записи SynthOS).' }
$systemSectors = if ([BitConverter]::ToUInt32($sector0, 428) -band 2) { 32768 } else { 16384 }
$head = New-Object byte[] 16
$img.Position = [long]$systemSectors * 512
Read-Exact $img $head 16
$bundle = 0
if ([Text.Encoding]::ASCII.GetString($head, 0, 8) -eq 'SYNTHBDL') { $bundle = [BitConverter]::ToUInt32($head, 12) }
if ($img.Length -lt ([long]$systemSectors + $bundle) * 512) { Fail 'system.img обрезан — скачайте архив заново.' }

# 3. Сколько места отдать
$need = [long]($systemSectors + $bundle + $MinData) * 512
$minGB = [int][Math]::Ceiling($need / 1GB) + 1
$range = Get-PartitionSupportedSize -DriveLetter $letter
$volume = Get-Volume -DriveLetter $letter
$canShrink = [Math]::Min($cpart.Size - $range.SizeMin, $volume.SizeRemaining - 15GB)
$maxGB = [int][Math]::Floor($canShrink / 1GB)
if ($maxGB -lt $minGB) {
    Fail ("на диске $($letter): можно освободить только $maxGB ГБ, а нужно хотя бы $minGB ГБ (и 15 ГБ остаются Windows). " +
          'Освободите место или сожмите том вручную: Win+X → «Управление дисками» → C: → «Сжать том».')
}
if ($SizeGB -le 0) {
    $default = [Math]::Min(32, $maxGB)
    if ($Yes) { $SizeGB = $default } else {
        $answer = Read-Host "Сколько гигабайт отдать SynthOS? ($minGB..$maxGB, Enter — $default)"
        if ($answer -eq '') { $SizeGB = $default } elseif ($answer -match '^\d+$') { $SizeGB = [int]$answer } else { Fail 'нужно число.' }
    }
}
if ($SizeGB -lt $minGB -or $SizeGB -gt $maxGB) { Fail "размер должен быть от $minGB до $maxGB ГБ." }
$bytes = [long]$SizeGB * 1GB

Say ''
Say "Диск:      $($disk.FriendlyName) (диск $($disk.Number))"
Say "Windows:   $($letter): уменьшится на $SizeGB ГБ, ваши файлы не трогаются"
Say "SynthOS:   новый раздел $SizeGB ГБ, загрузчик в разделе EFI Windows (\EFI\SynthOS)"
if ($secure) {
    if ($shim) {
        Say 'Secure Boot включён — выключать не нужно. При первом запуске появится синий экран MokManager:'
        Say '  OK → любая клавиша → Enroll key from disk → (раздел EFI) → SYNTHOS → SYNTHOS.CER → Continue → Yes → Reboot.'
    } else {
        Say 'Secure Boot включён, а в этом архиве нет shim: добавьте SYNTHOS\SYNTHOS.CER в db прошивки или выключите Secure Boot.' -ForegroundColor Yellow
    }
}
if (-not (Ask 'Продолжить?')) { Fail 'отменено.' }

# 4. BitLocker: меню загрузки меняется, поэтому защита приостанавливается на две перезагрузки
try {
    $bl = Get-BitLockerVolume -MountPoint "$($letter):" -ErrorAction Stop
    if ($bl.ProtectionStatus -eq 'On') {
        Step 'BitLocker: защита приостанавливается на 2 перезагрузки'
        Suspend-BitLocker -MountPoint "$($letter):" -RebootCount 2 | Out-Null
    }
} catch { }

# 5. Место для SynthOS
Step "Уменьшение $($letter): на $SizeGB ГБ"
$oldEnd = $cpart.Offset + $cpart.Size
try { Resize-Partition -DriveLetter $letter -Size ($cpart.Size - $bytes) } catch { Fail "Windows не смогла сжать $($letter): $($_.Exception.Message)" }
$cpart = Get-Partition -DriveLetter $letter
$start = $cpart.Offset + $cpart.Size
$start = [long][Math]::Ceiling($start / 1MB) * 1MB
$size = [long][Math]::Floor(($oldEnd - $start) / 1MB) * 1MB
$part = $null
try {
    $part = New-Partition -DiskNumber $disk.Number -Offset $start -Size $size -GptType $SynthType
} catch {
    Resize-Partition -DriveLetter $letter -Size (Get-PartitionSupportedSize -DriveLetter $letter).SizeMax -ErrorAction SilentlyContinue
    Fail "не удалось создать раздел: $($_.Exception.Message)"
}
$lba = [long]($part.Offset / 512)
$sectors = [long]($part.Size / 512)

# 6. Система на новый раздел: загрузочная область, пакет программ, запись установки
Step 'Копирование системы'
$id = New-Object byte[] 8
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
do { $rng.GetBytes($id) } while (($id | Where-Object { $_ -ne 0 }).Count -eq 0)
$record = [byte[]]$sector0.Clone()
[Array]::Copy($id, 0, $record, 420, 8)
$flags = [BitConverter]::ToUInt32($record, 428) -bor 1
[Array]::Copy([BitConverter]::GetBytes([uint32]$flags), 0, $record, 428, 4)
for ($i = 446; $i -lt 510; $i++) { $record[$i] = 0 }
$dev = $null
try {
    $dev = [SynthDisk]::Open($disk.Number)
    Copy-Region $img $dev 0 ($lba * 512) ([long]$BootArea * 512) 'загрузочная область'
    if ($bundle -gt 0) {
        Copy-Region $img $dev ([long]$systemSectors * 512) (($lba + $systemSectors) * 512) ([long]$bundle * 512) 'программы (Chromium)'
    }
    $dev.Position = ($lba + $systemSectors + $bundle) * 512
    $zero = New-Object byte[] (16 * 512)
    $dev.Write($zero, 0, $zero.Length)
    $dev.Position = $lba * 512
    $dev.Write($record, 0, 512)
    $dev.Flush()
} catch {
    if ($dev) { $dev.Dispose() }
    Fail "запись на диск: $($_.Exception.Message)"
}
$dev.Dispose()
$img.Dispose()

# 7. Загрузчик и ядро в раздел EFI Windows
Step 'Загрузчик в раздел EFI'
$esp = $null
foreach ($l in [char[]]'ZYXWVUTSRQPONMLKJIHG') { if (-not (Test-Path "$($l):\")) { $esp = "$($l):"; break } }
if (-not $esp) { Fail 'нет свободной буквы диска для раздела EFI.' }
mountvol $esp /S | Out-Null
if (-not (Test-Path "$esp\EFI")) { Fail 'не удалось открыть раздел EFI (mountvol /S).' }
try {
    $free = (New-Object IO.DriveInfo($esp)).AvailableFreeSpace
    $want = (Get-ChildItem (Join-Path $here 'EFI') -File | Measure-Object -Property Length -Sum).Sum + 1MB
    if ($free -lt $want) { throw "в разделе EFI свободно $([int]($free / 1MB)) МБ, нужно $([int]($want / 1MB)) МБ" }
    New-Item -ItemType Directory -Force -Path "$esp\EFI\SynthOS", "$esp\SYNTHOS" | Out-Null
    if ($shim) {
        foreach ($f in 'shimx64.efi', 'grubx64.efi', 'mmx64.efi') { Copy-Item (Join-Path $here "EFI\$f") "$esp\EFI\SynthOS\$f" -Force }
        $loader = '\EFI\SynthOS\shimx64.efi'
    } else {
        Copy-Item (Join-Path $here 'EFI\grubx64.efi') "$esp\EFI\SynthOS\synthos.efi" -Force
        $loader = '\EFI\SynthOS\synthos.efi'
    }
    foreach ($f in 'KERNEL.BIN', 'SYNTHOS.CER', 'SHIM.TXT') {
        if (Test-Path (Join-Path $here "EFI\$f")) { Copy-Item (Join-Path $here "EFI\$f") "$esp\SYNTHOS\$f" -Force }
    }
    $hex = ($id | ForEach-Object { $_.ToString('x2') }) -join ''
    $cfg = "# SynthOS installation: the loader reads this file`nsystem=$lba`nsectors=$sectors`nid=$hex`nregister=1`ntimeout=3`n"
    [IO.File]::WriteAllText("$esp\SYNTHOS\SYSTEM.CFG", $cfg, (New-Object Text.ASCIIEncoding))
} catch {
    mountvol $esp /D | Out-Null
    Fail "раздел EFI: $($_.Exception.Message)"
}
mountvol $esp /D | Out-Null

# 8. Пункт меню загрузки и запуск SynthOS при следующей перезагрузке
Step 'Меню загрузки'
$out = (& bcdedit.exe /copy '{bootmgr}' /d 'SynthOS') -join ' '
$entry = [regex]::Match($out, '\{[0-9a-fA-F-]{36}\}').Value
if (-not $entry) { Fail "bcdedit: $out" }
& bcdedit.exe /set $entry path $loader | Out-Null
& bcdedit.exe /set '{fwbootmgr}' displayorder $entry /addlast | Out-Null
& bcdedit.exe /set '{fwbootmgr}' bootsequence $entry | Out-Null

Write-Host ''
Write-Host 'Готово. При следующей перезагрузке запустится SynthOS; дальше при каждом включении будет меню' -ForegroundColor Green
Write-Host 'SynthOS / Windows (3 секунды). Удалить SynthOS: «Uninstall SynthOS.cmd».' -ForegroundColor Green
if (-not $NoRestart -and (Ask 'Перезагрузить компьютер сейчас?')) { Restart-Computer -Force }
elseif (-not $Yes) { Read-Host 'Нажмите Enter, чтобы закрыть' | Out-Null }
