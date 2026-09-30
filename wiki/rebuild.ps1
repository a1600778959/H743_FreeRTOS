# 本地一键重建 wiki：构建 → 预渲染 Mermaid → 刷新桌面离线包
# 用法：powershell -File rebuild.ps1   （或双击 rebuild.cmd）
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

Write-Host '[1/4] npm run build' -ForegroundColor Cyan
npm run build
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host '[2/4] ensure local server :8931' -ForegroundColor Cyan
$conn = Get-NetTCPConnection -LocalPort 8931 -State Listen -ErrorAction SilentlyContinue
if (-not $conn) {
  Start-Process -FilePath "$env:LOCALAPPDATA\Microsoft\WindowsApps\python.exe" `
    -ArgumentList '-m', 'http.server', '8931', '--bind', '127.0.0.1', `
      '--directory', "$PSScriptRoot\.vitepress\dist" -WindowStyle Hidden
  Start-Sleep -Seconds 2
}

Write-Host '[3/4] pre-render mermaid' -ForegroundColor Cyan
$env:WIKI_ORIGIN = 'http://127.0.0.1:8931/'
node render-static.mjs

Write-Host '[4/4] refresh desktop zip' -ForegroundColor Cyan
$stage = Join-Path $PSScriptRoot '.vitepress\package-staging\Dima-Wiki'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -Recurse -Force (Join-Path $PSScriptRoot '.vitepress\dist\*') $stage
Set-Content -Path (Join-Path $stage 'README-打开说明.txt') `
  -Value "Dima Wiki 离线包 $(Get-Date -Format yyyy-MM-dd)`r`n解压后双击 index.html 离线浏览。"
Compress-Archive -Path $stage `
  -DestinationPath "$([Environment]::GetFolderPath('Desktop'))\Dima-Wiki-$(Get-Date -Format yyyyMMdd).zip" -Force
Write-Host 'package refreshed on Desktop' -ForegroundColor Green
