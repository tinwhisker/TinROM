#!/usr/bin/env bash
# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Setup: put ps1-bare-metal in extern/, create the Python virtual
# environment its build scripts expect at ./env, and fetch mkpsxiso for the
# boot disc. Safe to run again: each step that is already done is left
# alone. scripts/bootstrap.ps1 is the same for Windows.
#
#     ./scripts/bootstrap.sh              asks which ps1-bare-metal
#     ./scripts/bootstrap.sh --supported  the revision tinrom records
#     ./scripts/bootstrap.sh --latest     upstream's newest main
set -euo pipefail

cd "$(dirname "$0")/.."

DIR=extern/ps1-bare-metal

die() { echo "bootstrap: $*" >&2; exit 1; }

command -v git >/dev/null || die "git is not on PATH."

choice=""

for arg in "$@"; do
	case "$arg" in
		--supported) choice=supported ;;
		--latest)    choice=latest ;;
		*)           die "unknown option $arg (--supported or --latest)" ;;
	esac
done

# --- ps1-bare-metal ----------------------------------------------------------
#
# A git submodule. tinrom's tree records one commit of it - the supported
# revision, which tinrom is built and tested against - and "supported" checks
# that out. "latest" checks out upstream's newest main instead: upstream
# changes its API, so that may need fixing in tinrom before it builds, and
# once it does, committing the submodule makes it the new supported one.

bm() { git -C "$DIR" "$@"; }

supported=$(git ls-files -s "$DIR" | awk '$1 == "160000" { print $2 }')
[ -n "$supported" ] || die "$DIR is not a submodule of this checkout."

if [ -z "$choice" ]; then
	choice=supported

	# Asked only of a person: piped or scripted, it is the supported one.
	if [ -t 0 ]; then
		echo "ps1-bare-metal:"
		echo "  1) supported - ${supported:0:12}, the revision tinrom is built against"
		echo "  2) latest    - upstream's newest main, which may need fixes in tinrom"
		read -r -p "Choose [1]: " answer

		case "$answer" in
			""|1) choice=supported ;;
			2)    choice=latest ;;
			*)    die "answer 1 or 2." ;;
		esac
	fi
fi

# A copy from before it was a submodule with no .git of its own - from when
# tinrom vendored it - cannot become one in place.
if [ -d "$DIR" ] && [ ! -e "$DIR/.git" ] && [ -n "$(ls -A "$DIR")" ]; then
	die "$DIR holds a copy of ps1-bare-metal that is not a checkout. Delete it
and run this again."
fi

git submodule sync -q -- "$DIR"
git submodule init -q -- "$DIR"

# Local changes would be lost moving to another revision, and NOTICE.md says
# it is used unmodified anyway: stop and say so rather than overwrite them.
if [ -e "$DIR/.git" ] && [ -n "$(bm status --porcelain)" ]; then
	die "$DIR has local changes, which this would lose:
$(bm status --short)"
fi

if [ "$choice" = latest ]; then
	git submodule update -q --remote --checkout -- "$DIR" \
		|| die "could not fetch upstream's main."
else
	git submodule update -q --checkout -- "$DIR" \
		|| die "could not check out ${supported:0:12}."
fi

head=$(bm rev-parse HEAD)

if [ "$head" = "$supported" ]; then
	echo "ps1-bare-metal is at ${head:0:12}, the supported revision."
else
	echo "ps1-bare-metal is at ${head:0:12}, upstream's latest; tinrom supports ${supported:0:12}."

	if bm merge-base --is-ancestor "$supported" "$head" 2>/dev/null; then
		echo
		bm --no-pager log --oneline --no-decorate "$supported..$head"
		echo
	fi

	echo "Rebuild and run scripts/syntax-check.sh: upstream changes its API. If it"
	echo "all works, commit it as the new supported revision:"
	echo "    git add $DIR && git commit"
	echo "To go back: ./scripts/bootstrap.sh --supported"
fi

# --- Python environment ------------------------------------------------------

# One env/ serves one kind of Python. Running "venv" over an env made by a
# different one - a native Windows Python's, from bootstrap.ps1, under MSYS2
# or WSL here - rewrites its pyvenv.cfg and leaves it unable to start. So an
# env that already works here is left alone, and a native Windows one is
# never touched.
envpy=""

if ./env/bin/python -c "" 2>/dev/null; then
	echo "The Python environment in env/ works; leaving it as it is."
	envpy=./env/bin/python
elif [ -e env/Scripts/python.exe ]; then
	echo "Note: env/ belongs to a native Windows Python (scripts/bootstrap.ps1),"
	echo "so it is left alone. Build from that side, or give this one its own"
	echo "checkout: two kinds of Python cannot share one env/."
else
	# ps1-bare-metal's build scripts locate this by path; it has to exist
	# even though nothing in the build needs a package installed into it.
	python3 -m venv env || die "creating the virtual environment failed."
	envpy=./env/bin/python
fi

# pyserial is only for talking to a console with tinrom_host, so a failure
# here must not stop anyone building. Deliberately not installing
# ps1-bare-metal's requirements: see tools/requirements.txt.
if [ -n "$envpy" ] && ! "$envpy" -m pip install -q -r tools/requirements.txt; then
	echo
	echo "Note: pyserial did not install. The build does not need it; you will"
	echo "only miss it when running tools/tinrom_host against a real console."
fi

# --- mkpsxiso ----------------------------------------------------------------

# Only the boot disc needs it, so a failed download is a note, not a stop.
if ! "${envpy:-python3}" tools/fetch_mkpsxiso.py; then
	echo
	echo "Note: mkpsxiso did not download. Everything but the boot disc builds"
	echo "without it; install it on PATH, or run tools/fetch_mkpsxiso.py again."
fi

echo
echo "Done. Now:  cmake --preset debug && cmake --build build/debug"
