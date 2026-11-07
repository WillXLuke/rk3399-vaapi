/* Import a driver-exported NV12 DMA-BUF into EGL, the way Sunshine does. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <linux/videodev2.h>

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif

static EGLImageKHR make_image(EGLDisplay dpy, int fd, unsigned w, unsigned h,
			      unsigned pitch, unsigned offset, unsigned fourcc)
{
	EGLint attrs[32];
	int n = 0;
	attrs[n++] = EGL_WIDTH;  attrs[n++] = (EGLint)w;
	attrs[n++] = EGL_HEIGHT; attrs[n++] = (EGLint)h;
	attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[n++] = (EGLint)fourcc;
	attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT; attrs[n++] = fd;
	attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[n++] = (EGLint)offset;
	attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attrs[n++] = (EGLint)pitch;
	attrs[n++] = EGL_NONE;
	typedef EGLImageKHR (*create_image_t)(EGLDisplay, EGLContext, EGLenum,
					      EGLClientBuffer, const EGLint *);
	static create_image_t create_image;

	if (!create_image)
		create_image = (create_image_t)eglGetProcAddress("eglCreateImageKHR");
	if (!create_image)
		return EGL_NO_IMAGE_KHR;
	return create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
			    attrs);
}

int main(void)
{
	int fd = open("/dev/dri/renderD128", O_RDWR);
	VADisplay dpy;
	VAConfigID cfg;
	VAContextID ctx;
	VASurfaceID surf;
	VADRMPRIMESurfaceDescriptor prime;
	struct gbm_device *gbm;
	EGLDisplay edpy;
	EGLContext ectx;
	EGLSurface esurf;
	EGLint major, minor;
	EGLImageKHR img_y, img_uv;
	VAStatus st;

	dpy = vaGetDisplayDRM(fd);
	if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
		puts("vaInitialize failed"); return 1;
	}
	vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice, NULL, 0, &cfg);
	vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1600, 900, &surf, 1, NULL, 0);
	vaCreateContext(dpy, cfg, 1600, 900, VA_PROGRESSIVE, &surf, 1, &ctx);

	memset(&prime, 0, sizeof(prime));
	st = vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
				   VA_EXPORT_SURFACE_WRITE_ONLY |
				   VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime);
	if (st != VA_STATUS_SUCCESS) { printf("export failed %d\n", st); return 1; }
	printf("export: %ux%u objects=%u layers=%u fd=%d size=%u pitch=%u\n",
	       prime.width, prime.height, prime.num_objects, prime.num_layers,
	       prime.objects[0].fd, prime.objects[0].size,
	       prime.layers[0].pitch[0]);

	gbm = gbm_create_device(fd);
	if (!gbm) { puts("gbm_create_device failed"); return 1; }

	(void)gbm;
	edpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
				     EGL_DEFAULT_DISPLAY, NULL);
	if (edpy == EGL_NO_DISPLAY) { puts("no EGL display"); return 1; }
	if (!eglInitialize(edpy, &major, &minor)) { puts("eglInitialize failed"); return 1; }
	if (!eglBindAPI(EGL_OPENGL_ES_API)) { puts("bindAPI failed"); return 1; }

	EGLint cfg_attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			       EGL_NONE };
	EGLConfig ecfg; EGLint ncfg;
	if (!eglChooseConfig(edpy, cfg_attrs, &ecfg, 1, &ncfg) || !ncfg) {
		puts("no EGL config"); return 1;
	}
	EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	esurf = eglCreatePbufferSurface(edpy, ecfg, pb);
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	ectx = eglCreateContext(edpy, ecfg, EGL_NO_CONTEXT, ctx_attrs);
	if (!eglMakeCurrent(edpy, esurf, esurf, ectx)) {
		printf("makeCurrent failed 0x%x\n", eglGetError()); return 1;
	}

	img_y = make_image(edpy, prime.objects[0].fd, prime.width, prime.height,
			   prime.layers[0].pitch[0], prime.layers[0].offset[0],
			   prime.layers[0].drm_format);
	printf("Y  image (%08x): %p err=0x%x\n", prime.layers[0].drm_format,
	       (void *)img_y, eglGetError());

	img_uv = make_image(edpy, prime.objects[0].fd, prime.width / 2,
			    prime.height / 2, prime.layers[1].pitch[0],
			    prime.layers[1].offset[0],
			    prime.layers[1].drm_format);
	printf("UV image (%08x): %p err=0x%x\n", prime.layers[1].drm_format,
	       (void *)img_uv, eglGetError());

	return (img_y != EGL_NO_IMAGE_KHR && img_uv != EGL_NO_IMAGE_KHR) ? 0 : 1;
}
