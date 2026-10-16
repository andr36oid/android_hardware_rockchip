/*
 * Copyright (C) 2026 The andr36oid Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "hwc-crt"

#include "crt_mask.h"

#include "drmcrtc.h"
#include "drmhwcomposer.h"
#include "drmplane.h"
#include "drmresources.h"

#include <atomic>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <cutils/properties.h>
#include <drm/drm_fourcc.h>
#include <log/log.h>
#include <xf86drm.h>

namespace android {

static std::atomic<bool> g_crt_failed(false);

static int clamp_int(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

CrtMaskConfig crt_mask_read_config() {
  CrtMaskConfig cfg;
  if (g_crt_failed.load())
    return cfg;
  if (property_get_int32("persist.andr36oid.crt.enable", 0) != 1)
    return cfg;
  /* the GPU effects can be used without the lines */
  if (property_get_int32("persist.andr36oid.crt.lines", 1) != 1)
    return cfg;
  cfg.strength = clamp_int(property_get_int32("persist.andr36oid.crt.strength", 50), 0, 100);
  if (cfg.strength == 0)
    return cfg;
  cfg.style = clamp_int(property_get_int32("persist.andr36oid.crt.style", 0), 0,
                        CRT_STYLE_COUNT - 1);
  cfg.pitch = clamp_int(property_get_int32("persist.andr36oid.crt.pitch", 0), 0, 8);
  /* SurfaceFlinger's coloured phosphor mask is on: it replaces our grille, a second one
     on top would only darken the picture; the scanlines stay */
  if (property_get_int32("persist.andr36oid.crt.gpu_mask", 0) > 0) {
    if (cfg.style == CRT_STYLE_GRILLE)
      return cfg;
    cfg.style = CRT_STYLE_SCANLINES;
  }
  cfg.phase = clamp_int(property_get_int32("persist.andr36oid.crt.phase", 0), 0, 7);

  char orient[PROPERTY_VALUE_MAX];
  property_get("ro.surface_flinger.primary_display_orientation", orient, "");
  cfg.rotated = !strcmp(orient, "ORIENTATION_90") || !strcmp(orient, "ORIENTATION_270");
  cfg.on = true;
  return cfg;
}

void crt_mask_set_failed() {
  if (!g_crt_failed.exchange(true))
    ALOGE("The display controller refused the CRT effect, it stays off until restart");
}

/* How dark (0-1) a scanline row is; the line is the last row of each pitch. */
static float scan_dark(int y, int pitch) {
  int r = y % pitch;
  if (r == pitch - 1)
    return 1.0f;
  /* wide pitches: soften the rows next to the line, like a round beam */
  if (pitch >= 4 && (r == 0 || r == pitch - 2))
    return 0.35f;
  return 0.0f;
}

/* The gap between phosphor stripes: every third column, a bit lighter than a scanline. */
static float grille_dark(int x) {
  return (x % 3 == 2) ? 0.6f : 0.0f;
}

static float mask_dark(const CrtMaskConfig &cfg, int vx, int vy, int pitch) {
  float s = 0.0f, g = 0.0f;
  int y = vy + cfg.phase;
  switch (cfg.style) {
    case CRT_STYLE_SCANLINES:
      s = scan_dark(y, pitch);
      break;
    case CRT_STYLE_SCANLINES_GRILLE:
      s = scan_dark(y, pitch);
      g = grille_dark(vx);
      break;
    case CRT_STYLE_SLOT: {
      /* 3 pixel wide slots, every other column of slots moved down half a line */
      int shift = ((vx / 3) & 1) ? (pitch + 1) / 2 : 0;
      s = scan_dark(y + shift, pitch);
      g = grille_dark(vx);
      break;
    }
    case CRT_STYLE_GRILLE:
      g = grille_dark(vx);
      break;
  }
  /* both darken: light through = (1 - s)(1 - g) */
  return 1.0f - (1.0f - s) * (1.0f - g);
}

CrtMask::CrtMask(DrmResources *drm) : drm_(drm) {
}

CrtMask::~CrtMask() {
  Free(&old_);
  Free(&cur_);
}

void CrtMask::Free(Buffer *b) {
  if (b->fb_id)
    drmModeRmFB(drm_->fd(), b->fb_id);
  if (b->handle) {
    struct drm_mode_destroy_dumb d;
    memset(&d, 0, sizeof(d));
    d.handle = b->handle;
    drmIoctl(drm_->fd(), DRM_IOCTL_MODE_DESTROY_DUMB, &d);
  }
  *b = Buffer();
}

