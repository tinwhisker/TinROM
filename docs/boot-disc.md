# The boot disc

`cmake/iso.cmake` drives [mkpsxiso](https://github.com/Lameguy64/mkpsxiso),
which the bootstrap script fetches into `tools/bin/` (pinned to v2.30 and
checked by SHA-256 in `tools/fetch_mkpsxiso.py`; it is GPL-2.0, so tinrom
does not carry it). CMake looks there, then on `PATH`. If it is in neither,
the target degrades to a message rather than failing the configure, so the
other three images still build.

The project file is `iso/tinrom.xml`, configured into the build directory.

## No licence data

A retail PS1 disc starts with licence data that the console checks before it
will boot. tinrom does not ship it - it is Sony's, and shipping it would
contradict the one rule this project has.

So a disc built from this repository:

- boots on a modchipped console
- boots through the swap trick
- boots in emulators
- **does not boot on an unmodified console on its own**

mkpsxiso accepts a `<license>` element pointing at a licence file you supply.
If you want an unmodified console to boot the disc, that is the mechanism, and
extracting that file from a disc you own is your decision to make. The XML has
no such element and this repository will not add one.

## Contents

```
SYSTEM.CNF     BOOT=cdrom:\TINROM.EXE;1
TINROM.EXE     the payload, identical to tinrom.psexe
```

Unless `TINROM_ISO_EXCLUDE_MODULES` leaves modules out of the disc alone:
then `TINROM.EXE` is `tinrom_disc.psexe`, built without them, or the cart's
`tinrom_cart.psexe` when the cart's list is the same. See "What the cart
carries" in [cart-rom.md](cart-rom.md).
