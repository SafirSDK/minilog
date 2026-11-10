# What a Developer Command Prompt sets — cl, link and the SDK on the PATH,
# INCLUDE, LIB and the rest — which Ninja needs and the runner does not provide:
# vcvars64.bat of the newest Visual Studio with the C++ tools, and every variable
# it sets or changes carried into the later steps of the calling job.
#
# Written out here rather than taken from an action so that no third-party code
# runs in the job that builds and publishes the release (ilammy/msvc-dev-cmd did
# this until it stopped being maintained). It lives in a script, called by both
# the windows and windows-coverage jobs, so that the PATH handling below exists
# once rather than in two copies that have to stay in step.

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'no Visual Studio with the C++ tools found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

# Environment names are case-insensitive on Windows, and so is @{}.
$before = @{}
Get-ChildItem env: | ForEach-Object { $before[$_.Name] = $_.Value }
$after = cmd /c "`"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed with exit code $LASTEXITCODE" }

foreach ($line in $after) {
  if ($line -notmatch '^([^=]+)=(.*)$') { continue }
  $name, $value = $Matches[1], $Matches[2]
  if ($before[$name] -eq $value) { continue }
  if ($name -eq 'Path') {
    # The directories vcvars added, through GITHUB_PATH, which is how a step
    # adds to the PATH of the steps after it. Each line written there goes in
    # front of the ones before it, so they are written last first, to come out
    # in vcvars' order.
    $old   = $before['Path'] -split ';'
    $added = @($value -split ';' | Where-Object { $_ -and $_ -notin $old })
    [array]::Reverse($added)
    $added | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
  } else {
    "$name=$value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
  }
}
