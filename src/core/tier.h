/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

/*
 * Tiers are a *presentation* filter, not a security boundary. A module with
 * minTier == TINROM_TIER_DEVELOPER is still in the image at BASIC; it just
 * does not appear in the menu. This keeps the menu short for someone who only
 * wants to boot an import, without maintaining separate builds.
 *
 * Compiling a module out entirely is the other axis: see cmake/options.cmake.
 */
typedef enum {
	TINROM_TIER_BASIC     = 0,
	TINROM_TIER_ADVANCED  = 1,
	TINROM_TIER_DEVELOPER = 2,
	TINROM_TIER_COUNT     = 3
} TinromTier;

#ifdef __cplusplus
extern "C" {
#endif

const char *tinromTierName(TinromTier tier);
const char *tinromTierDescription(TinromTier tier);

#ifdef __cplusplus
}
#endif
