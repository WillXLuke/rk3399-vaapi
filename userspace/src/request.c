/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
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

#include "buffer.h"
#include "cap_pool.h"
#include "config.h"
#include "context.h"
#include "encoder.h"
#include "image.h"
#include "picture.h"
#include "request_pool.h"
#include "subpicture.h"
#include "surface.h"

#include "autoconfig.h"

#include <va/va_backend.h>

#include "request.h"
#include "utils.h"
#include "v4l2.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include "hevc-ctrls/v4l2-hevc-ext-controls.h"

/*
 * fresnel-fourier iter4 Phase 6 commit Z + iter7 Phase 6 (B1a): device-path
 * auto-detect via media controller topology with decoder-entity discrimination.
 *
 * Pre-iter4 the backend hardcoded /dev/video0 + /dev/media0. On Linux 7.0 the
 * udev/probe order changed and rockchip-rga (an RGB color converter, no codec
 * support) now claims /dev/video0 — the legacy default returns an empty
 * profile list. iter4 commit Z replaced enumeration-order discovery with
 * media-topology discovery.
 *
 * iter7 (B1a): the iter4 walk treated the hantro-vpu driver name as a single
 * unit, but hantro-vpu registers BOTH encoder and decoder entities under one
 * /dev/mediaN on RK3399. iter4's "pick the first V4L_VIDEO interface" could
 * land on the encoder. iter7 walks ENTITIES looking for
 * MEDIA_ENT_F_PROC_VIDEO_DECODER, then follows the kernel's link graph
 * (data link from proc to IO entity, interface link from IO entity to V4L
 * interface) to the correct /dev/videoN.
 *
 * Two-pass to prefer rkvdec: pass 1 accepts only "rkvdec" (multi-codec
 * decoder, 3 of 5 codecs); pass 2 accepts any known decoder driver. On
 * RK3399 this makes auto-detect always pick rkvdec when available.
 *
 * iter4-B1b (multi-decoder routing — open BOTH rkvdec AND hantro from one
 * backend instance, dispatch per codec) is still deferred. Post-iter7 the
 * backend opens one decoder per process; MPEG-2/VP8 (hantro) still need
 * explicit LIBVA_V4L2_REQUEST_VIDEO_PATH override when iter7's pass-1
 * lands on rkvdec.
 *
 * Escape hatch: LIBVA_V4L2_REQUEST_NO_AUTODETECT=1 reverts to legacy
 * hardcoded /dev/video0 + /dev/media0 for callers that relied on it.
 */
static const char * const known_decoder_drivers[] = {
	"rkvdec",
	"hantro-vpu",
	"rpi-hevc-dec",  /* iter40: Pi 5 / CM5 stateless HEVC */
#ifdef HAVE_DAEDALUS_V4L2
	"daedalus_v4l2", /* phase 8.10: Pi 5 daemon-backed VP9/AV1/H264 */
#endif
	"cedrus",
	"sun4i_csi",
	NULL
};

static int resolve_dev_node(uint32_t major, uint32_t minor, char *out, size_t out_sz)
{
	char sysfs_path[64], target[256];
	ssize_t n;
	const char *base;

	snprintf(sysfs_path, sizeof sysfs_path, "/sys/dev/char/%u:%u", major, minor);
	n = readlink(sysfs_path, target, sizeof target - 1);
	if (n < 0)
		return -1;
	target[n] = '\0';
	base = strrchr(target, '/');
	base = base ? base + 1 : target;
	snprintf(out, out_sz, "/dev/%s", base);
	return 0;
}

/*
 * iter7 B1a: walk topology graph from decoder-proc entity to its V4L_VIDEO
 * interface. Returns 0 + sets video_out on success, -1 if this media device
 * has no decoder entity (e.g. encoder-only device).
 *
 * Algorithm (per Phase 5 review, empirically validated against
 * boltzmann:~/src/linux-rockchip):
 *   1. For each entity E with function == MEDIA_ENT_F_PROC_VIDEO_DECODER:
 *   2.   Find IO entity neighbors via DATA links (entity↔entity).
 *   3.   Find the V4L_VIDEO interface via INTERFACE links from those IO
 *        neighbors.
 *   4.   Resolve interface.devnode.major:minor to /dev/videoN.
 *
 * Two-call MEDIA_IOC_G_TOPOLOGY pattern (Phase 5 IMP-3): first call gets
 * counts; second call fills the three arrays after we allocate them.
 *
 * Link discrimination via MEDIA_LNK_FL_INTERFACE_LINK (1U<<28):
 * data links have flags & MEDIA_LNK_FL_INTERFACE_LINK == 0; interface
 * links have it set. source_id/sink_id ordering is not guaranteed —
 * check both endpoints.
 */
