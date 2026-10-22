# Modules

## Adding one

1. `mkdir src/modules/mything`
2. Write `src/modules/mything/mything.c`:

```c
#include "modules/modules.h"

static TinromResult myThingEnter(TinromContext *ctx) {
    uiNotImplemented(ctx, "My thing");
    return TINROM_OK;
}

const TinromModule tinromModule_mything = {
    .id      = "mything",
    .name    = "My thing",
    .summary = "One line, shown under the menu",
    .minTier = TINROM_TIER_ADVANCED,
    .flags   = TINROM_MODULE_MENU,
    .init    = NULL,
    .enter   = myThingEnter,
    .tick    = NULL,
    .shutdown = NULL
};
```

3. In `cmake/options.cmake`, add
   `tinromModuleOption(mything ON "What it does")`.
4. In `cmake/modules.cmake`, add
   `set(TINROM_MODULE_SRC_mything src/modules/mything/mything.c)`.

That is all. CMake regenerates `generated/tinrom/module_list.h`, the registry
picks it up, and the menu grows an entry.

The symbol name must be `tinromModule_<id>` and the id must match the option
and the directory. The generated table pastes the id straight into the symbol,
so a mismatch is a link error naming exactly what went wrong.

## The flags

| Flag | Meaning |
| --- | --- |
| `TINROM_MODULE_MENU` | Appears in the main menu. Almost always set. |
| `TINROM_MODULE_BACKGROUND` | `tick()` runs every frame while other screens are up. |
| `TINROM_MODULE_AUTOSTART` | May be chosen as the autoboot target. |
| `TINROM_MODULE_ESSENTIAL` | Cannot be disabled at runtime. About and Settings only. |
| `TINROM_MODULE_OWNS_SIO` | Claims SIO1. Only one such module should be enabled. |

## Tiers

| Tier | Intent | Modules |
| --- | --- | --- |
| Basic | Boot a disc, manage saves, play a CD, nothing in the way | about, settingsui, cdboot, memcard, cdplay |
| Advanced | Plus the things an enthusiast wants | sysinfo, filebrowser, cheats, flasher, regionpatch, terminal, irc |
| Developer | Everything | siocons, gdb, hexedit, pcdrv |

A module's `minTier` is the lowest tier it appears at. It is a menu filter,
not an access control - see `docs/architecture.md`.

## Dependencies between modules

There is no dependency mechanism in the registry, deliberately: a module that
needs another one's services should be calling through the HAL instead. Where
that genuinely is not possible - `gdb` and `pcdrv` both need the serial
link `siocons` owns - the dependency is enforced in `cmake/options.cmake` as
a configure-time error, so it fails at the point where it can be explained.

The limit is 32 modules, because `moduleEnableMask` is a `uint32_t`. If that
ever needs raising, both `TINROM_MAX_MODULES` and the mask type change
together, and the settings version must be bumped.
