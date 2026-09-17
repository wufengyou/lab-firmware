<#
  nano130/isp_send.ps1 —— N2 的 host 端工具：把 .bin 經 UART0 送進 master(flash_isp)

  用法：
    .\isp_send.ps1 -Port COM13 -Ping
    .\isp_send.ps1 -Port COM13 -File build\blink\blink.bin            # 送到 0x0
    .\isp_send.ps1 -Port COM13 -File x.bin -Addr 0x1EA00              # 送到指定位址
    .\isp_send.ps1 -Port COM13 -Run                                   # 放 target 跑

  master 端沒解保險（keypad 按 0 切換）時，每塊只會回 'D'(dry run)，不碰 target。
  **這正是 Stage b 的跑法**：先用沒解保險的狀態把協定、CRC、位址檢查跑通。

  ⚠ 這支腳本不是安全邊界。位址範圍檢查在韌體那一端（flash_isp/main.c），
    因為腳本會被改、會被打錯；韌體不會。這裡的檢查只是為了早點給出好訊息。
#>
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$File,
    [long]$Addr = 0,
    [int]$Baud = 115200,
    [switch]$Ping,
    [switch]$Run,
    [switch]$Reset,
    [string]$Dump,                      # 讀回 target 的記憶體存成檔案
    [switch]$EraseAll,                  # 燒新韌體前把整個 APROM 抹乾淨
    [long]$Length = 0x1EC00,            # 預設整個 APROM
    [switch]$Halt
)

$ErrorActionPreference = 'Stop'

# ⚠ PowerShell 5.1：大於 0x7FFFFFFF 的十六進位常值會溢位成負數（PowerShell 5.1 的已知行為）。
#   這裡的位址都在 APROM 範圍內（< 0x20000）所以碰不到，但參數刻意收 [long]
#   而不是 [int]，照那份備忘的建議「在函式邊界收斂」。
$CHUNK = 512

function New-Crc16([byte[]]$Bytes) {
    # CRC16-CCITT：poly 0x1021、init 0xFFFF。要跟 flash_isp/main.c 的 crc16() 一致。
    [uint16]$crc = 0xFFFF
    foreach ($b in $Bytes) {
        $crc = $crc -bxor ([uint16]$b -shl 8)
        for ($i = 0; $i -lt 8; $i++) {
            if ($crc -band 0x8000) { $crc = (($crc -shl 1) -bxor 0x1021) -band 0xFFFF }
            else                   { $crc = ($crc -shl 1) -band 0xFFFF }
        }
    }
    return [uint16]$crc
}

function Wait-Ready($sp) {
    # 動作之前先 ping 到回應為止。
    # master 剛 reset 時要跑 swd_connect() 加開機畫面，那段期間灌資料進去
    # 只會塞爆 UART FIFO，然後第一塊就 ERR_TIMEOUT —— 2026-09-15 實際踩過。
    # 「先握手再動作」跟韌體那邊「ACK 最後才送」是同一條規則的兩面。
    for ($try = 1; $try -le 20; $try++) {
        $sp.DiscardInBuffer()
        $sp.Write([byte[]](0x50), 0, 1)     # 'P'
        Start-Sleep -Milliseconds 250
        if ($sp.BytesToRead -gt 0) {
            if ($sp.ReadExisting().Contains('P')) { $sp.DiscardInBuffer(); return }
        }
    }
    throw 'master 沒有回應 ping —— 檢查 COM 埠、鮑率、接線，或按 SW1 重開'
}