static int find_decoder_video_node_via_topology(int media_fd,
						char *video_out,
						size_t video_out_sz)
{
	struct media_v2_topology topo;
	struct media_v2_entity *entities = NULL;
	struct media_v2_interface *interfaces = NULL;
	struct media_v2_link *links = NULL;
	struct media_v2_pad *pads = NULL;
	int ret = -1;
	unsigned int i, j;

	memset(&topo, 0, sizeof topo);
	if (ioctl(media_fd, MEDIA_IOC_G_TOPOLOGY, &topo) < 0)
		return -1;
	if (topo.num_entities == 0 || topo.num_interfaces == 0 ||
	    topo.num_links == 0 || topo.num_pads == 0)
		return -1;

	entities   = calloc(topo.num_entities,   sizeof *entities);
	interfaces = calloc(topo.num_interfaces, sizeof *interfaces);
	links      = calloc(topo.num_links,      sizeof *links);
	pads       = calloc(topo.num_pads,       sizeof *pads);
	if (!entities || !interfaces || !links || !pads)
		goto out;

	topo.ptr_entities   = (uintptr_t)entities;
	topo.ptr_interfaces = (uintptr_t)interfaces;
	topo.ptr_links      = (uintptr_t)links;
	topo.ptr_pads       = (uintptr_t)pads;

	if (ioctl(media_fd, MEDIA_IOC_G_TOPOLOGY, &topo) < 0)
		goto out;

	for (i = 0; i < topo.num_entities; i++) {
		uint32_t proc_id;
		uint32_t proc_pad_ids[16];
		uint32_t io_entity_ids[16];
		unsigned int proc_pad_count = 0;
		unsigned int io_count = 0;

		if (entities[i].function != MEDIA_ENT_F_PROC_VIDEO_DECODER)
			continue;
		proc_id = entities[i].id;

		/* Step 2a: collect pads belonging to the proc entity. Data
		 * links connect PADs, not entities directly. */
		for (j = 0; j < topo.num_pads; j++) {
			if (pads[j].entity_id != proc_id)
				continue;
			if (proc_pad_count < (sizeof proc_pad_ids /
					      sizeof proc_pad_ids[0]))
				proc_pad_ids[proc_pad_count++] = pads[j].id;
		}

		/* Step 2b: walk data links. For each link with either endpoint
		 * in proc_pad_ids[], the other endpoint is a pad belonging to
		 * an IO neighbor. Resolve that pad's entity_id via pads[]. */
		for (j = 0; j < topo.num_links; j++) {
			uint32_t other_pad = 0;
			unsigned int k;

			if (links[j].flags & MEDIA_LNK_FL_INTERFACE_LINK)
				continue;
			for (k = 0; k < proc_pad_count; k++) {
				if (links[j].source_id == proc_pad_ids[k])
					other_pad = links[j].sink_id;
				else if (links[j].sink_id == proc_pad_ids[k])
					other_pad = links[j].source_id;
				if (other_pad != 0)
					break;
			}
			if (other_pad == 0)
				continue;
			/* Resolve other_pad to its entity_id. */
			for (k = 0; k < topo.num_pads; k++) {
				if (pads[k].id != other_pad)
					continue;
				if (io_count < (sizeof io_entity_ids /
						sizeof io_entity_ids[0]))
					io_entity_ids[io_count++] =
						pads[k].entity_id;
				break;
			}
		}

		/* Step 3-4: find an interface link from any IO entity neighbor;
		 * resolve devnode for the linked V4L_VIDEO interface.
		 * Interface links connect interfaces↔entities directly (not
		 * via pads), so source_id/sink_id is an entity ID on one side
		 * and an interface ID on the other. */
		for (j = 0; j < topo.num_links; j++) {
			uint32_t intf_id = 0;
			unsigned int k;

			if (!(links[j].flags & MEDIA_LNK_FL_INTERFACE_LINK))
				continue;
			for (k = 0; k < io_count; k++) {
				if (links[j].source_id == io_entity_ids[k])
					intf_id = links[j].sink_id;
				else if (links[j].sink_id == io_entity_ids[k])
					intf_id = links[j].source_id;
				if (intf_id != 0)
					break;
			}
			if (intf_id == 0)
				continue;

			for (k = 0; k < topo.num_interfaces; k++) {
				if (interfaces[k].id != intf_id)
					continue;
				if (interfaces[k].intf_type !=
				    MEDIA_INTF_T_V4L_VIDEO)
					break;
				if (resolve_dev_node(
					    interfaces[k].devnode.major,
					    interfaces[k].devnode.minor,
					    video_out, video_out_sz) == 0)
					ret = 0;
				break;
			}
			if (ret == 0)
				goto out;
		}
	}

out:
	free(entities);
	free(interfaces);
	free(links);
	free(pads);
	return ret;
}

/*
 * iter7 B1a: two-pass walk of /dev/media0..N. Pass 1 accepts only "rkvdec"
 * (multi-codec decoder serving 3 of 5 codecs). Pass 2 accepts any
 * known_decoder_drivers entry. Within each pass, the chosen media device
 * must ALSO contain at least one MEDIA_ENT_F_PROC_VIDEO_DECODER entity —
 * guards against encoder-only devices that happen to share the same driver
 * name (e.g. hantro-vpu encoder vs decoder inside one /dev/mediaN).
 */
/*
 * iter38: locate a /dev/mediaN whose driver name matches `want_driver`
 * AND exposes at least one MEDIA_ENT_F_PROC_VIDEO_DECODER entity (rules
 * out encoder-only devices sharing the same driver name). Resolves the
 * matching /dev/videoM via topology graph walk.
 *
 * `want_driver`:
 *   - non-NULL → match only that exact driver name
 *   - NULL     → match any name in known_decoder_drivers[]
 */
