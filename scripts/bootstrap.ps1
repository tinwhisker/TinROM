# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Setup on Windows. From the repository root:
#     powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
#     ... -File scripts\bootstrap.ps1 -Supported   the revision tinrom records
#     ... -File scripts\bootstrap.ps1 -Latest      upstream's newest main
#
# Puts ps1-bare-metal in extern\, creates the Python virtual environment its
# build scripts expect at .\env, and fetches mkpsxiso for the boot disc.
# Without -Supported or -Latest it asks which ps1-bare-metal. Safe to run
# again: each step that is already done is left alone.
# scripts/bootstrap.sh is the same elsewhere.
#
# PowerShell names are not case sensitive, so no variable here may share a
# name with the -Supported and -Latest switches: assigning a commit id to
# $supported would try to make it the switch.
#
# Deliberately does NOT set $ErrorActionPreference = "Stop": several git calls
# below are expected to fail and are checked by exit code instead. And no
# "rev^{commit}" anywhere: Git for Windows expands braces in its own command
# line, so it would arrive as "rev^commit".

param([switch]$Supported, [switch]$Latest)

Set-Location (Join-Path $PSScriptRoot "..")

$dir = "extern/ps1-bare-metal"

function Fail($message) {
    Write-Host "bootstrap: $message" -ForegroundColor Red
    exit 1
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is not on PATH." }
if (-not (Test-Path "CMakeLists.txt"))                    { Fail "run this from the repository root." }
if ($Supported -and $Latest)                              { Fail "-Supported or -Latest, not both." }

# --- ps1-bare-metal --------------------------------------------------------
#
# A git submodule. tinrom's tree records one commit of it - the supported
# revision, which tinrom is built and tested against - and "supported" checks
# that out. "latest" checks out upstream's newest main instead: upstream
# changes its API, so that may need fixing in tinrom before it builds, and
# once it does, committing the submodule makes it the new supported one.

$entry = (git ls-files -s $dir) -split '\s+'
if (($entry.Count -lt 2) -or ($entry[0] -ne "160000")) { Fail "$dir is not a submodule of this checkout." }
$supportedRev = $entry[1]
$short        = $supportedRev.Substring(0, 12)

$choice = if ($Latest) { "latest" } else { "supported" }

# Asked only of a person: run from a script or a pipe, it is the supported one.
if (-not $Supported -and -not $Latest -and -not [Console]::IsInputRedirected) {
    Write-Host "ps1-bare-metal:"
    Write-Host "  1) supported - $short, the revision tinrom is built against"
    Write-Host "  2) latest    - upstream's newest main, which may need fixes in tinrom"
    $answer = Read-Host "Choose [1]"

    switch ($answer.Trim()) {
        ""      { $choice = "supported" }
        "1"     { $choice = "supported" }
        "2"     { $choice = "latest" }
        default { Fail "answer 1 or 2." }
    }
}

# A copy from before it was a submodule with no .git of its own - from when
# tinrom vendored it - cannot become one in place.
if ((Test-Path $dir) -and -not (Test-Path "$dir/.git") -and
    (Get-ChildItem $dir -Force | Select-Object -First 1)) {
    Fail "$dir holds a copy of ps1-bare-metal that is not a checkout. Delete it and run this again."
}

git submodule sync -q -- $dir
git submodule init -q -- $dir

# Local changes would be lost moving to another revision, and NOTICE.md says
# it is used unmodified anyway: stop and say so rather than overwrite them.
if (Test-Path "$dir/.git") {
    $changes = (git -C $dir status --porcelain) -join "`n"
    if ($changes) { Fail "$dir has local changes, which this would lose:`n$changes" }
}

if ($choice -eq "latest") {
    git submodule update -q --remote --checkout -- $dir
    if ($LASTEXITCODE -ne 0) { Fail "could not fetch upstream's main." }
} else {
    git submodule update -q --checkout -- $dir
    if ($LASTEXITCODE -ne 0) { Fail "could not check out $short." }
}

$head = git -C $dir rev-parse HEAD

if ($head -eq $supportedRev) {
    Write-Host "ps1-bare-metal is at $($head.Substring(0, 12)), the supported revision."
} else {
    Write-Host "ps1-bare-metal is at $($head.Substring(0, 12)), upstream's latest; tinrom supports $short." -ForegroundColor Yellow

    git -C $dir merge-base --is-ancestor $supportedRev $head 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        git -C $dir --no-pager log --oneline --no-decorate "$supportedRev..$head"
        Write-Host ""
    }

    Write-Host "Rebuild and run scripts/syntax-check.sh: upstream changes its API. If it"
    Write-Host "all works, commit it as the new supported revision:"
    Write-Host "    git add $dir ; git commit"
    Write-Host "To go back: scripts\bootstrap.ps1 -Supported"
}

