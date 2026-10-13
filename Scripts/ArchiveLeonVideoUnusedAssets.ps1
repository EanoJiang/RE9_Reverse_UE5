$ErrorActionPreference = 'Stop'
$leonProjectRoot = (Resolve-Path -LiteralPath 'E:\RE9_Leon_UE5').Path.TrimEnd('\')
$leonPlanPath = Join-Path $leonProjectRoot 'Scripts\leon_video_cleanup_plan.json'
$leonPlan = Get-Content -LiteralPath $leonPlanPath -Raw | ConvertFrom-Json
$leonArchiveRoot = [IO.Path]::GetFullPath($leonPlan.archive).TrimEnd('\')
if ($leonPlan.project -ne $leonProjectRoot -or $leonPlan.state -ne 'planned') { throw 'Unexpected project or already applied plan.' }
if ($leonArchiveRoot.StartsWith($leonProjectRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $leonArchiveRoot)) { throw 'Archive must be a new directory outside the project.' }
if ([IO.Path]::GetPathRoot($leonArchiveRoot) -ne [IO.Path]::GetPathRoot($leonProjectRoot)) { throw 'Archive must be on the same drive.' }
if (Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) { throw 'Close Unreal before cleanup.' }

function Get-LeonSafePath([string]$root, [string]$relative) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $resolved.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes allowed root: $relative" }
    return $resolved
}
function Move-LeonRelative([string]$relative, [bool]$restore = $false) {
    $fromRoot = if ($restore) { $leonArchiveRoot } else { $leonProjectRoot }
    $toRoot = if ($restore) { $leonProjectRoot } else { $leonArchiveRoot }
    $from = Get-LeonSafePath $fromRoot $relative
    $to = Get-LeonSafePath $toRoot $relative
    if (-not (Test-Path -LiteralPath $from)) { return }
    if (Test-Path -LiteralPath $to) { throw "Refusing to overwrite destination: $to" }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($to)) -Force | Out-Null
    Move-Item -LiteralPath $from -Destination $to
}

# Check every retained original before the first mutation.
foreach ($file in $leonPlan.retained_files) {
    $path = Get-LeonSafePath $leonProjectRoot $file.path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { throw "Retained asset changed: $path" }
}
foreach ($relative in $leonPlan.critical_nonasset_dependencies) {
    if (-not (Test-Path -LiteralPath (Get-LeonSafePath $leonProjectRoot $relative))) { throw "Missing runtime shader: $relative" }
}
New-Item -ItemType Directory -Path $leonArchiveRoot | Out-Null
Copy-Item -LiteralPath $leonPlanPath -Destination (Join-Path $leonArchiveRoot 'cleanup_plan.json')
New-Item -ItemType Directory -Path (Join-Path $leonArchiveRoot 'Config') | Out-Null
Get-ChildItem -LiteralPath (Join-Path $leonProjectRoot 'Config') -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $leonArchiveRoot ('Config\' + $_.Name))
}

# Rename the Content tree on the same volume; put back only the registry closure.
Move-LeonRelative 'Content'
foreach ($file in $leonPlan.retained_files) { Move-LeonRelative $file.path $true }
foreach ($file in $leonPlan.retained_files) {
    $path = Get-LeonSafePath $leonProjectRoot $file.path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { throw "Restored asset hash mismatch: $path" }
}
Write-Output ('Content retained: ' + $leonPlan.registry_package_count + ' packages')
foreach ($relative in $leonPlan.archive_directories) { Move-LeonRelative $relative }
foreach ($name in $leonPlan.preserve_diagnostics) { Move-LeonRelative ('Diagnostics\' + $name) $true }
foreach ($relative in $leonPlan.archive_files) { Move-LeonRelative $relative }
$leonKeepScripts = @{}
foreach ($name in $leonPlan.preserve_scripts) { $leonKeepScripts[$name] = $true }
Get-ChildItem -LiteralPath (Join-Path $leonProjectRoot 'Scripts') -Force | ForEach-Object {
    if (-not $leonKeepScripts.ContainsKey($_.Name)) { Move-LeonRelative ('Scripts\' + $_.Name) }
}
$leonPlan.state = 'archived'
$leonPlan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $leonPlanPath -Encoding utf8
Copy-Item -LiteralPath $leonPlanPath -Destination (Join-Path $leonArchiveRoot 'cleanup_plan.json') -Force
Write-Output ('Archive completed: ' + $leonArchiveRoot)
