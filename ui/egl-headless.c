#include "qemu/osdep.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#include "ui/shader.h"
#include <epoxy/egl_generated.h>

typedef struct egl_dpy {
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    QemuGLShader *gls;
    egl_fb guest_fb;
    egl_fb cursor_fb;
    egl_fb blit_fb;
    bool y_0_top;
    uint32_t pos_x;
    uint32_t pos_y;
} egl_dpy;

/* ------------------------------------------------------------------ */

static EGLContext egl_headless2_rn_ctx;

static void egl_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static void egl_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
}

static void egl_gfx_switch(DisplayChangeListener *dcl,
                           struct DisplaySurface *new_surface)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->ds = new_surface;
}

static QEMUGLContext egl_create_context(DisplayChangeListener *dcl,
                                        QEMUGLParams *params)
{
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   egl_headless2_rn_ctx);
    return qemu_egl_create_context(dcl, params);
}

static void egl_scanout_disable(DisplayChangeListener *dcl)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    egl_fb_destroy(&edpy->guest_fb);
    egl_fb_destroy(&edpy->blit_fb);
}

static void egl_scanout_texture(DisplayChangeListener *dcl,
                                uint32_t backing_id,
                                bool backing_y_0_top,
                                uint32_t backing_width,
                                uint32_t backing_height,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->y_0_top = backing_y_0_top;

    /* source framebuffer */
    egl_fb_setup_for_tex(&edpy->guest_fb,
                         backing_width, backing_height, backing_id, false);

    /* dest framebuffer */
    if (edpy->blit_fb.width  != backing_width ||
        edpy->blit_fb.height != backing_height) {
        egl_fb_destroy(&edpy->blit_fb);
        egl_fb_setup_new_tex(&edpy->blit_fb, backing_width, backing_height);
    }
}

static void egl_cursor_position(DisplayChangeListener *dcl,
                                uint32_t pos_x, uint32_t pos_y)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->pos_x = pos_x;
    edpy->pos_y = pos_y;
}

static void egl_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    if (!edpy->guest_fb.texture || !edpy->ds) {
        return;
    }
    assert(surface_width(edpy->ds)  == edpy->guest_fb.width);
    assert(surface_height(edpy->ds) == edpy->guest_fb.height);
    assert(surface_format(edpy->ds) == PIXMAN_x8r8g8b8);

    if (edpy->cursor_fb.texture) {
        /* have cursor -> render using textures */
        egl_texture_blit(edpy->gls, &edpy->blit_fb, &edpy->guest_fb,
                         !edpy->y_0_top);
        egl_texture_blend(edpy->gls, &edpy->blit_fb, &edpy->cursor_fb,
                          !edpy->y_0_top, edpy->pos_x, edpy->pos_y);
    } else {
        /* no cursor -> use simple framebuffer blit */
        egl_fb_blit(&edpy->blit_fb, &edpy->guest_fb, edpy->y_0_top);
    }

    egl_fb_read(surface_data(edpy->ds), &edpy->blit_fb);
    dpy_gfx_update(edpy->dcl.con, x, y, w, h);
}

static const DisplayChangeListenerOps egl_ops = {
    .dpy_name                = "egl-headless",
    .dpy_refresh             = egl_refresh,
    .dpy_gfx_update          = egl_gfx_update,
    .dpy_gfx_switch          = egl_gfx_switch,

    .dpy_gl_ctx_create       = egl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
    .dpy_gl_ctx_get_current  = qemu_egl_get_current_context,

    .dpy_gl_scanout_disable  = egl_scanout_disable,
    .dpy_gl_scanout_texture  = egl_scanout_texture,
    .dpy_gl_cursor_position  = egl_cursor_position,
    .dpy_gl_update           = egl_scanout_flush,
};

static void early_egl_headless_init(DisplayOptions *opts)
{
    display_opengl = 1;
}

static bool egl_headless2_can_use_enumeration(void)
{
    return epoxy_has_egl_extension(NULL, "EGL_EXT_platform_base") &&
           epoxy_has_egl_extension(NULL, "EGL_EXT_device_enumeration");
}

