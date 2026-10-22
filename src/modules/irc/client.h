/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Internal interfaces of the IRC module. See docs/irc.md.
 */
#pragma once

#include <stdbool.h>
#include "comms/config.h"
#include "core/app.h"

/* Who to be on the server: a bookmark's, with its passwords unsealed. */
typedef struct {
	char nick[COMMS_NICK_MAX];
	char user[16];
	char realName[COMMS_REALNAME_MAX];
	char channel[COMMS_CHANNEL_MAX];
	char serverPassword[COMMS_SECRET_MAX];
	char nickServPassword[COMMS_SECRET_MAX];
	bool showJoins;
} IrcSessionLogin;

/**
 * Dials `bookmark` and runs a session until the user leaves it. The
 * passwords in `login` are wiped as soon as they have been sent.
 */
void ircSession(TinromContext *ctx, const CommsBookmark *bookmark,
	IrcSessionLogin *login);
