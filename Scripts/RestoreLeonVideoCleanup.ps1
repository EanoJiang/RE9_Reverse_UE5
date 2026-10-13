# Restore excluded files without overwriting current work. Run with Unreal closed.
param([string]$Archive)
$ErrorActionPreference = 'Stop'
$leonProjectRoot = (Resolve-Path -LiteralPath 'E:\RE9_Leon_UE5').Path.TrimEnd('\')
if (-not $Archive) {
    $Archive = (Get-Content -LiteralPath (Join-Path $leonProjectRoot 'Scripts\leon_video_cleanup_plan.json') -Raw | ConvertFrom-Json).archive
}
$leonArchiveRoot = (Resolve-Path -LiteralPath $Archive).Path.TrimEnd('\')
$leonPlan = Get-Content -LiteralPath (Join-Path $leonArchiveRoot 'cleanup_plan.json') -Raw | ConvertFrom-Json
if ($leonPlan.project -ne $leonProjectRoot -or $leonPlan.archive -ne $leonArchiveRoot) { throw 'Manifest path mismatch.' }
if (Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) { throw 'Close Unreal before restoring.' }
foreach ($file in Get-ChildItem -LiteralPath $leonArchiveRoot -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath($leonArchiveRoot, $file.FullName)
    if ($relative -in @('cleanup_plan.json', 'cleanup_summary.json') -or $relative.StartsWith('Config\')) { continue }
    $target = [IO.Path]::GetFullPath((Join-Path $leonProjectRoot $relative))
    if (-not $target.StartsWith($leonProjectRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Restore path escapes project.' }
    if (-not (Test-Path -LiteralPath $target)) {
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
        Move-Item -LiteralPath $file.FullName -Destination $target
    }
}
Write-Output 'Missing archived files restored. Existing files and current Config were preserved; original Config remains in the archive.'
