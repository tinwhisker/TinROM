/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * See ui/launch.h. Lifted out of the cdboot module, which had the only copy
 * of it, so the file browser launches through exactly the same code rather
 * than a second version of it that drifts.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "core/app.h"
#include "core/log.h"
#include "core/psexe.h"
#include "core/result.h"
#include "hal/hal.h"
#include "hal/handoff.h"
#include "ui/launch.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

static uint8_t headerBuffer[TINROM_EXE_HEADER_SIZE];

static void step(TinromContext *ctx, const char *message) {
	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, "Launch", "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, message);
	uiEndFrame(&ctx->ui);
}

TinromResult uiLaunchExe(
	TinromContext *ctx,
	const char    *what,
	UILaunchReader read,
	void          *user,
	uint32_t       headerSector,
	void         (*quiesce)(void *user)
) {
	if (!ctx || !read)
		return TINROM_ERR_BAD_ARG;

	TinromResult result = read(user, headerSector, headerBuffer, 1);

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Reading the executable", result);

		return result;
	}

	TinromExeHeader header;

	result = tinromExeParse(headerBuffer, &header);

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Reading the executable header", result);

		return result;
	}

	/*
	 * Everything needed to tell a loading problem from a handover problem,
	 * logged before either can happen.
	 */
	TINROM_LOG("launch: load %08x + %u bytes, pc %08x gp %08x sp %08x",
		(unsigned int) header.loadAddress, (unsigned int) header.loadSize,
		(unsigned int) header.pc, (unsigned int) header.gp,
		(unsigned int) (header.spBase + header.spOffset));

	/*
	 * An entry point outside the region being loaded cannot work: the console
	 * would jump into whatever happened to be in RAM there, which shows up as
	 * a reserved instruction at the entry point and looks exactly like a
	 * failed load. Refuse it here, where it can still be explained.
	 */
	if ((header.pc < header.loadAddress)
		|| ((header.pc - header.loadAddress) >= header.loadSize)) {
		char body[256];

		snprintf(body, sizeof(body),
			"This executable's entry point is outside the code it loads.\n\n"
			"  entry\t%08x\n"
			"  loads\t%08x .. %08x\n\n"
			"There would be nothing there to run.",
			(unsigned int) header.pc,
			(unsigned int) header.loadAddress,
			(unsigned int) (header.loadAddress + header.loadSize));
		uiMessageBox(ctx, "Cannot load", body);

		return TINROM_ERR_CORRUPT;
	}

	/* --- can this be handed over at all? --- */

	TinromHandoffPlan plan;

	result = tinromHandoffPlan(header.loadAddress, header.loadSize, &plan);

	if (result != TINROM_OK) {
		char body[256];

		snprintf(body, sizeof(body),
			"This executable cannot be loaded.\n\n"
			"  wants\t%08x, %u KB\n"
			"  reaches\t%08x\n"
			"  tinrom\tends at %08x\n\n"
			"%s\n"
			"See docs/cd-boot.md.",
			(unsigned int) header.loadAddress,
			(unsigned int) (header.loadSize / 1024),
			(unsigned int) (header.loadAddress + header.loadSize),
			(unsigned int) tinromHandoffProtectedEnd(),
			(result == TINROM_ERR_NO_SPACE)
				? "It reaches too far up RAM to leave room to work."
				: "That is not somewhere an executable can go.");
		uiMessageBox(ctx, "Cannot load", body);

		return result;
	}

	char confirm[192];

	snprintf(confirm, sizeof(confirm),
		"tinrom will hand the console to\n\n  %s\n\n"
		"There is no way back without a reset.", what);

	if (!uiConfirm(ctx, "Launch", confirm, false))
		return TINROM_ERR_ABORTED;

	/* --- load ---
	 *
	 * Two reads. The front of the payload lands on tinrom itself, so it goes
	 * to the staging area and the relocated stub puts it in place at the last
	 * moment; everything past tinrom is read straight to where it belongs.
	 * The split is on a sector boundary, which is what makes it two reads
	 * rather than a copy.
	 */
	uint32_t payloadSector = headerSector + 1;

	/*
	 * The exception handler goes first, before a single byte is read. It
	 * lives in the resident monitor at the top of RAM, which is exactly
	 * where the staging area and a large payload land; a vector left
	 * pointing at it while it is overwritten is a crash waiting for the
	 * first exception. It is forgotten as well as removed, so nothing reads
	 * its state once the load has put game code there.
	 */
	tinromDebugRelease();

	if (plan.stagedLength) {
		step(ctx, "Loading...");

		result = read(
			user, payloadSector, (void *) (uintptr_t) plan.stagingAddress,
			plan.stagedLength / UI_LAUNCH_SECTOR_SIZE
		);

		if (result != TINROM_OK) {
			/*
			 * tinrom is intact - the staged part is parked above it - but the
			 * monitor under the staging area may not be. Put a fresh copy
			 * back before anything calls into it.
			 */
			tinromDebugLoad();
			uiShowResult(ctx, "Loading the executable", result);

			return result;
		}
	}

	if (plan.directLength) {
		step(ctx, "Loading...");

		result = read(
			user, payloadSector + (plan.stagedLength / UI_LAUNCH_SECTOR_SIZE),
			(void *) (uintptr_t) plan.directAddress,
			plan.directLength / UI_LAUNCH_SECTOR_SIZE
		);

		if (result != TINROM_OK) {
			/*
			 * Past here tinrom has been partly overwritten by the payload,
			 * so there is no screen to complain on and no code to complain
			 * with. Nothing to do but report what we can and let the user
			 * reset.
			 */
			tinromDebugLoad();
			uiShowResult(ctx, "Loading the executable", result);

			return result;
		}
	}

	TINROM_LOG("launch: %s, entering %08x", what, (unsigned int) header.pc);

	if (quiesce)
		quiesce(user);

	tinromHalShutdown();

	tinromHandoffRun(
		&plan, header.pc, header.gp, header.spBase + header.spOffset
	);

	return TINROM_OK; /* not reached */
}