# --- Python environment ----------------------------------------------------

# This is the native Windows setup: CMake from PowerShell or VS Code runs a
# native Windows Python, from env\Scripts\. An MSYS2 or Cygwin Python - the
# one devkitPro puts on PATH, say - makes a different kind of venv, in env/bin
# with POSIX paths, and if it runs "venv" over a native one it rewrites
# env\pyvenv.cfg to point at itself, which leaves env\Scripts\python.exe
# unable to start. So: a native Python only, and an env that already works
# is left alone. Those setups use scripts/bootstrap.sh instead.

# "Native" is what Python itself says: sys.platform is win32 only there.
function Test-Native($exe, [string[]]$pre) {
    $platform = & $exe @pre -c "import sys; print(sys.platform)" 2>$null
    return ($LASTEXITCODE -eq 0) -and ($platform -eq "win32")
}

$envPython = "env\Scripts\python.exe"

if ((Test-Path $envPython) -and (Test-Native $envPython @())) {
    Write-Host "The Python environment in env\ works; leaving it as it is."
} else {
    # The py launcher, if installed, only ever starts native Pythons; then
    # each "python" on PATH in turn, skipping MSYS2 and Cygwin ones.
    $base = $null; $pre = @()

    if ((Get-Command py -ErrorAction SilentlyContinue) -and (Test-Native "py" @("-3"))) {
        $base = "py"; $pre = @("-3")
    } else {
        foreach ($candidate in (Get-Command python -All -CommandType Application -ErrorAction SilentlyContinue)) {
            if (Test-Native $candidate.Source @()) { $base = $candidate.Source; break }
        }
    }

    if (-not $base) {
        Fail "no native Windows Python 3.10+ found (the py launcher, or python on PATH). An MSYS2 or devkitPro Python is not one: install Python from python.org, or use scripts/bootstrap.sh from MSYS2."
    }

    if (Test-Path "env") {
        Write-Host "Repairing the Python environment in env\ with $base $pre..."
    }

    # ps1-bare-metal's build scripts locate this by path; it has to exist even
    # though nothing in the build needs a package installed into it.
    & $base @pre -m venv env
    if ($LASTEXITCODE -ne 0) { Fail "creating the virtual environment failed." }
    if (-not (Test-Native $envPython @())) { Fail "env\Scripts\python.exe does not start after creating it." }
}

$py = $envPython

# pyserial is only for talking to a console with tinrom_host, so a failure
# here must not stop anyone building. Deliberately not installing
# ps1-bare-metal's requirements: see tools/requirements.txt.
& $py -m pip install -q -r tools/requirements.txt
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Note: pyserial did not install. The build does not need it; you will"
    Write-Host "only miss it when running tools/tinrom_host against a real console."
}

# --- mkpsxiso --------------------------------------------------------------

# Only the boot disc needs it, so a failed download is a note, not a stop.
& $py tools/fetch_mkpsxiso.py
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Note: mkpsxiso did not download. Everything but the boot disc builds"
    Write-Host "without it; install it on PATH, or run tools/fetch_mkpsxiso.py again."
}

Write-Host ""
Write-Host "Done. Now:  cmake --preset debug ; cmake --build build/debug"
