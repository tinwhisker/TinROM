# extern/

Third-party code. Nothing in here is tinrom's; see `NOTICE.md`.

## ps1-bare-metal

https://github.com/spicyjpeg/ps1-bare-metal (MIT, by spicyjpeg), as a git
submodule: tinrom's tree records which commit of it to use, not its files.
The bootstrap script checks it out.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
```

```bash
./scripts/bootstrap.sh
```

tinrom uses it unmodified. Fixes go upstream rather than being carried here.

### Supported or latest

The bootstrap script asks which revision to check out:

- **1) supported**, the default: the commit tinrom's tree records, which
  tinrom is built and tested against. Just Enter picks it, and so does a run
  with nobody there to answer, such as one from a script or with its input
  redirected.
- **2) latest**: upstream's newest `main`. It lists the upstream commits that
  came with it.

To skip the question, pass `--supported` or `--latest` to `bootstrap.sh`, or
`-Supported` or `-Latest` to `bootstrap.ps1`. `git submodule update --init`
on its own gives the supported revision too.

The configure summary says which one a build is using, and CMake prints a
line when it is not the supported one. Either way it builds; nothing is
refused. Running bootstrap again moves the checkout, but it refuses to touch
one with local changes.

### Making the latest the supported one

**Upstream changes its API.** Registers get renamed and files move,
sometimes in commits marked as breaking, so the latest may need changes in
tinrom before it builds. Rebuild and run `scripts/syntax-check.sh`. Once it
all works, commit the submodule on its own, and that commit becomes the
supported revision for everyone:

```
git add extern/ps1-bare-metal
git commit
```

To go back without committing, run bootstrap with `--supported`
(`-Supported`).

### An older checkout

Until 2026-10 tinrom's bootstrap made a plain clone here, which git ignored,
at the revision named in a text file, `extern/ps1-bare-metal.rev`. That clone
becomes the submodule as it stands: bootstrap registers it and checks out the
supported revision. A copy with no `.git` of its own, from when tinrom
vendored it as a subtree, cannot. Delete it and run bootstrap again.

### Using a checkout somewhere else

Pass `-DTINROM_BARE_METAL_DIR=/path/to/ps1-bare-metal` to CMake.
