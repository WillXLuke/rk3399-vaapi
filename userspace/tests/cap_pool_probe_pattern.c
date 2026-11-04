/*
 * cap_pool_probe_pattern.c — synthetic regression test for the
 * iter5 sonnet C4 / iter6 candidate A "cap_pool resolution-change race."
 *
 * Exercises the surface-allocation pattern that originally tripped
 * REQBUFS-EBUSY on the iter5-end driver: vaCreateSurfaces at one
 * resolution, then vaDestroySurfaces, then vaCreateSurfaces at a
 * different resolution. iter6's REINIT discipline + cap_pool's
 * REQBUFS(0)-on-CAPTURE-and-OUTPUT during S_FMT-on-resolution-change
 * (CreateSurfaces2 in surface.c) closes this race; this test anchors
 * that fact with a deterministic repro.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o cap_pool_probe_pattern \
 *       cap_pool_probe_pattern.c \
 *       $(pkg-config --cflags --libs libva libva-drm)
 *
 * Run:
 *   LIBVA_DRIVER_NAME=v4l2_request \
 *   LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/video1 \
 *   LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/media0 \
 *     ./cap_pool_probe_pattern
 *
 * Pass criterion (on iter6 driver and later):
 *   - Exit code 0
 *   - No "REQBUFS" / "EBUSY" / "Unable to request buffers" /
 *     "Unable to set format" lines on the v4l2-request driver's stderr
 *   - vainfo or visual inspection confirms the test program reached
 *     the "PASS" line on stdout
 *
 * Fail behavior pre-iter5: vaCreateSurfaces at the second resolution
 * would emit REQBUFS-EBUSY because OUTPUT/CAPTURE buffers from the
 * first allocation hadn't been torn down before S_FMT was attempted
 * on the new resolution. iter5's CreateSurfaces2 added the dual
 * REQBUFS(0) drain; iter6's REINIT keeps the OUTPUT pool's request_fd
 * lifecycle clean across the destroy-recreate cycle.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>

#define DRM_RENDER_NODE "/dev/dri/renderD128"

static const char *va_status_str(VAStatus s)
{
	return vaErrorStr(s);
}

#define VA_OK_OR_FAIL(call, msg) do { \
	VAStatus _vs = (call); \
	if (_vs != VA_STATUS_SUCCESS) { \
		fprintf(stderr, "FAIL: %s: %s (0x%x)\n", \
			(msg), va_status_str(_vs), _vs); \
		return 10; \
	} \
} while (0)

int main(void)
{
	int drm_fd;
	VADisplay dpy;
	int va_major = 0, va_minor = 0;
	VAConfigID config = VA_INVALID_ID;
	VAContextID context = VA_INVALID_ID;
	VASurfaceID small_surfaces[4];
	VASurfaceID big_surfaces[4];
	const unsigned int small_w = 128, small_h = 128;
	const unsigned int big_w = 1920, big_h = 1080;

	/* Open render node + libva display. */
	drm_fd = open(DRM_RENDER_NODE, O_RDWR);
	if (drm_fd < 0) {
		fprintf(stderr, "FAIL: open(%s): %s\n",
			DRM_RENDER_NODE, strerror(errno));
		return 1;
	}

	dpy = vaGetDisplayDRM(drm_fd);
	if (dpy == NULL) {
		fprintf(stderr, "FAIL: vaGetDisplayDRM returned NULL\n");
		close(drm_fd);
		return 2;
	}

	VA_OK_OR_FAIL(vaInitialize(dpy, &va_major, &va_minor),
		      "vaInitialize");
	printf("libva %d.%d initialized via %s\n", va_major, va_minor,
	       DRM_RENDER_NODE);

	/*
	 * vaCreateConfig with H.264 Main + VLD entrypoint forces our
	 * driver's RequestCreateConfig to set up the H.264 decode path,
	 * which is the path that reaches CreateSurfaces2 (and the
	 * resolution-change handling there).
	 */
	VA_OK_OR_FAIL(vaCreateConfig(dpy, VAProfileH264Main, VAEntrypointVLD,
				     NULL, 0, &config),
		      "vaCreateConfig(H264Main, VLD)");

	/* Phase 1: allocate small probe-pattern surfaces.
	 *
	 * iter5 sonnet C4 specified the race as vaCreateSurfaces(small)
	 * then vaCreateSurfaces(big), allocation-only — matching mpv's
	 * libplacebo probe pattern that surfaced the original failure.
	 * No context creation needed for the C4 race; the cap_pool's
	 * resolution-change handling lives in CreateSurfaces2 itself
	 * (REQBUFS(0)+S_FMT pair on the OUTPUT queue, cap_pool_destroy
	 * + cap_pool_init on the CAPTURE queue).
	 *
	 * (vaCreateContext + recreate at a new resolution surfaced an
	 * additional STREAMON-on-recreate failure during iter7 Phase 7
	 * verification. That's iter8 candidate; out of scope for the C4
	 * regression test.)
	 */
	printf("Phase 1: vaCreateSurfaces %ux%u, count=4\n", small_w, small_h);
	VA_OK_OR_FAIL(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420,
				       small_w, small_h, small_surfaces, 4,
				       NULL, 0),
		      "vaCreateSurfaces (small)");

	/* Phase 2: dispose small surfaces. Our driver's CreateSurfaces2
	 * keeps the cap_pool initialized at the small resolution; the
	 * pool will be torn down + rebuilt by Phase 3's resolution-change
	 * branch in CreateSurfaces2.
	 */
	printf("Phase 2: vaDestroySurfaces (small)\n");
	VA_OK_OR_FAIL(vaDestroySurfaces(dpy, small_surfaces, 4),
		      "vaDestroySurfaces (small)");

	/* Phase 3: allocate at the new (much larger) resolution. This is
	 * the C4 race-hitting path: pre-iter5 hit REQBUFS-EBUSY because
	 * CAPTURE/OUTPUT buffers from the small allocation hadn't been
	 * torn down before S_FMT on the new size. iter5's CreateSurfaces2
	 * added the dual REQBUFS(0) drain; iter7 also adds OUTPUT pool
	 * teardown for the case where a context-bound resolution change
	 * leaves the request_pool stale (defensive — not exercised in
	 * this no-context test path).
	 */
	printf("Phase 3: vaCreateSurfaces %ux%u, count=4 (resolution change)\n",
	       big_w, big_h);
	VA_OK_OR_FAIL(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420,
				       big_w, big_h, big_surfaces, 4,
				       NULL, 0),
		      "vaCreateSurfaces (big)");

	/* Phase 4: clean up. */
	printf("Phase 4: cleanup\n");
	VA_OK_OR_FAIL(vaDestroySurfaces(dpy, big_surfaces, 4),
		      "vaDestroySurfaces (big)");
	VA_OK_OR_FAIL(vaDestroyConfig(dpy, config),
		      "vaDestroyConfig");
	VA_OK_OR_FAIL(vaTerminate(dpy),
		      "vaTerminate");
	close(drm_fd);
	(void)context; /* unused in the C4-faithful no-context test path */

	printf("PASS: cap_pool probe-pattern resolution-change handled cleanly.\n");
	printf("Inspect driver stderr for absence of REQBUFS/EBUSY/Unable lines.\n");
	return 0;
}
