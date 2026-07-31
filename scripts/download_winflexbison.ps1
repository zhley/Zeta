$targetDir = "tools/win_flex_bison"
$version = "2.5.25"
$url = "https://github.com/lexxmark/winflexbison/releases/download/v$version/win_flex_bison-$version.zip"
$zipFile = Join-Path $targetDir "winflexbison.zip"

if (-not (Test-Path $targetDir)) { New-Item -ItemType Directory -Path $targetDir | Out-Null }

Write-Host "Downloading WinFlexBison from $url ..."
Invoke-WebRequest -Uri $url -OutFile $zipFile

Write-Host "Extracting to $targetDir ..."
Expand-Archive -Path $zipFile -DestinationPath $targetDir -Force

Remove-Item $zipFile
Write-Host "Done. win_flex.exe and win_bison.exe are in $targetDir."
