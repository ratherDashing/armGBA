/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Development builds only (make LOG_URL=http://...): at start, the last run's log (log-prev.txt) and
// this one so far are POSTed to a server on the local network, for finding out what happened on a
// console without taking its SD card out. Off unless the build names a URL.

#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARMGBA_LOG_URL)
static void post(const char* path, const char* name) {
	size_t size = 0;
	char* data = readFile(path, &size);
	if (!data || !size) {
		free(data);
		return;
	}
	// The end, if it's long: what happened last is what's wanted.
	size_t const most = 768 * 1024;
	if (size > most) {
		memmove(data, data + size - most, most);
		size = most;
	}
	char url[256];
	snprintf(url, sizeof(url), "%s?name=%s", ARMGBA_LOG_URL, name);
	httpcContext context;
	Result rc = httpcOpenContext(&context, HTTPC_METHOD_POST, url, 0);
	u32 status = 0;
	if (R_SUCCEEDED(rc)) {
		httpcAddRequestHeaderField(&context, "Content-Type", "text/plain");
		httpcAddPostDataRaw(&context, (const u32*) data, size);
		rc = httpcBeginRequest(&context);
		if (R_SUCCEEDED(rc)) {
			rc = httpcGetResponseStatusCodeTimeout(&context, &status, 3000000000ull);
		}
		httpcCloseContext(&context);
	}
	logLine("log upload: %s to %s: %08lX, HTTP %lu", name, url, (unsigned long) rc, (unsigned long) status);
	free(data);
}
#endif

void logUpload(void) {
#if defined(ARMGBA_LOG_URL)
	// The http service holds a POST's body in memory shared with it: none (0) sent every log empty.
	if (R_FAILED(httpcInit(1 << 20))) {
		logLine("log upload: no httpc");
		return;
	}
	post(APP_DIR "/log-prev.txt", "prev");
	post(LOG_FILE, "start");
	httpcExit();
#endif
}
