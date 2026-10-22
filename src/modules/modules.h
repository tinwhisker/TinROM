/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Convenience header for module implementations. A module only ever needs
 * this: it should not reach for hardware registers directly (that is the
 * HAL's job) or for another module's internals.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "core/app.h"
#include "core/log.h"
#include "core/module.h"
#include "core/result.h"
#include "core/settings.h"
#include "hal/hal.h"
#include "ui/menu.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"