int CrtMask::Build(const CrtMaskConfig &cfg, uint32_t w, uint32_t h, Buffer *out) {
  int fd = drm_->fd();
  struct drm_mode_create_dumb create;
  memset(&create, 0, sizeof(create));
  create.width = w;
  create.height = h;
  create.bpp = 32;
  int ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
  if (ret) {
    ALOGE("Can't get a %ux%u buffer for the CRT effect: %d", w, h, -errno);
    return -errno;
  }
  out->handle = create.handle;
  out->w = w;
  out->h = h;
  out->cfg = cfg;

  struct drm_mode_map_dumb map;
  memset(&map, 0, sizeof(map));
  map.handle = create.handle;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
  if (ret) {
    ret = -errno;
    ALOGE("Can't map the CRT effect buffer: %d", ret);
    Free(out);
    return ret;
  }
  uint8_t *base = (uint8_t *)mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                  fd, map.offset);
  if (base == MAP_FAILED) {
    ret = -errno;
    ALOGE("Can't map the CRT effect buffer: %d", ret);
    Free(out);
    return ret;
  }

  /* lines run across what the player sees, so a turned panel gets them along y */
  uint32_t view_h = cfg.rotated ? w : h;
  int pitch = cfg.pitch;
  if (pitch < 2)
    pitch = clamp_int((int)((view_h + 120) / 240), 2, 8);
  float strength = cfg.strength / 100.0f;

  for (uint32_t py = 0; py < h; py++) {
    uint32_t *row = (uint32_t *)(base + (size_t)py * create.pitch);
    for (uint32_t px = 0; px < w; px++) {
      int vx = cfg.rotated ? py : px;
      int vy = cfg.rotated ? px : py;
      int a = (int)(mask_dark(cfg, vx, vy, pitch) * strength * 255.0f + 0.5f);
      /* ARGB8888, black, premultiplied: only alpha is set */
      row[px] = (uint32_t)clamp_int(a, 0, 255) << 24;
    }
  }
  munmap(base, create.size);

  uint32_t handles[4] = {create.handle, 0, 0, 0};
  uint32_t pitches[4] = {create.pitch, 0, 0, 0};
  uint32_t offsets[4] = {0, 0, 0, 0};
  ret = drmModeAddFB2(fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                      &out->fb_id, 0);
  if (ret) {
    ALOGE("Can't make a framebuffer for the CRT effect: %d", ret);
    Free(out);
    return ret;
  }
  ALOGI("CRT effect: style %d, strength %d, %d rows per line, %ux%u%s", cfg.style,
        cfg.strength, pitch, w, h, cfg.rotated ? ", turned panel" : "");
  return 0;
}

int CrtMask::AddToPset(drmModeAtomicReqPtr pset, DrmCrtc *crtc, DrmPlane *plane,
                       const CrtMaskConfig &cfg, uint32_t w, uint32_t h) {
  if (!w || !h)
    return -EINVAL;
  if (!cur_.fb_id || cur_.w != w || cur_.h != h || !cur_.cfg.same_look(cfg)) {
    /* a change before the last one reached the screen: that one is not shown */
    Free(&old_);
    old_ = cur_;
    cur_ = Buffer();
    int ret = Build(cfg, w, h, &cur_);
    if (ret)
      return ret;
  }

  uint32_t id = plane->id();
  int ret = drmModeAtomicAddProperty(pset, id, plane->crtc_property().id(), crtc->id()) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->fb_property().id(), cur_.fb_id) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->crtc_x_property().id(), 0) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->crtc_y_property().id(), 0) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->crtc_w_property().id(), w) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->crtc_h_property().id(), h) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->src_x_property().id(), 0) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->src_y_property().id(), 0) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->src_w_property().id(),
                                  (uint64_t)w << 16) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->src_h_property().id(),
                                  (uint64_t)h << 16) < 0;
  ret |= drmModeAtomicAddProperty(pset, id, plane->zpos_property().id(), cfg.zpos) < 0;
  /* a layer that had this window before may have left these behind */
#if DRM_DRIVER_VERSION == 2
  if (plane->rotation_property().id())
    ret |= drmModeAtomicAddProperty(pset, id, plane->rotation_property().id(),
                                    kRotate0) < 0;
#endif
  if (plane->alpha_property().id())
    ret |= drmModeAtomicAddProperty(pset, id, plane->alpha_property().id(), 0xff) < 0;
  if (plane->blend_mode_property().id())
    ret |= drmModeAtomicAddProperty(pset, id, plane->blend_mode_property().id(), 1) < 0;
  if (ret) {
    ALOGE("Failed to add the CRT effect to plane %u", id);
    return -EINVAL;
  }
  return 0;
}

void CrtMask::Committed(bool mask_shown) {
  /* the commit waited for the new frame, so the old picture is off the screen */
  Free(&old_);
  if (!mask_shown)
    Free(&cur_);
}

}  // namespace android
