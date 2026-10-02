# 检查发布目录里所有 exe/dll 的导入表，是否直接引用了 Win7 上不存在的 API
# 返回 0 = 可安全分发到 Win7(64位), 1 = 存在不兼容 API
# 用法: powershell -ExecutionPolicy Bypass -File check_win7_compat.ps1 [产物目录]

param(
    [string]$Dir = "D:\software\Qt\Qt5.14.2\HelloVscode\build\win7\release",
    [string]$ObjDump = "D:\software\mingw730_clean\Tools\mingw730_64\bin\objdump.exe"
)

# Win8+ 才有的 Win32 API（静态导入它们 = Win7 上启动即报“找不到入口点”）
# 注: GetFileInformationByHandleEx / SetFileInformationByHandle / InitializeCriticalSectionEx 是 Vista 起就有的，不在此列
$win8Apis = @(
    "GetSystemTimePreciseAsFileTime", "SetThreadDescription", "GetThreadDescription",
    "SetDefaultDllDirectories", "AddDllDirectory", "RemoveDllDirectory",
    "WaitOnAddress", "WakeByAddressSingle", "WakeByAddressAll",
    "GetTempPath2W", "CreateFile2", "GetCurrentPackageFullName", "ProcessPrng",
    "GetOverlappedResultEx", "MapViewOfFileFromApp", "GetSystemCpuSetInformation"
)
# Win7 上不存在的系统 DLL（api-ms-win-core-* 等 API Set 在 Win7 上缺失）
$badDllPattern = '^(api-ms-win-|ext-ms-win-|ucrtbase\.dll|vcruntime14|msvcp14)'

if (!(Test-Path $ObjDump)) { Write-Host "找不到 objdump: $ObjDump" -ForegroundColor Red; exit 2 }

Write-Host "=== Win7 兼容性导入表检查: $Dir ===" -ForegroundColor Cyan
$failed = @()
$files = Get-ChildItem -Path $Dir -Recurse -Include *.exe, *.dll
foreach ($f in $files) {
    $rel = $f.FullName.Substring($Dir.Length).TrimStart('\')
    $dump = & $ObjDump -p $f.FullName 2>$null
    $dlls = $dump | Select-String 'DLL Name:\s+(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    $funcs = $dump | Select-String '^\s+[0-9a-f]+\s+[0-9]+\s+(\S+)\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    $hits = @($funcs | Where-Object { $win8Apis -contains $_ } | Sort-Object -Unique)
    $hits += @($dlls | Where-Object { $_ -match $badDllPattern } | Sort-Object -Unique)
    if ($hits.Count -gt 0) {
        Write-Host "  [FAIL] $rel => $($hits -join ', ')" -ForegroundColor Red
        $failed += $rel
    } else {
        Write-Host "  [OK]   $rel" -ForegroundColor Green
    }
}

Write-Host ""
if ($failed.Count -eq 0) {
    Write-Host "结论: 可分发到 Windows 7 (64位)。把整个目录(含 platforms/ sqldrivers/ tiles/)拷到 Win7 机器双击运行。" -ForegroundColor Green
    exit 0
} else {
    Write-Host "结论: 以下文件在 Win7 上会因找不到入口点/DLL 而无法启动: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
