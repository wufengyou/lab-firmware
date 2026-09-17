# stm32/pyocd.ps1 —— pyOCD 的包裝，給 Stage 4b 之後用
#
# 為什麼要這一支：
#   1. pyocd.exe 不在 PATH 上，它藏在 VS Code 的 CMSIS Debugger 擴充套件裡，
#      完整路徑長到每次打都會出錯（貼進終端機會被換行拆斷）。
#   2. 目標型號固定是 stm32l475xg —— L475/L476/L486 是同一顆晶片
#      （dev_id=0x415，見 README 的裝置 ID 表），pyOCD 沒有 l476 的內建支援，
#      但 flash 控制器與記憶體佈局完全相同，用 l475xg 是對的。
#   3. 燒錄一律 --erase sector，**絕不 chip** —— chip erase 會清掉 target 上
#      原廠 demo 的一切。這個預設值寫死在這裡，不開放從參數改。
#
# 用法：
#   .\pyocd.ps1 -List                                列出 probe
#   .\pyocd.ps1 -Read 0x080FF800                     讀一個 32-bit
#   .\pyocd.ps1 -Flash build\pyocd_sig.bin -At 0x080FF800    燒一個 raw binary
#   .\pyocd.ps1 -Raw list --targets                  其餘參數直接丟給 pyocd
#
# ⚠ -Flash 會不可逆改變 target 的 flash。動手前先確認 build\l476_factory_backup.bin
#   這份 1MB 全備份還在（那是唯一的還原路徑）。

param(
    [switch]$List,
    [string]$Read,
    [string]$Flash,
    [string]$At,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Raw
)

$ext = Get-ChildItem "$env:USERPROFILE\.vscode\extensions\arm.vscode-cmsis-debugger-*\tools\pyocd\pyocd.exe" -ErrorAction SilentlyContinue
if (-not $ext) {
    Write-Host "找不到 pyocd.exe —— 它跟著 VS Code 的 CMSIS Debugger 擴充套件走，" -ForegroundColor Red
    Write-Host "擴充套件被移除或改版路徑就會失效。" -ForegroundColor Red
    exit 1
}
$pyocd = $ext[0].FullName

# attach：不要在連上時就把 target 停住。讀東西不需要停它，停了反而改變現場。
$attach = @('-t', 'stm32l475xg', '-O', 'connect_mode=attach')

if ($List)  { & $pyocd list; exit $LASTEXITCODE }

if ($Read)  { & $pyocd cmd @attach -c "read32 $Read"; exit $LASTEXITCODE }

if ($Flash) {
    if (-not $At) { Write-Host "要給 -At <位址>，例如 -At 0x080FF800" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path $Flash)) { Write-Host "找不到檔案：$Flash" -ForegroundColor Red; exit 1 }

    Write-Host "燒錄 $Flash -> $At（sector erase，只抹這一頁）" -ForegroundColor Yellow
    & $pyocd flash -t stm32l475xg --erase sector --format bin --base-address $At $Flash
    exit $LASTEXITCODE
}

if ($Raw)   { & $pyocd @Raw; exit $LASTEXITCODE }

Write-Host "用法見檔案開頭的註解。常用：-List / -Read <addr> / -Flash <file> -At <addr>"