static int egl_headless2_surfaceless_init(DisplayOptions *opts)
{
    static const EGLint conf_att_core[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    static const EGLint conf_att_gles[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_NONE
    };
    DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAYGL_MODE_ON;
    bool gles = (mode == DISPLAYGL_MODE_ES);
    int major, minor;
    EGLBoolean b;
    EGLint n;

    if (egl_headless2_can_use_enumeration()) {
        EGLDeviceEXT *devices;
        EGLint devcnt;
        int index = 0;

        b = eglQueryDevicesEXT(0, NULL, &devcnt);
        if (b == EGL_FALSE) {
            error_report("egl: eglQueryDevicesEXT failed (Error: %d)", eglGetError());
            return -1;
        }
        if (!devcnt) {
            error_report("egl: no devices exist");
            return -1;
        }

        devices = g_malloc(sizeof(EGLDeviceEXT) * devcnt);
        b = eglQueryDevicesEXT(devcnt, devices, &devcnt);
        if (b == EGL_FALSE) {
            error_report("egl: eglQueryDevicesEXT enumeration failed (Error: %d)", eglGetError());
            g_free(devices);
            return -1;
        }

        info_report("egl: number of devices is %d", devcnt);
        for (n = 0; n < devcnt; n++) {
            const char *rn;
            const char *dexts = eglQueryDeviceStringEXT(devices[n], EGL_EXTENSIONS);

            if (dexts == NULL || strstr(dexts, "EGL_EXT_device_drm") == NULL)
                continue;

            rn = eglQueryDeviceStringEXT(devices[n], EGL_DRM_DEVICE_FILE_EXT);
            info_report("egl: device %d - %s", n, rn ? rn : "NULL");
        }

        if (opts->u.egl_headless.has_rendernode) {
            const char *reqrn = opts->u.egl_headless.rendernode;

            for (n = 0; n < devcnt; n++) {
                const char *rn;
                const char *dexts = eglQueryDeviceStringEXT(devices[n], EGL_EXTENSIONS);

                if (dexts == NULL || strstr(dexts, "EGL_EXT_device_drm") == NULL)
                    continue;

                rn = eglQueryDeviceStringEXT(devices[n], EGL_DRM_DEVICE_FILE_EXT);
                if (rn && !g_strcmp0(rn, reqrn)) {
                    index = n;
                    break;
                }
            }
        }

        info_report("egl: device %d chosen", index);
        qemu_egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devices[index], NULL);
        if (qemu_egl_display == EGL_NO_DISPLAY) {
            error_report("egl: eglGetPlatformDisplayEXT failed (Error: %d)", eglGetError());
            g_free(devices);
            return -1;
        }

        g_free(devices);
    } else {
        qemu_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (qemu_egl_display == EGL_NO_DISPLAY) {
            error_report("egl: eglGetDisplay failed (Error: %d)", eglGetError());
            return -1;
        }
    }

    b = eglInitialize(qemu_egl_display, &major, &minor);
    if (b == EGL_FALSE) {
        error_report("egl: eglInitialize failed (Error: %d)", eglGetError());
        return -1;
    }

    b = eglBindAPI(gles ?  EGL_OPENGL_ES_API : EGL_OPENGL_API);
    if (b == EGL_FALSE) {
      error_report("egl: eglBindAPI failed (%s mode)", gles ? "gles" : "core");
      return -1;
    }

    b = eglChooseConfig(qemu_egl_display, gles ? conf_att_gles : conf_att_core,
                        &qemu_egl_config, 1, &n);
    if (b == EGL_FALSE || n != 1) {
      error_report("egl: eglChooseConfig failed (%s mode)",
                   gles ? "gles" : "core");
      return -1;
    }

    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_KHR_surfaceless_context")) {
        error_report("egl: EGL_KHR_surfaceless_context not supported");
        return -1;
    }

    qemu_egl_mode = gles ? DISPLAYGL_MODE_ES : DISPLAYGL_MODE_CORE;

    egl_headless2_rn_ctx = qemu_egl_init_ctx();
    if (!egl_headless2_rn_ctx) {
        error_report("egl: egl_init_ctx failed. Error code: %d", eglGetError());
        return -1;
    }

    return 0;
}

static void egl_headless_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    egl_dpy *edpy;
    int idx;

    if (egl_headless2_surfaceless_init(opts) < 0) {
        error_report("egl: render node init failed");
        exit(1);
    }

    for (idx = 0;; idx++) {
        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        edpy = g_new0(egl_dpy, 1);
        edpy->dcl.con = con;
        edpy->dcl.ops = &egl_ops;
        edpy->gls = qemu_gl_init_shader();
        register_displaychangelistener(&edpy->dcl);
    }
}

static QemuDisplay qemu_display_egl = {
    .type       = DISPLAY_TYPE_EGL_HEADLESS,
    .early_init = early_egl_headless_init,
    .init       = egl_headless_init,
};

static void register_egl(void)
{
    qemu_display_register(&qemu_display_egl);
}

type_init(register_egl);