function Read-Reply($sp, [string]$what) {
    try { $r = $sp.ReadByte() } catch { throw "$what : 逾時，master 沒回應" }
    $c = [char]$r
    if ($c -eq 'E') {
        $code = $sp.ReadByte()
        $names = @{
            1 = 'CRC 不符'; 2 = '位址被韌體拒收'; 3 = '長度不合法'
            4 = 'ISP 前置失敗（halt/解鎖/ISPEN）'; 5 = '抹除失敗'; 6 = '寫入失敗'
            7 = '回讀驗證不符'; 8 = '收封包逾時'; 9 = 'SWD 未連線'
            10 = '沒解保險（master 的 keypad 按 0 切到 ARMED）'
        }
        $msg = if ($names.ContainsKey([int]$code)) { $names[[int]$code] } else { '未知' }
        throw "$what : master 回報錯誤 $code（$msg）"
    }
    return $c
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 5000
$sp.WriteTimeout = 5000
$sp.Open()
try {
    Start-Sleep -Milliseconds 200
    $sp.DiscardInBuffer()

    if ($Ping) {
        $sp.Write([byte[]](0x50), 0, 1)          # 'P'
        $c = Read-Reply $sp 'ping'
        Write-Output "ping -> $c"
        return
    }
    # 除了 -Ping 本身，任何動作之前都先握手。
    # 原本只有送檔案那條路徑有，結果 -Reset 撞上 master 開機就逾時 —— 同一個坑
    # 在同一支腳本裡踩第二次，因為修的是「那一條路徑」而不是「所有路徑」。
    Wait-Ready $sp

    if ($Halt) { $sp.Write([byte[]](0x48), 0, 1); Write-Output ("halt -> " + (Read-Reply $sp 'halt')); return }
    if ($Run)  { $sp.Write([byte[]](0x52), 0, 1); Write-Output ("run  -> " + (Read-Reply $sp 'run'));  return }
    # 燒完要用 -Reset，不是 -Run。'R' 只是「從停住的地方繼續」，而那個位址屬於
    # 已經被換掉的舊程式；要讓新韌體從向量表跑起來得重置。
    # 見 flash_isp/main.c 的 target_reset()（走 AIRCR SYSRESETREQ，不需要 nRESET 腳）。
    if ($Reset) { $sp.Write([byte[]](0x53), 0, 1); Write-Output ("reset -> " + (Read-Reply $sp 'reset')); return }

    if ($EraseAll) {
        # 用小韌體覆蓋大韌體時，write 只抹它自己要寫的那幾頁，後面會留著上一手的
        # 程式碼 —— 2026-09-15 dump 抓到的。殘骸可能含上一手的資料（這個 repo
        # 就有過明文 WiFi 帳密的韌體），所以換韌體時該先抹乾淨。
        $pages = 0x1EC00 / 512          # 整個 APROM = 246 頁
        $head = New-Object byte[] 6     # addr = 0
        $head[4] = $pages -band 0xFF; $head[5] = ($pages -shr 8) -band 0xFF
        $sp.Write([byte[]](0x45), 0, 1) # 'E'
        $sp.Write($head, 0, 6)
        $sp.ReadTimeout = 60000         # 246 頁要抹一陣子
        # -f 的優先權低於 +，所以這裡一定要先把兩個值都算好再格式化，
        # 不然 {0} 會原樣印出來（第一版就是這樣）。
        $r = Read-Reply $sp '抹除'
        Write-Output ("抹除整個 APROM（{0} 頁）-> {1}" -f $pages, $r)
        $sp.ReadTimeout = 5000
        if (-not $File) { return }
    }

    if ($Dump) {
        # 讀回來存檔。**燒之前先做這件事** —— 這條保險在 N2 一開始是缺的，
        # 於是「原始碼在 repo 裡」變成了唯一的退路，而那是運氣不是設計。
        $out = New-Object System.Collections.Generic.List[byte]
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $off = 0
        while ($off -lt $Length) {
            $len = [Math]::Min($CHUNK, $Length - $off)
            $a = $Addr + $off

            $head = New-Object byte[] 6
            $head[0] = $a -band 0xFF; $head[1] = ($a -shr 8) -band 0xFF
            $head[2] = ($a -shr 16) -band 0xFF; $head[3] = ($a -shr 24) -band 0xFF
            $head[4] = $len -band 0xFF; $head[5] = ($len -shr 8) -band 0xFF

            $sp.Write([byte[]](0x44), 0, 1)      # 'D'
            $sp.Write($head, 0, 6)

            $c = Read-Reply $sp ("讀 0x{0:X}" -f $a)
            if ($c -ne 'K') { throw ("非預期回應 '{0}'" -f $c) }

            $data = New-Object byte[] $len
            $got = 0
            while ($got -lt $len) { $got += $sp.Read($data, $got, $len - $got) }
            $cr = New-Object byte[] 2
            $got = 0
            while ($got -lt 2) { $got += $sp.Read($cr, $got, 2 - $got) }

            $want = [uint16]$cr[0] -bor ([uint16]$cr[1] -shl 8)
            $have = New-Crc16 $data
            if ($want -ne $have) {
                throw ("讀 0x{0:X}：CRC 不符（master {1:X4} / 本地 {2:X4}）" -f $a, $want, $have)
            }

            $out.AddRange($data)
            $off += $len
            Write-Progress -Activity 'dump' -Status ("0x{0:X} / {1} byte" -f $a, $Length) `
                           -PercentComplete ([int](100 * $off / $Length))
        }
        $sw.Stop()
        [System.IO.File]::WriteAllBytes((Join-Path (Get-Location) $Dump), $out.ToArray())
        Write-Output ("讀回 {0} byte -> {1}，耗時 {2:N1} 秒" -f $out.Count, $Dump, $sw.Elapsed.TotalSeconds)
        return
    }

    if (-not $File) { throw '要 -File / -Dump，或用 -Ping / -Run / -Reset / -Halt' }

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $File))

    # 補齊到 4 的倍數：flash 是以 word 為單位寫的，尾巴補 0xFF（= 空白 flash 的值，
    # 寫 0xFF 等於不改變那個位元組，是最無害的填充）。
    while ($bytes.Length % 4 -ne 0) { $bytes += [byte]0xFF }

    Write-Output ("送 {0}：{1} byte -> 0x{2:X}" -f $File, $bytes.Length, $Addr)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $off = 0
    $nk = 0; $nd = 0
    while ($off -lt $bytes.Length) {
        $len = [Math]::Min($CHUNK, $bytes.Length - $off)
        $a = $Addr + $off

        $head = New-Object byte[] 6
        $head[0] = $a -band 0xFF; $head[1] = ($a -shr 8) -band 0xFF
        $head[2] = ($a -shr 16) -band 0xFF; $head[3] = ($a -shr 24) -band 0xFF
        $head[4] = $len -band 0xFF; $head[5] = ($len -shr 8) -band 0xFF

        $data = New-Object byte[] $len
        [Array]::Copy($bytes, $off, $data, 0, $len)

        $crc = New-Crc16 ($head + $data)

        $sp.Write([byte[]](0x57), 0, 1)          # 'W'
        $sp.Write($head, 0, 6)
        $sp.Write($data, 0, $len)
        $sp.Write([byte[]](($crc -band 0xFF), (($crc -shr 8) -band 0xFF)), 0, 2)

        $c = Read-Reply $sp ("寫 0x{0:X}" -f $a)
        if ($c -eq 'K') { $nk++ } elseif ($c -eq 'D') { $nd++ } else { throw ("非預期回應 '{0}'" -f $c) }

        $off += $len
        Write-Progress -Activity 'flash_isp' -Status ("0x{0:X} / {1} byte" -f $a, $bytes.Length) `
                       -PercentComplete ([int](100 * $off / $bytes.Length))
    }
    $sw.Stop()
    Write-Output ("完成：寫入 {0} 塊、dry-run {1} 塊，耗時 {2:N1} 秒" -f $nk, $nd, $sw.Elapsed.TotalSeconds)
    if ($nd -gt 0) { Write-Output '※ 全部是 dry run —— master 端沒解保險（keypad 按 0 切換到 ARMED）' }
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
}