/*
 * iter2 (ampere-kernel-decoders campaign) — runtime probe for the
 * V4L2 stateless HEVC EXT_SPS_{ST,LT}_RPS controls added in
 * Linux 7.0 (Casanova VDPU381/VDPU383 series). Returns true iff BOTH
 * controls are registered on the given fd. Stored per-fd on
 * driver_data so the multi-device-probe model (iter38) doesn't
 * silently misbehave when codec routing switches devices.
 *
 * The two CIDs together are the gate — neither alone is meaningful
 * without the other (st-RPS + lt-RPS arrays both need to be set to
 * match the SPS num_short_term_ref_pic_sets / num_long_term_ref_pics_sps
 * counts). Old kernels (RK3399 rkvdec on linux 6.x) register neither;
 * RK3588 rkvdec (VDPU381/383 path) registers both.
 *
 * Reference: phase4_plan_iter2.md §Step 3 in
 * ~/src/ampere-kernel-decoders/.
 */
static bool probe_hevc_ext_sps_rps_controls(int video_fd)
{
	struct v4l2_queryctrl q;

	if (video_fd < 0)
		return false;

	memset(&q, 0, sizeof(q));
	q.id = V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS;
	if (ioctl(video_fd, VIDIOC_QUERYCTRL, &q) < 0)
		return false;

	memset(&q, 0, sizeof(q));
	q.id = V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS;
	if (ioctl(video_fd, VIDIOC_QUERYCTRL, &q) < 0)
		return false;

	return true;
}

static int find_decoder_device_by_driver(const char *want_driver,
					 char *video_out, size_t video_out_sz,
					 char *media_out, size_t media_out_sz)
{
	struct media_device_info info;
	char path[32];
	const char * const *kd;
	int fd, i;

	for (i = 0; i < 16; i++) {
		bool match;

		snprintf(path, sizeof path, "/dev/media%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(&info, 0, sizeof info);
		if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) != 0) {
			close(fd);
			continue;
		}
		if (want_driver != NULL) {
			match = (strcmp(info.driver, want_driver) == 0);
		} else {
			match = false;
			for (kd = known_decoder_drivers; *kd; kd++) {
				if (strcmp(info.driver, *kd) == 0) {
					match = true;
					break;
				}
			}
		}
		if (!match) {
			close(fd);
			continue;
		}
		if (find_decoder_video_node_via_topology(
			    fd, video_out, video_out_sz) == 0) {
			snprintf(media_out, media_out_sz, "%s", path);
			close(fd);
			return 0;
		}
		close(fd);
	}
	return -1;
}

static int find_codec_device(char *video_out, size_t video_out_sz,
			     char *media_out, size_t media_out_sz)
{
	if (find_decoder_device_by_driver("rkvdec",
					  video_out, video_out_sz,
					  media_out, media_out_sz) == 0)
		return 0;
	return find_decoder_device_by_driver(NULL,
					     video_out, video_out_sz,
					     media_out, media_out_sz);
}

/*
 * iter38: profile → which physical decoder device should serve it on
 * RK3399. Returns 'r' for rkvdec, 'h' for hantro, '?' for unknown.
 *
 * This is RK3399-shaped knowledge — a more general impl would interrogate
 * each open device's supported OUTPUT formats. For the campaign-scope
 * five codecs, the mapping is stable and explicit.
 */
char request_device_kind_for_profile(VAProfile profile);
char request_device_kind_for_profile(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
	case VAProfileHEVCMain:
	case VAProfileVP9Profile0:
		return 'r';
	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
	case VAProfileVP8Version0_3:
		return 'h';
	case VAProfileAV1Profile0:
		/*
		 * ampere-av1-enablement Phase 2: RK3588 vpu981 dedicated
		 * AV1 hantro instance. 'a' kind dispatches to
		 * driver_data->video_fd_vpu981. On hosts without the AV1
		 * instance the fd stays -1 and RequestQueryConfigProfiles
		 * never enumerates AV1, so this branch is unreachable for
		 * non-RK3588 hosts.
		 */
		return 'a';
	default:
		return '?';
	}
}

/*
 * iter38: retarget driver_data->{video,media}_fd to the device kind
 * required by `profile`. If a switch is needed, tear down any per-device
 * pool state so the next RequestCreateContext rebuilds it against the
 * new device. Returns 0 on success, -1 if the required device wasn't
 * probed (e.g. trying VP8 on a system without hantro).
 *
 * Safe to call repeatedly with the same profile: if the active fd
 * already matches, the function is a no-op.
 */
int request_switch_device_for_profile(struct request_data *driver_data,
				      VAProfile profile);
