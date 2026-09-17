# stm32/dap.ps1 —— Stage 4a 的主機端：對 dap_uart 送 CMSIS-DAP 命令
#
# 為什麼不是 Python：這台機器沒有 pyserial，而裝套件只為了跑一支測試腳本不划算。
# System.IO.Ports.SerialPort 本來就在 .NET 裡，serial.ps1 已經用同一套。
#
# 用法：
#   .\dap.ps1                 對 COM7 跑一整輪自我測試
#   .\dap.ps1 -Port COM7
#
# ⚠ 這支檔案要存成 UTF-8 **with BOM**（理由見 README「存檔編碼」）。
#
# 框架格式（跟 dap_uart/main.c 對齊）：
#   主機 -> MCU:  0xA5 len payload xor
#   MCU  -> 主機:  0x5A len payload xor

param(
    [string]$Port = 'COM7',
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'one'
$sp.ReadTimeout = 2000
$sp.DtrEnable = $true

try { $sp.Open() } catch {
    Write-Host "開 $Port 失敗：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host "常見原因：serial.ps1 還開著同一個埠。" -ForegroundColor Yellow
    exit 1
}

# 開埠後把殘留的位元組清掉。上一支韌體可能還有沒讀完的文字在緩衝區裡，
# 那些會被當成框架垃圾 —— 這是二進位通道特有的坑。
Start-Sleep -Milliseconds 200
$sp.DiscardInBuffer()

function Send-Dap {
    param([byte[]]$Payload)

    $n = $Payload.Length
    $frame = New-Object byte[] ($n + 3)
    $frame[0] = 0xA5
    $frame[1] = [byte]$n
    [Array]::Copy($Payload, 0, $frame, 2, $n)
    $x = [byte]$n
    foreach ($b in $Payload) { $x = $x -bxor $b }
    $frame[$n + 2] = $x
    $sp.Write($frame, 0, $frame.Length)

    # 等框頭。收到別的位元組就丟掉繼續找 —— 不試圖修復，只求能重新同步。
    for ($i = 0; $i -lt 64; $i++) {
        if ($sp.ReadByte() -eq 0x5A) { break }
        if ($i -eq 63) { throw "等不到框頭 0x5A" }
    }
    $len = $sp.ReadByte()
    if ($len -lt 1 -or $len -gt 64) { throw "框架長度不合理：$len" }

    $resp = New-Object byte[] $len
    $x = [byte]$len
    for ($i = 0; $i -lt $len; $i++) {
        $resp[$i] = [byte]$sp.ReadByte()
        $x = $x -bxor $resp[$i]
    }
    $chk = [byte]$sp.ReadByte()
    if ($chk -ne $x) { throw "校驗不符（收到 $chk，算出 $x）—— 框架跑掉了" }
    return $resp
}

function Hex32([uint32]$v) { '0x{0:X8}' -f $v }

# ── PowerShell 5.1 的 32-bit 常值坑，一次收斂在這裡 ──
#
# 大於 0x7FFFFFFF 的十六進位常值會被當成 Int32 溢位成負數：
#   0xE000ED00 -> -536810240、0xA0000000 -> -1610612736
# 直接 [uint32] 轉型會拋「數值對 UInt32 而言太大或太小」。
#
# 與其在每個常值後面撒 L 後綴（漏一個就再炸一次，2026-09-11 連炸兩次），
# **改成在函式邊界統一收斂**：參數一律收 [long]，進來先 -band 0xFFFFFFFFL。
# 負數在 64 位元下是 0xFFFFFFFFE000ED00，遮掉高位就變回正確的 0xE000ED00。
function U32 {
    param([long]$v)
    return [uint32]($v -band 0xFFFFFFFFL)
}

function Get-DapString {
    param([byte]$Id)
    $r = Send-Dap @(0x00, $Id)
    $n = $r[1]
    if ($n -le 1) { return '' }
    return [System.Text.Encoding]::ASCII.GetString($r, 2, $n - 1)
}

# ── DAP_Transfer 的 request byte ──
# bit0 APnDP、bit1 RnW、bit2-3 是暫存器位址 A[3:2]（已經在正確的位置上）
$DP_RD_IDCODE = 0x02; $DP_WR_ABORT = 0x00
$DP_RD_CTRL   = 0x06; $DP_WR_CTRL  = 0x04
$DP_WR_SELECT = 0x08; $DP_RD_RDBUF = 0x0E
$AP_RD_CSW    = 0x03; $AP_WR_CSW   = 0x01
$AP_WR_TAR    = 0x05; $AP_RD_DRW   = 0x0F

function Xfer-Read {
    param([byte]$Req)
    $r = Send-Dap @(0x05, 0x00, 0x01, $Req)
    if ($r[1] -ne 1 -or $r[2] -ne 1) {
        throw ("transfer 失敗：做了 {0} 筆，回應 0x{1:X2}" -f $r[1], $r[2])
    }
    return [BitConverter]::ToUInt32($r, 3)
}

function Xfer-Write {
    param([byte]$Req, [long]$Val)
    $d = [BitConverter]::GetBytes((U32 $Val))
    $r = Send-Dap @(0x05, 0x00, 0x01, $Req, $d[0], $d[1], $d[2], $d[3])
    if ($r[1] -ne 1 -or $r[2] -ne 1) {
        throw ("transfer 失敗：做了 {0} 筆，回應 0x{1:X2}" -f $r[1], $r[2])
    }
}

# AP 的讀取是 posted 的：讀 AP 拿到的是「上一次」的結果，真正的值要再讀一次
# DP 的 RDBUFF 才拿得到。swd.h 的 swd__ap_rd 就是這樣做的，主機端也必須照做。
function Ap-Read {
    param([long]$Addr)
    Xfer-Write $AP_WR_TAR $Addr
    [void](Xfer-Read $AP_RD_DRW)
    return Xfer-Read $DP_RD_RDBUF
}

function Check {
    param([string]$Name, [long]$Got, [long]$Want)
    $Got = U32 $Got
    $Want = U32 $Want
    if ($Got -eq $Want) {
        Write-Host ("  {0,-22} {1}  ✓" -f $Name, (Hex32 $Got)) -ForegroundColor Green
    } else {
        Write-Host ("  {0,-22} {1}  ✗ 期待 {2}" -f $Name, (Hex32 $Got), (Hex32 $Want)) -ForegroundColor Red
    }
}

try {
    Write-Host ""
    Write-Host "── DAP_Info ─────────────────────────────────────────" -ForegroundColor Cyan
    Write-Host ("  廠商        {0}" -f (Get-DapString 0x01))
    Write-Host ("  產品        {0}" -f (Get-DapString 0x02))
    Write-Host ("  韌體版本    {0}" -f (Get-DapString 0x04))

    $caps = Send-Dap @(0x00, 0xF0)
    Write-Host ("  capabilities 0x{0:X2}  （bit0=SWD bit1=JTAG）" -f $caps[2])

    $psz = Send-Dap @(0x00, 0xFF)
    Write-Host ("  封包大小    {0} byte" -f ([BitConverter]::ToUInt16($psz, 2)))

    Write-Host ""
    Write-Host "── 連線 ─────────────────────────────────────────────" -ForegroundColor Cyan
    $r = Send-Dap @(0x02, 0x01)             # DAP_Connect，port=SWD
    if ($r[1] -ne 1) { throw "DAP_Connect 沒有選到 SWD" }
    Write-Host "  DAP_Connect  -> SWD"

    # SWJ_Clock：1 000 000 = 0x000F4240，小端序送。韌體會回 OK 但實際上調不了速度
    # （SWD_DLY() 是寫死的），理由見 dap.h 那段註解。
    [void](Send-Dap @(0x11, 0x40, 0x42, 0x0F, 0x00))
    [void](Send-Dap @(0x04, 0x00, 0x64, 0x00, 0x00, 0x00))  # TransferConfigure retry=100

    # line reset + JTAG-to-SWD。**這一段是主機下令做的**，不是韌體自己做 ——
    # 真正的 CMSIS-DAP probe 就是這樣：協定順序由主機掌握。
    [void](Send-Dap @(0x12, 56, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF))
    [void](Send-Dap @(0x12, 16, 0x9E, 0xE7))
    [void](Send-Dap @(0x12, 56, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF))
    [void](Send-Dap @(0x12, 8, 0x00))
    Write-Host "  SWJ_Sequence -> line reset + JTAG-to-SWD"

    Write-Host ""
    Write-Host "── 讀 target ────────────────────────────────────────" -ForegroundColor Cyan
    Check 'DPIDR'  (Xfer-Read $DP_RD_IDCODE) 0x2BA01477

    Xfer-Write $DP_WR_ABORT  0x1E            # 清 sticky error
    Xfer-Write $DP_WR_SELECT 0x00            # AP0、bank0
    Xfer-Write $DP_WR_CTRL   0x50000000      # CSYS/CDBG 上電

    # ⚠ PowerShell 5.1：**超過 0x7FFFFFFF 的十六進位常值會被當成 Int32 溢位成負數**。
    # 0xA0000000 在這裡等於 -1610612736，拿去跟 -band 算出來的正數比永遠不相等
    # （2026-09-11 踩過：CTRL/STAT 明明是 0xF0000040、兩個 ACK 都亮了，卻判成失敗）。
    #
    # 而且 **[uint32]0xA0000000 不能用來修** —— 轉型發生在常值已經是負 Int32 之後，
    # 會直接拋「Value was either too large or too small for a UInt32」。
    # 正解是加 L 後綴，讓它一開始就是 Int64。
    $PWRUP_ACK = 0xA0000000L               # CSYSPWRUPACK(31) + CDBGPWRUPACK(29)

    $ctrl = [uint32]0
    for ($i = 0; $i -lt 50; $i++) {
        $ctrl = Xfer-Read $DP_RD_CTRL
        if (($ctrl -band $PWRUP_ACK) -eq $PWRUP_ACK) { break }
        Start-Sleep -Milliseconds 10
    }
    if (($ctrl -band $PWRUP_ACK) -ne $PWRUP_ACK) {
        throw ("電源握手失敗，CTRL/STAT = {0}" -f (Hex32 $ctrl))
    }
    Write-Host ("  CTRL/STAT              {0}  ✓ 已上電" -f (Hex32 $ctrl)) -ForegroundColor Green

    # MEM-AP 的存取大小設成 word。
    # ⚠ 讀 AP 拿到的是「上一次」的結果，真正的值在 RDBUFF —— 所以 $csw 必須取
    # 第二次讀的結果。第一次的回傳值是過期的，拿它去 read-modify-write 會把
    # CSW 寫成別的東西。
    [void](Xfer-Read $AP_RD_CSW)
    $csw = Xfer-Read $DP_RD_RDBUF
    Xfer-Write $AP_WR_CSW (($csw -band (-bnot 7)) -bor 2)

    Check 'CPUID'          (Ap-Read 0xE000ED00) 0x410FC241
    Check 'DBGMCU_IDCODE'  (Ap-Read 0xE0042000) 0x10076415
    Check 'SP[0]'          (Ap-Read 0x08000000) 0x20018000

    Write-Host ""
    Write-Host "  全部通過 —— DAP 指令層在 UART 上已經可用。" -ForegroundColor Green
    Write-Host "  下一步是 Stage 4b：把同一個 dap_process() 接到 USB HID 端點。"
    Write-Host ""
}
catch {
    Write-Host ""
    Write-Host "✗ $($_.Exception.Message)" -ForegroundColor Red
    Write-Host ""
}
finally {
    $sp.Close()
}
