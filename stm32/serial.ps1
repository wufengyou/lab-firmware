# stm32/serial.ps1 —— 給 swd_probe / cpu_ctrl 用的互動式序列終端機
#
# 為什麼不用 arduino-cli monitor / PuTTY：
#   前者在這個 repo 裡被歸類為互動式不能用；後者要另外裝。而 cpu_ctrl 需要
#   「一邊看輸出、一邊按單鍵下指令」，唯讀的那段 PowerShell（先前
#   Mega 用的）不夠用。所以這裡自己寫一支。
#
# 用法（PowerShell）：
#   .\serial.ps1                 預設 COM7 = NUCLEO-F446RE（master）
#   .\serial.ps1 -Port COM6      改用別的埠
#   .\serial.ps1 -List           只列出目前接著哪些板子，不開埠
#
# 離開：Ctrl+] 。（不是 Ctrl+C —— Ctrl+C 會讓埠來不及關，下次開會說「存取被拒」）
#
# 打進去的字元會直接送給 master，這就是 cpu_ctrl 的 h/r/s/d/m/?/c。

param(
    [string]$Port = 'COM7',
    [int]$Baud = 115200,
    [switch]$List
)

# ── 列出接著的 ST-LINK：把 VCP 和燒錄磁碟用 USB 序號對起來 ──
# 兩片板子都是 ST-LINK/V2-1（VID_0483&PID_374B），光看 COM 編號分不出誰是誰，
# 但磁碟標籤（NODE_F446RE / DIS_L476VG）認得出來，而兩者共用同一個 USB 序號。
function Show-Boards {
    $disks = @{}
    Get-CimInstance Win32_DiskDrive | Where-Object { $_.InterfaceType -eq 'USB' } | ForEach-Object {
        $dd = $_
        # PNPDeviceID 長這樣：USBSTOR\DISK&VEN_MBED&...\7&DDA8D8C&0&0670FF5752...&0
        # 序號前面那個 "&0" 的 0 是分隔用的，不是序號的一部分 —— 但序號本身也以 0 開頭，
        # 寫成 '&0(...)' 會把序號的第一個字元吃掉，跟 VCP 那邊對不起來（踩過）。
        if ($dd.PNPDeviceID -match '&([0-9A-F]{20,})&') { $serial = $Matches[1] } else { return }
        $part = Get-CimInstance -Query "ASSOCIATORS OF {Win32_DiskDrive.DeviceID='$($dd.DeviceID)'} WHERE AssocClass=Win32_DiskDriveToDiskPartition"
        foreach ($p in $part) {
            $ld = Get-CimInstance -Query "ASSOCIATORS OF {Win32_DiskPartition.DeviceID='$($p.DeviceID)'} WHERE AssocClass=Win32_LogicalDiskToPartition"
            foreach ($l in $ld) {
                $vol = (Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($l.DeviceID)'").VolumeName
                $disks[$serial] = "$($l.DeviceID) $vol"
            }
        }
    }

    Write-Host ""
    Write-Host "  COM   燒錄磁碟              板子"
    Write-Host "  ----  --------------------  --------------------------------"
    Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' -and $_.DeviceID -match 'VID_0483' } | ForEach-Object {
        if ($_.Name -match '\((COM\d+)\)') { $com = $Matches[1] } else { return }
        $parent = (Get-PnpDeviceProperty -InstanceId $_.DeviceID -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
        $serial = if ($parent -match '\\([0-9A-F]{20,})$') { $Matches[1] } else { '' }
        $disk = if ($serial -and $disks.ContainsKey($serial)) { $disks[$serial] } else { '(找不到磁碟)' }
        $who = switch -Wildcard ($disk) {
            '*NODE_F446RE*' { 'NUCLEO-F446RE  <- master，燒這片' }
            '*DIS_L476VG*'  { 'STM32L476G-DISCO  <- target，不燒' }
            default         { '?' }
        }
        "  {0,-4}  {1,-20}  {2}" -f $com, $disk, $who | Write-Host
    }
    Write-Host ""
}

if ($List) { Show-Boards; exit 0 }

Show-Boards

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'one'
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.ReadTimeout = 50

# NUCLEO 的 USART2 是普通 UART，DTR 不會重置 F446（跟 Mega 不一樣，
# 那邊開埠就會讓車自己跑）。代價是：開埠時開機那行標題早就印完了，
# 所以下面會提示按 RESET 鈕重跑一次。
$sp.DtrEnable = $true

try {
    $sp.Open()
} catch {
    Write-Host "開 $Port 失敗：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host "常見原因：另一個終端機還開著同一個埠，或上次是用 Ctrl+C 離開的。" -ForegroundColor Yellow
    exit 1
}

Write-Host "已連上 $Port @ $Baud  ——  按板子上的黑色 RESET 鈕看開機訊息；按 Esc 離開" -ForegroundColor Green
Write-Host ("-" * 70)

try {
    while ($true) {
        if ($sp.BytesToRead -gt 0) {
            Write-Host -NoNewline $sp.ReadExisting()
        }

        if ([Console]::KeyAvailable) {
            $k = [Console]::ReadKey($true)
            # Esc 或 Ctrl+] 都可以離開。用這兩個而不是 Ctrl+C，是為了讓 finally
            # 有機會關埠 —— Ctrl+] 在某些鍵盤/輸入法切換狀態下打不出來（2026-09-14
            # 遇到），所以加 Esc 當更好按的替代。
            if ($k.Key -eq 'Escape') { break }
            if ($k.Modifiers -band [ConsoleModifiers]::Control -and $k.Key -eq 'Oem6') { break }
            if ($k.KeyChar) { $sp.Write([string]$k.KeyChar) }
        }

        Start-Sleep -Milliseconds 10
    }
} finally {
    $sp.Close()
    Write-Host ""
    Write-Host ("-" * 70)
    Write-Host "$Port 已關閉。"
}
