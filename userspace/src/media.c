/*
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>

#include <linux/media.h>

#include "media.h"
#include "utils.h"

int media_request_alloc(int media_fd)
{
	int fd;
	int rc;

	rc = ioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &fd);
	if (rc < 0) {
		request_log("Unable to allocate media request: %s\n",
			    strerror(errno));
		return -1;
	}

	return fd;
}

int media_request_reinit(int request_fd)
{
	int rc;

	rc = ioctl(request_fd, MEDIA_REQUEST_IOC_REINIT, NULL);
	if (rc < 0) {
		request_log("Unable to reinit media request: %s\n",
			    strerror(errno));
		return -1;
	}

	return 0;
}

int media_request_queue(int request_fd)
{
	int rc;

	rc = ioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL);
	if (rc < 0) {
		request_log("Unable to queue media request: %s\n",
			    strerror(errno));
		return -1;
	}

	return 0;
}

int media_request_wait_completion_timeout(int request_fd, int timeout_ms)
{
	/* poll() instead of select(): Firefox's RDD seccomp policy admits
	 * poll/ppoll but not select/pselect6 (as of FF150). Functionally
	 * equivalent here — the media request fd signals completion via
	 * exceptional condition, mapped to POLLPRI for poll(). */
	struct pollfd pfd = { .fd = request_fd, .events = POLLPRI };
	int rc;

	rc = poll(&pfd, 1, timeout_ms);
	if (rc == 0) {
		request_log("Timeout when waiting for media request\n");
		return -1;
	} else if (rc < 0) {
		request_log("Unable to poll media request: %s\n",
			    strerror(errno));
		return -1;
	}

	return 0;
}

int media_request_wait_completion(int request_fd)
{
	return media_request_wait_completion_timeout(request_fd, 300);
}