int request_switch_device_for_profile(struct request_data *driver_data,
				      VAProfile profile)
{
	char kind = request_device_kind_for_profile(profile);
	int target_video, target_media;

	/*
	 * iter40: HEVC override when rpi-hevc-dec is probed. The static
	 * table (request_device_kind_for_profile) maps HEVC → 'r' (rkvdec)
	 * because that's the canonical RK path. On Pi 5 there's no rkvdec
	 * — rpi-hevc-dec is the only decoder. When BOTH would be present
	 * (hypothetical mixed board), prefer rpi-hevc-dec for HEVC.
	 *
	 * Other rkvdec-routed profiles (VP9, H.264) stay on 'r' because
	 * rpi-hevc-dec is HEVC-only.
	 */
	if ((profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10) &&
	    driver_data->video_fd_rpi_hevc_dec >= 0 &&
	    driver_data->media_fd_rpi_hevc_dec >= 0) {
		kind = 'p';
	}

#ifdef HAVE_DAEDALUS_V4L2
	/*
	 * LIBVA-1: VP9/AV1/H.264 → daedalus_v4l2 when the daemon-backed
	 * decoder fd is open.  Pi 5 has no rkvdec (those profiles map to
	 * 'r' by default → video_fd_rkvdec = -1 → "stay on whatever's
	 * active" fallback would put H.264 frames on rpi-hevc-dec's fd
	 * and S_FMT would fail).  Re-route to the daedalus daemon instead.
	 *
	 * HEVC stays on 'p' (rpi-hevc-dec is HEVC-only — daedalus would
	 * accept it via FFmpeg, but rpi-hevc-dec has the GPU-backed
	 * hardware path so it's the right choice on this SoC).
	 *
	 * AV1 'a' kind (RK3588 vpu981) wins ONLY if vpu981 was probed.
	 * On a Pi 5 the vpu981 slot stays -1, so we still route AV1 to
	 * daedalus here.  Check video_fd_vpu981 to preserve the RK3588
	 * priority for that case.
	 */
	if (driver_data->video_fd_daedalus >= 0 &&
	    driver_data->media_fd_daedalus >= 0) {
		switch (profile) {
		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
		case VAProfileVP9Profile0:
			kind = 'd';
			break;
		case VAProfileAV1Profile0:
			if (driver_data->video_fd_vpu981 < 0)
				kind = 'd';
			break;
		default:
			break;
		}
	}
#endif

	if (kind == 'r') {
		target_video = driver_data->video_fd_rkvdec;
		target_media = driver_data->media_fd_rkvdec;
	} else if (kind == 'h') {
		target_video = driver_data->video_fd_hantro;
		target_media = driver_data->media_fd_hantro;
	} else if (kind == 'p') {
		target_video = driver_data->video_fd_rpi_hevc_dec;
		target_media = driver_data->media_fd_rpi_hevc_dec;
	} else if (kind == 'a') {
		target_video = driver_data->video_fd_vpu981;
		target_media = driver_data->media_fd_vpu981;
#ifdef HAVE_DAEDALUS_V4L2
	} else if (kind == 'd') {
		target_video = driver_data->video_fd_daedalus;
		target_media = driver_data->media_fd_daedalus;
#endif
	} else {
		return -1;
	}

	/* Either side never probed (e.g. env-override single-device init,
	 * or this kind isn't present on the running kernel) → tolerate by
	 * staying on whatever's already active. RequestCreateConfig still
	 * accepted the profile via the format probe, so the active fd
	 * supports it. */
	if (target_video < 0 || target_media < 0)
		return 0;

	if (driver_data->video_fd == target_video &&
	    driver_data->media_fd == target_media)
		return 0;  /* already active, nothing to do */

	/*
	 * Tear down any per-device pool state. cap_pool needs capture_type,
	 * which comes from video_format. Both rkvdec and hantro use
	 * V4L2_PIX_FMT_NV12 MPLANE on RK3399 (verified Phase 0 inventory)
	 * so the MPLANE form is always right here.
	 */
	if (driver_data->capture_pool.initialized) {
		cap_pool_destroy(&driver_data->capture_pool,
				 driver_data->video_fd,
				 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	}
	if (driver_data->output_pool.initialized)
		request_pool_destroy(&driver_data->output_pool);

	/* video_format is a static-ref pointer; re-probe on next
	 * CreateContext since the new device's format menu may differ. */
	driver_data->video_format = NULL;
	driver_data->fmt_valid = false;

	driver_data->video_fd = target_video;
	driver_data->media_fd = target_media;
	return 0;
}

/* Set default visibility for the init function only. */
VAStatus __attribute__((visibility("default")))
VA_DRIVER_INIT_FUNC(VADriverContextP context);

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP context)
{
	struct request_data *driver_data;
	struct VADriverVTable *vtable = context->vtable;
	VAStatus status;
	unsigned int capabilities;
	unsigned int capabilities_required;
	int video_fd = -1;
	int media_fd = -1;
	char *video_path;
	char *media_path;
	int rc;

	context->version_major = VA_MAJOR_VERSION;
	context->version_minor = VA_MINOR_VERSION;
	context->max_profiles = V4L2_REQUEST_MAX_PROFILES;
	context->max_entrypoints = V4L2_REQUEST_MAX_ENTRYPOINTS;
	context->max_attributes = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES;
	context->max_image_formats = V4L2_REQUEST_MAX_IMAGE_FORMATS;
	context->max_subpic_formats = V4L2_REQUEST_MAX_SUBPIC_FORMATS;
	context->max_display_attributes = V4L2_REQUEST_MAX_DISPLAY_ATTRIBUTES;
	context->str_vendor = V4L2_REQUEST_STR_VENDOR;

	vtable->vaTerminate = RequestTerminate;
	vtable->vaQueryConfigEntrypoints = RequestQueryConfigEntrypoints;
	vtable->vaQueryConfigProfiles = RequestQueryConfigProfiles;
	vtable->vaQueryConfigEntrypoints = RequestQueryConfigEntrypoints;
	vtable->vaQueryConfigAttributes = RequestQueryConfigAttributes;
	vtable->vaCreateConfig = RequestCreateConfig;
	vtable->vaDestroyConfig = RequestDestroyConfig;
	vtable->vaGetConfigAttributes = RequestGetConfigAttributes;
	vtable->vaCreateSurfaces = RequestCreateSurfaces;
	vtable->vaCreateSurfaces2 = RequestCreateSurfaces2;
	vtable->vaDestroySurfaces = RequestDestroySurfaces;
	vtable->vaExportSurfaceHandle = RequestExportSurfaceHandle;
	vtable->vaCreateContext = RequestCreateContext;
	vtable->vaDestroyContext = RequestDestroyContext;
	vtable->vaCreateBuffer = RequestCreateBuffer;
	vtable->vaBufferSetNumElements = RequestBufferSetNumElements;
	vtable->vaMapBuffer = RequestMapBuffer;
	vtable->vaUnmapBuffer = RequestUnmapBuffer;
	vtable->vaDestroyBuffer = RequestDestroyBuffer;
	vtable->vaBufferInfo = RequestBufferInfo;
	vtable->vaAcquireBufferHandle = RequestAcquireBufferHandle;
	vtable->vaReleaseBufferHandle = RequestReleaseBufferHandle;
	vtable->vaBeginPicture = RequestBeginPicture;
	vtable->vaRenderPicture = RequestRenderPicture;
	vtable->vaEndPicture = RequestEndPicture;
	vtable->vaSyncSurface = RequestSyncSurface;
	vtable->vaQuerySurfaceAttributes = RequestQuerySurfaceAttributes;
	vtable->vaQuerySurfaceStatus = RequestQuerySurfaceStatus;
	vtable->vaPutSurface = RequestPutSurface;
	vtable->vaQueryImageFormats = RequestQueryImageFormats;
	vtable->vaCreateImage = RequestCreateImage;
	vtable->vaDeriveImage = RequestDeriveImage;
	vtable->vaDestroyImage = RequestDestroyImage;
	vtable->vaSetImagePalette = RequestSetImagePalette;
	vtable->vaGetImage = RequestGetImage;
	vtable->vaPutImage = RequestPutImage;
	vtable->vaQuerySubpictureFormats = RequestQuerySubpictureFormats;
	vtable->vaCreateSubpicture = RequestCreateSubpicture;
	vtable->vaDestroySubpicture = RequestDestroySubpicture;
	vtable->vaSetSubpictureImage = RequestSetSubpictureImage;
	vtable->vaSetSubpictureChromakey = RequestSetSubpictureChromakey;
	vtable->vaSetSubpictureGlobalAlpha = RequestSetSubpictureGlobalAlpha;
	vtable->vaAssociateSubpicture = RequestAssociateSubpicture;
	vtable->vaDeassociateSubpicture = RequestDeassociateSubpicture;
	vtable->vaQueryDisplayAttributes = RequestQueryDisplayAttributes;
	vtable->vaGetDisplayAttributes = RequestGetDisplayAttributes;
	vtable->vaSetDisplayAttributes = RequestSetDisplayAttributes;
	vtable->vaLockSurface = RequestLockSurface;
	vtable->vaUnlockSurface = RequestUnlockSurface;

	driver_data = malloc(sizeof(*driver_data));
	memset(driver_data, 0, sizeof(*driver_data));

	context->pDriverData = driver_data;

	object_heap_init(&driver_data->config_heap,
			 sizeof(struct object_config), CONFIG_ID_OFFSET);
	object_heap_init(&driver_data->context_heap,
			 sizeof(struct object_context), CONTEXT_ID_OFFSET);
	object_heap_init(&driver_data->surface_heap,
			 sizeof(struct object_surface), SURFACE_ID_OFFSET);
	object_heap_init(&driver_data->buffer_heap,
			 sizeof(struct object_buffer), BUFFER_ID_OFFSET);
	object_heap_init(&driver_data->image_heap, sizeof(struct object_image),
			 IMAGE_ID_OFFSET);

	static char auto_video[32], auto_media[32];
	bool auto_media_set = false;

	video_path = getenv("LIBVA_V4L2_REQUEST_VIDEO_PATH");
	if (video_path == NULL) {
		if (getenv("LIBVA_V4L2_REQUEST_NO_AUTODETECT")) {
			video_path = "/dev/video0";
		} else if (find_codec_device(auto_video, sizeof auto_video,
					     auto_media, sizeof auto_media) == 0) {
			video_path = auto_video;
			auto_media_set = true;
			request_log("auto-selected codec device: %s + %s\n",
				    auto_video, auto_media);
		} else {
			video_path = "/dev/video0";
		}
	}

	video_fd = open(video_path, O_RDWR | O_NONBLOCK);
	if (video_fd < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_query_capabilities(video_fd, &capabilities);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	capabilities_required = V4L2_CAP_STREAMING;

	if ((capabilities & capabilities_required) != capabilities_required) {
		request_log("Missing required driver capabilities\n");
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	media_path = getenv("LIBVA_V4L2_REQUEST_MEDIA_PATH");
	if (media_path == NULL) {
		if (auto_media_set)
			media_path = auto_media;
		else
			media_path = "/dev/media0";
	}

	media_fd = open(media_path, O_RDWR | O_NONBLOCK);
	if (media_fd < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	driver_data->video_fd = video_fd;
	driver_data->media_fd = media_fd;
	driver_data->video_fd_rkvdec = -1;
	driver_data->media_fd_rkvdec = -1;
	driver_data->video_fd_hantro = -1;
	driver_data->media_fd_hantro = -1;
	driver_data->video_fd_rpi_hevc_dec = -1;
	driver_data->media_fd_rpi_hevc_dec = -1;
	driver_data->video_fd_daedalus = -1;
	driver_data->media_fd_daedalus = -1;
	driver_data->video_fd_vpu981 = -1;
	driver_data->media_fd_vpu981 = -1;
	driver_data->video_fd_enc = -1;
	driver_data->media_fd_enc = -1;
	driver_data->encode_active = false;

	/*
	 * iter38: probe BOTH rkvdec and hantro-vpu so a single libva session
	 * can serve all 5 codecs. Tag the primary fd (already opened above)
	 * by inspecting which driver the media_fd is on, then probe the OTHER
	 * driver and open its fds if present. RequestCreateConfig retargets
	 * driver_data->{video,media}_fd to the right pair per profile.
	 *
	 * Skip the alt-probe when the user provided explicit
	 * LIBVA_V4L2_REQUEST_VIDEO_PATH / MEDIA_PATH overrides — they signal
	 * a specific single device intent.
	 */
	if (!getenv("LIBVA_V4L2_REQUEST_VIDEO_PATH") &&
	    !getenv("LIBVA_V4L2_REQUEST_MEDIA_PATH")) {
		struct media_device_info info;
		const char *primary_driver = NULL;
		const char *alt_driver = NULL;

		memset(&info, 0, sizeof info);
		if (ioctl(media_fd, MEDIA_IOC_DEVICE_INFO, &info) == 0) {
			if (strcmp(info.driver, "rkvdec") == 0) {
				primary_driver = "rkvdec";
				alt_driver = "hantro-vpu";
				driver_data->video_fd_rkvdec = video_fd;
				driver_data->media_fd_rkvdec = media_fd;
			} else if (strcmp(info.driver, "hantro-vpu") == 0) {
				primary_driver = "hantro-vpu";
				alt_driver = "rkvdec";
				driver_data->video_fd_hantro = video_fd;
				driver_data->media_fd_hantro = media_fd;
			} else if (strcmp(info.driver, "rpi-hevc-dec") == 0) {
				/* iter40 + LIBVA-1: Pi 5 / CM5.  rpi-hevc-dec is
				 * HEVC-only.  If daedalus_v4l2 is ALSO loaded (Pi 5
				 * mixed deployment — out-of-tree daemon-backed
				 * decoder for VP9/AV1/H264), pick it up as the alt
				 * so VP9/AV1/H264 have somewhere to land. */
				primary_driver = "rpi-hevc-dec";
#ifdef HAVE_DAEDALUS_V4L2
				alt_driver = "daedalus_v4l2";
#else
				alt_driver = NULL;
#endif
				driver_data->video_fd_rpi_hevc_dec = video_fd;
				driver_data->media_fd_rpi_hevc_dec = media_fd;
#ifdef HAVE_DAEDALUS_V4L2
			} else if (strcmp(info.driver, "daedalus_v4l2") == 0) {
				/* phase 8.10 + LIBVA-1: Pi 5 daemon-backed decoder.
				 * VP9 / AV1 / H.264 route through it via the 'd'
				 * kind below.  On a mixed-driver box where
				 * rpi-hevc-dec is ALSO loaded, pick it up as the
				 * alt so HEVC has somewhere to land too — find_
				 * codec_device's known_decoder_drivers[] order
				 * normally puts rpi-hevc-dec first (we hit the
				 * other branch in practice), but symmetric handling
				 * keeps us correct if probe order ever flips. */
				primary_driver = "daedalus_v4l2";
				alt_driver = "rpi-hevc-dec";
				driver_data->video_fd_daedalus = video_fd;
				driver_data->media_fd_daedalus = media_fd;
#endif
			}
		}

		if (alt_driver != NULL) {
			static char alt_video[32], alt_media[32];
			if (find_decoder_device_by_driver(alt_driver,
							  alt_video, sizeof alt_video,
							  alt_media, sizeof alt_media) == 0) {
				int alt_v = open(alt_video, O_RDWR | O_NONBLOCK);
				int alt_m = (alt_v >= 0) ? open(alt_media, O_RDWR | O_NONBLOCK) : -1;
				if (alt_v >= 0 && alt_m >= 0) {
					/* Dispatch into the matching per-driver slot.
					 * iter38 only had rkvdec/hantro pairs; iter40 +
					 * LIBVA-1 extended this to rpi-hevc-dec and
					 * daedalus_v4l2 for the Pi 5 mixed-decoder
					 * deployment. */
					if (strcmp(alt_driver, "rkvdec") == 0) {
						driver_data->video_fd_rkvdec = alt_v;
						driver_data->media_fd_rkvdec = alt_m;
					} else if (strcmp(alt_driver, "hantro-vpu") == 0) {
						driver_data->video_fd_hantro = alt_v;
						driver_data->media_fd_hantro = alt_m;
					} else if (strcmp(alt_driver, "rpi-hevc-dec") == 0) {
						driver_data->video_fd_rpi_hevc_dec = alt_v;
						driver_data->media_fd_rpi_hevc_dec = alt_m;
#ifdef HAVE_DAEDALUS_V4L2
					} else if (strcmp(alt_driver, "daedalus_v4l2") == 0) {
						driver_data->video_fd_daedalus = alt_v;
						driver_data->media_fd_daedalus = alt_m;
#endif
					} else {
						/* Shouldn't happen — primary_driver branches
						 * above only set alt_driver to one of the
						 * names handled here.  Close and move on. */
						close(alt_v);
						close(alt_m);
						alt_v = -1;
						alt_m = -1;
					}
					if (alt_v >= 0) {
						request_log("iter38: also opened %s decoder at %s + %s\n",
							    alt_driver, alt_video, alt_media);
					}
				} else {
					if (alt_v >= 0) close(alt_v);
					if (alt_m >= 0) close(alt_m);
				}
			}
		}
		(void)primary_driver;

		/*
		 * ampere-av1-enablement Phase 2: walk hantro-vpu media nodes
		 * for a SECOND one that advertises V4L2_PIX_FMT_AV1_FRAME
		 * (AV1F) as OUTPUT pixfmt. RK3588 has 3 hantro-vpu instances
		 * (legacy MPEG2/VP8 decoder, vepu121 encoder, vpu981 AV1
		 * decoder) all reporting driver="hantro-vpu" / model="hantro-
		 * vpu" — so OUTPUT-format probe is the only reliable
		 * disambiguator that doesn't depend on parsing card-name
		 * strings (which are DTS-dependent). First match wins.
		 *
		 * On non-RK3588 hosts the slot stays -1; RequestQueryConfig
		 * Profiles' AV1 push then no-ops because any_fd_supports_
		 * output_format() returns false for AV1F.
		 */
		{
			int i;
			char path[32], av1_video[32];

			for (i = 0; i < 16; i++) {
				int mfd, vfd;
				struct media_device_info info;

				snprintf(path, sizeof path, "/dev/media%d", i);
				mfd = open(path, O_RDWR | O_NONBLOCK);
				if (mfd < 0) continue;
				memset(&info, 0, sizeof info);
				if (ioctl(mfd, MEDIA_IOC_DEVICE_INFO, &info) != 0 ||
				    strcmp(info.driver, "hantro-vpu") != 0) {
					close(mfd);
					continue;
				}
				if (find_decoder_video_node_via_topology(
					    mfd, av1_video, sizeof av1_video) != 0) {
					close(mfd);
					continue;
				}
				vfd = open(av1_video, O_RDWR | O_NONBLOCK);
				if (vfd < 0) {
					close(mfd);
					continue;
				}
				if (!v4l2_find_format(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_AV1_FRAME) &&
				    !v4l2_find_format(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_AV1_FRAME)) {
					close(vfd);
					close(mfd);
					continue;
				}
				driver_data->video_fd_vpu981 = vfd;
				driver_data->media_fd_vpu981 = mfd;
				request_log("ampere-av1: vpu981 AV1 decoder at %s + %s\n",
					    av1_video, path);
				break;
			}
		}
	}

	/*
	 * RK3399 VEPU2 H.264 encoder: independent of the decoder probes.
	 * Auto-detected by driver/card/CAPTURE-format so node renumbering
	 * across module reloads is harmless.  Absence is not an error — the
	 * decoder keeps working and EncSlice simply isn't advertised.
	 */
	if (encoder_device_probe(&driver_data->video_fd_enc,
				 &driver_data->media_fd_enc) == 0) {
		request_log("encoder: hantro VEPU2 H.264 encoder probed\n");
	} else {
		driver_data->video_fd_enc = -1;
		driver_data->media_fd_enc = -1;
		request_log("encoder: no hantro VEPU2 encoder found\n");
	}

	/*
	 * iter2 (ampere-kernel-decoders): probe the new HEVC EXT_SPS_RPS
	 * controls on each rkvdec/hantro fd. Result is consumed by
	 * h265_set_controls per-codec gate. Per-fd storage matches the
	 * iter38 multi-device-probe pattern (Phase 5 review item).
	 */
	driver_data->has_hevc_ext_sps_rps_rkvdec =
		probe_hevc_ext_sps_rps_controls(driver_data->video_fd_rkvdec);
	driver_data->has_hevc_ext_sps_rps_hantro =
		probe_hevc_ext_sps_rps_controls(driver_data->video_fd_hantro);
	driver_data->has_hevc_ext_sps_rps_rpi_hevc_dec =
		probe_hevc_ext_sps_rps_controls(driver_data->video_fd_rpi_hevc_dec);
	if (driver_data->has_hevc_ext_sps_rps_rkvdec) {
		request_log("iter2: kernel registers HEVC EXT_SPS_{ST,LT}_RPS "
			    "controls on rkvdec fd (will route through "
			    "vendored GStreamer parser)\n");
	}
	if (driver_data->video_fd_rpi_hevc_dec >= 0) {
		request_log("iter40: also opened rpi-hevc-dec at video_fd=%d "
			    "media_fd=%d (Pi 5 HEVC stateless)\n",
			    driver_data->video_fd_rpi_hevc_dec,
			    driver_data->media_fd_rpi_hevc_dec);
	}
#ifdef HAVE_DAEDALUS_V4L2
	if (driver_data->video_fd_daedalus >= 0) {
		request_log("phase 8.10: opened daedalus_v4l2 at video_fd=%d "
			    "media_fd=%d (Pi 5 daemon-backed VP9/AV1/H264)\n",
			    driver_data->video_fd_daedalus,
			    driver_data->media_fd_daedalus);
	}
#endif

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	status = VA_STATUS_ERROR_OPERATION_FAILED;

	if (video_fd >= 0)
		close(video_fd);

	if (media_fd >= 0)
		close(media_fd);

complete:
	return status;
}

VAStatus RequestTerminate(VADriverContextP context)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_buffer *buffer_object;
	struct object_image *image_object;
	struct object_surface *surface_object;
	struct object_context *context_object;
	struct object_config *config_object;
	int iterator;

	/*
	 * Tear down the OUTPUT buffer pool before closing video_fd so
	 * the munmap calls in request_pool_destroy() can still touch the
	 * mmap regions (which are tied to that fd's lifetime).
	 */
	request_pool_destroy(&driver_data->output_pool);

	/*
	 * iter38: close both probed device pairs. video_fd / media_fd above
	 * are ACTIVE pointers into one of these pairs; close the underlying
	 * fds explicitly. Each may be -1 if its device wasn't found.
	 */
	if (driver_data->video_fd_rkvdec >= 0)
		close(driver_data->video_fd_rkvdec);
	if (driver_data->media_fd_rkvdec >= 0)
		close(driver_data->media_fd_rkvdec);
	if (driver_data->video_fd_hantro >= 0)
		close(driver_data->video_fd_hantro);
	if (driver_data->media_fd_hantro >= 0)
		close(driver_data->media_fd_hantro);
	if (driver_data->video_fd_rpi_hevc_dec >= 0)
		close(driver_data->video_fd_rpi_hevc_dec);
	if (driver_data->media_fd_rpi_hevc_dec >= 0)
		close(driver_data->media_fd_rpi_hevc_dec);
	if (driver_data->video_fd_vpu981 >= 0)
		close(driver_data->video_fd_vpu981);
	if (driver_data->media_fd_vpu981 >= 0)
		close(driver_data->media_fd_vpu981);
	if (driver_data->video_fd_enc >= 0)
		close(driver_data->video_fd_enc);
	if (driver_data->media_fd_enc >= 0)
		close(driver_data->media_fd_enc);
#ifdef HAVE_DAEDALUS_V4L2
	if (driver_data->video_fd_daedalus >= 0)
		close(driver_data->video_fd_daedalus);
	if (driver_data->media_fd_daedalus >= 0)
		close(driver_data->media_fd_daedalus);
#endif
	/* Fall back to direct close if neither alt fd captured the active
	 * pair (env-override path). */
	if (driver_data->video_fd_rkvdec < 0 && driver_data->video_fd_hantro < 0) {
		if (driver_data->video_fd >= 0)
			close(driver_data->video_fd);
		if (driver_data->media_fd >= 0)
			close(driver_data->media_fd);
	}

	/* Cleanup leftover buffers. */

	image_object = (struct object_image *)
		object_heap_first(&driver_data->image_heap, &iterator);
	while (image_object != NULL) {
		RequestDestroyImage(context, (VAImageID)image_object->base.id);
		image_object = (struct object_image *)
			object_heap_next(&driver_data->image_heap, &iterator);
	}

	object_heap_destroy(&driver_data->image_heap);

	buffer_object = (struct object_buffer *)
		object_heap_first(&driver_data->buffer_heap, &iterator);
	while (buffer_object != NULL) {
		RequestDestroyBuffer(context,
				     (VABufferID)buffer_object->base.id);
		buffer_object = (struct object_buffer *)
			object_heap_next(&driver_data->buffer_heap, &iterator);
	}

	object_heap_destroy(&driver_data->buffer_heap);

	surface_object = (struct object_surface *)
		object_heap_first(&driver_data->surface_heap, &iterator);
	while (surface_object != NULL) {
		RequestDestroySurfaces(context,
				      (VASurfaceID *)&surface_object->base.id, 1);
		surface_object = (struct object_surface *)
			object_heap_next(&driver_data->surface_heap, &iterator);
	}

	object_heap_destroy(&driver_data->surface_heap);

	context_object = (struct object_context *)
		object_heap_first(&driver_data->context_heap, &iterator);
	while (context_object != NULL) {
		RequestDestroyContext(context,
				      (VAContextID)context_object->base.id);
		context_object = (struct object_context *)
			object_heap_next(&driver_data->context_heap, &iterator);
	}

	object_heap_destroy(&driver_data->context_heap);

	config_object = (struct object_config *)
		object_heap_first(&driver_data->config_heap, &iterator);
	while (config_object != NULL) {
		RequestDestroyConfig(context,
				     (VAConfigID)config_object->base.id);
		config_object = (struct object_config *)
			object_heap_next(&driver_data->config_heap, &iterator);
	}

	object_heap_destroy(&driver_data->config_heap);

	free(context->pDriverData);
	context->pDriverData = NULL;

	return VA_STATUS_SUCCESS;
}
