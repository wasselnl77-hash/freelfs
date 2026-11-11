/* GPU read path for dmabuf buffers: EGLImage import, a GLES2 draw into a
 * linear framebuffer, glReadPixels into the surface's CPU copy. It makes
 * tiled and compressed buffers (what a real GPU renders into) readable
 * without the CPU having to know the layout. Compositing itself stays on the
 * CPU, so a machine without a GPU, with llvmpipe, or where this fails for any
 * reason runs the same code as before (linear buffers only, see dmabuf.c).
 *
 * Started lazily by the first client that binds zwp_linux_dmabuf_v1, so a
 * session of shm clients never loads EGL.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
	bool started, ok, have_mods;
	int own_fd;
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint prog, tex, fbo;
	GLint u_opaque;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_mods;
} G;

static bool has_ext(const char *list, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = list; p && (p = strstr(p, name)); p += n)
		if ((p == list || p[-1] == ' ') && (p[n] == ' ' || !p[n]))
			return true;
	return false;
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static void gl_stop(void)
{
	if (G.dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (G.ctx != EGL_NO_CONTEXT)
			eglDestroyContext(G.dpy, G.ctx);
		eglTerminate(G.dpy);
	}
	if (G.gbm)
		gbm_device_destroy(G.gbm);
	if (G.own_fd > 0)
		close(G.own_fd);
	G.own_fd = 0;
	G.dpy = EGL_NO_DISPLAY;
	G.ctx = EGL_NO_CONTEXT;
	G.gbm = NULL;
	G.ok = false;
}

void gl_start(void)
{
	if (G.started)
		return;
	G.started = true;
	G.dpy = EGL_NO_DISPLAY;
	G.ctx = EGL_NO_CONTEXT;
	if (getenv("FLCOMP_NO_GL"))
		return;
	int fd = backend_drm_fd();
	if (fd < 0 && getenv("FLCOMP_RENDER_NODE")) {
		/* tests: a headless flcomp on a real GPU's render node (nothing is displayed) */
		fd = open(getenv("FLCOMP_RENDER_NODE"), O_RDWR | O_CLOEXEC);
		G.own_fd = fd;
	}
	if (fd < 0)
		return;
	G.gbm = gbm_create_device(fd);
	if (!G.gbm)
		return;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_dpy)
		goto fail;
	G.dpy = get_dpy(EGL_PLATFORM_GBM_KHR, G.gbm, NULL);
	EGLint maj, min;
	if (G.dpy == EGL_NO_DISPLAY || !eglInitialize(G.dpy, &maj, &min))
		goto fail;
	const char *ex = eglQueryString(G.dpy, EGL_EXTENSIONS);
	if (!has_ext(ex, "EGL_EXT_image_dma_buf_import") || !has_ext(ex, "EGL_KHR_surfaceless_context") ||
	    !has_ext(ex, "EGL_KHR_image_base"))
		goto fail;
	G.have_mods = has_ext(ex, "EGL_EXT_image_dma_buf_import_modifiers");
	if (!eglBindAPI(EGL_OPENGL_ES_API))
		goto fail;
	EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
	EGLConfig cfg;
	EGLint ncfg = 0;
	if (!eglChooseConfig(G.dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1)
		goto fail;
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	G.ctx = eglCreateContext(G.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (G.ctx == EGL_NO_CONTEXT || !eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, G.ctx))
		goto fail;
	/* Software rendering gains nothing over the mmap copy and costs memory. */
	const char *rend = (const char *)glGetString(GL_RENDERER);
	if (!rend || strstr(rend, "llvmpipe") || strstr(rend, "softpipe") || strstr(rend, "swrast") ||
	    strstr(rend, "Software"))
		goto fail;
	if (!has_ext((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image"))
		goto fail;
	G.create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	G.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	G.image_target =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	G.query_mods = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	if (!G.create_image || !G.destroy_image || !G.image_target || (G.have_mods && !G.query_mods))
		goto fail;

	GLuint vs = compile(GL_VERTEX_SHADER,
			    "attribute vec2 p; varying vec2 t;"
			    "void main(){ t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }");
	GLuint fs = compile(GL_FRAGMENT_SHADER,
			    "precision mediump float; varying vec2 t; uniform sampler2D s; uniform float opaque;"
			    "void main(){ vec4 c = texture2D(s, t); "
			    "gl_FragColor = vec4(c.b, c.g, c.r, opaque > 0.5 ? 1.0 : c.a); }");
	if (!vs || !fs)
		goto fail;
	G.prog = glCreateProgram();
	glAttachShader(G.prog, vs);
	glAttachShader(G.prog, fs);
	glBindAttribLocation(G.prog, 0, "p");
	glLinkProgram(G.prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = 0;
	glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
	if (!ok)
		goto fail;
	G.u_opaque = glGetUniformLocation(G.prog, "opaque");
	glGenTextures(1, &G.tex);
	glGenFramebuffers(1, &G.fbo);
	G.ok = true;
	fprintf(stderr, "flcomp: dmabuf through the GPU (%s)\n", rend);
	/* a note for clients: buffers with several planes (AMD's DCC) are accepted. A browser that finds it
	 * (FreeBWS) may use the GPU path; an older flcomp does not write it and gets shm buffers. */
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (rt && *rt) {
		char p[400];
		snprintf(p, sizeof p, "%s/flcomp-dmabuf-planes", rt);
		int nf = open(p, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
		if (nf >= 0)
			close(nf);
	}
	return;
fail:
	gl_stop();
}

bool gl_usable(void) { return G.ok; }

/* Modifiers to announce for a format. Linear always works (mmap); with a GPU
 * path every modifier it can sample, and the implicit one. */
int gl_modifiers(uint32_t format, uint64_t *mods, int max)
{
	int n = 0;
	mods[n++] = DRM_FORMAT_MOD_LINEAR;
	if (!G.ok)
		return n;
	if (!G.have_mods) {
		mods[n++] = DRM_FORMAT_MOD_INVALID;
		return n;
	}
	EGLuint64KHR m[64];
	EGLBoolean ext[64];
	EGLint cnt = 0;
	if (!G.query_mods(G.dpy, (EGLint)format, 64, m, ext, &cnt))
		return n;
	for (int i = 0; i < cnt && n < max; i++) {
		if (ext[i] || m[i] == DRM_FORMAT_MOD_LINEAR || m[i] == DRM_FORMAT_MOD_INVALID)
			continue;
		mods[n++] = m[i];
	}
	if (n < max)
		mods[n++] = DRM_FORMAT_MOD_INVALID;
	return n;
}

bool gl_modifier_ok(uint32_t format, uint64_t modifier, int nplanes)
{
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return nplanes == 1;
	if (!G.ok)
		return modifier == DRM_FORMAT_MOD_INVALID && nplanes == 1;	/* treated as linear */
	if (modifier == DRM_FORMAT_MOD_INVALID)
		return nplanes == 1;
	/* A client may use any modifier the GPU can sample, announced by us or not (a browser
	 * gets its list from its own EGL display): ask the driver. */
	if (G.have_mods) {
		EGLuint64KHR m[128];
		EGLBoolean ext[128];
		EGLint cnt = 0;
		if (G.query_mods(G.dpy, (EGLint)format, 128, m, ext, &cnt))
			for (int i = 0; i < cnt && i < 128; i++)
				if (m[i] == modifier && !ext[i])
					return gbm_device_get_format_modifier_plane_count(G.gbm, format, modifier) == nplanes;
	}
	return false;
}

bool gl_read(struct dbuf *b, uint32_t *dst, struct rect dmg)
{
	if (!G.ok)
		return false;
	int w = b->w, h = b->h;
	/* A texture as the render target: RGBA8 is guaranteed for textures, not for
	 * renderbuffers on GLES2. */
	static GLuint rt;
	static int rtw, rth;
	if (!rt)
		glGenTextures(1, &rt);
	if (w > rtw || h > rth) {
		rtw = w > rtw ? w : rtw;
		rth = h > rth ? h : rth;
		glBindTexture(GL_TEXTURE_2D, rt);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rtw, rth, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return false;

	EGLint at[48];
	int i = 0;
	at[i++] = EGL_WIDTH, at[i++] = w;
	at[i++] = EGL_HEIGHT, at[i++] = h;
	at[i++] = EGL_LINUX_DRM_FOURCC_EXT, at[i++] = (EGLint)b->format;
	at[i++] = EGL_DMA_BUF_PLANE0_FD_EXT, at[i++] = b->fd;
	at[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT, at[i++] = (EGLint)b->offset;
	at[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT, at[i++] = (EGLint)b->stride;
	if (G.have_mods && b->modifier != DRM_FORMAT_MOD_INVALID) {
		at[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, at[i++] = (EGLint)(b->modifier & 0xffffffffu);
		at[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, at[i++] = (EGLint)(b->modifier >> 32);
	}
	static const EGLint pl_attr[3][5] = {
		{ EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		  EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
		  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		  EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
	};
	for (int pl = 0; pl < b->nplanes - 1 && pl < 3; pl++) {
		at[i++] = pl_attr[pl][0], at[i++] = b->extra[pl].fd;
		at[i++] = pl_attr[pl][1], at[i++] = (EGLint)b->extra[pl].offset;
		at[i++] = pl_attr[pl][2], at[i++] = (EGLint)b->extra[pl].stride;
		if (G.have_mods && b->modifier != DRM_FORMAT_MOD_INVALID) {
			at[i++] = pl_attr[pl][3], at[i++] = (EGLint)(b->modifier & 0xffffffffu);
			at[i++] = pl_attr[pl][4], at[i++] = (EGLint)(b->modifier >> 32);
		}
	}
	at[i++] = EGL_NONE;
	EGLImageKHR img = G.create_image(G.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
	if (img == EGL_NO_IMAGE_KHR)
		return false;
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, G.tex);
	G.image_target(GL_TEXTURE_2D, (GLeglImageOES)img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glViewport(0, 0, w, h);
	glDisable(GL_BLEND);
	glUseProgram(G.prog);
	glUniform1f(G.u_opaque, dbuf_has_alpha(b) ? 0.0f : 1.0f);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	int rw = dmg.x2 - dmg.x1, rh = dmg.y2 - dmg.y1;
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	if (rw == w) {
		glReadPixels(0, dmg.y1, w, rh, GL_RGBA, GL_UNSIGNED_BYTE, dst + (size_t)dmg.y1 * w);
	} else {
		uint32_t *tmp = malloc((size_t)rw * rh * 4);
		if (tmp) {
			glReadPixels(dmg.x1, dmg.y1, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
			for (int y = 0; y < rh; y++)
				memcpy(dst + (size_t)(dmg.y1 + y) * w + dmg.x1, tmp + (size_t)y * rw,
				       (size_t)rw * 4);
			free(tmp);
		}
	}
	GLenum err = glGetError();
	glBindTexture(GL_TEXTURE_2D, G.tex);
	G.destroy_image(G.dpy, img);
	return err == GL_NO_ERROR;
}
