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

#ifndef ANDROID_CRT_MASK_H_
#define ANDROID_CRT_MASK_H_

#include <stdint.h>
#include <xf86drmMode.h>

/*
 * CRT look without the GPU: a still picture of black lines with per-pixel alpha
 * (scanlines, aperture grille or slot mask) sits on the display controller's top
 * window, and the VOP blends it over the screen while it scans out. Nothing is drawn
 * per frame; the picture is built once when a setting changes.
 *
 * The top window (win2 on the RK3326 VOPB, the "cursor" plane and its 3 areas) is
 * taken away from apps while the effect is on, so apps keep win0 and win1. A frame
 * with more layers than that goes through the GPU as it already does today.
 *
 * Settings (Settings > Display > CRT effect writes them):
 *   persist.andr36oid.crt.enable    1 = on
 *   persist.andr36oid.crt.style     0 scanlines, 1 scanlines + grille, 2 slot mask,
 *                                   3 aperture grille only
 *   persist.andr36oid.crt.strength  0-100, how dark the lines get
 *   persist.andr36oid.crt.pitch     screen rows per scanline, 0 = auto (height / 240)
 *   persist.andr36oid.crt.phase     move the lines down by this many rows
 */

namespace android {

class DrmResources;
class DrmCrtc;
class DrmPlane;

enum {
  CRT_STYLE_SCANLINES = 0,
  CRT_STYLE_SCANLINES_GRILLE = 1,
  CRT_STYLE_SLOT = 2,
  CRT_STYLE_GRILLE = 3,
  CRT_STYLE_COUNT
};

struct CrtMaskConfig {
  bool on = false;
  int style = CRT_STYLE_SCANLINES;
  int strength = 0;
  int pitch = 0;
  int phase = 0;
  /* the panel is turned by SurfaceFlinger (R50S, D-28S): lines go along the other axis */
  bool rotated = false;
  /* zpos of the reserved window, the top one */
  int zpos = 0;

  bool same_look(const CrtMaskConfig &o) const {
    return style == o.style && strength == o.strength && pitch == o.pitch &&
           phase == o.phase && rotated == o.rotated;
  }
};

/* Reads the settings; on is false if the effect is off, at 0 strength or failed before. */
CrtMaskConfig crt_mask_read_config();

/* The VOP refused a frame with the mask: keep it off until the next start. */
void crt_mask_set_failed();

class CrtMask {
 public:
  explicit CrtMask(DrmResources *drm);
  ~CrtMask();

  /* Puts the mask on plane, full screen w x h. 0 if it was added. */
  int AddToPset(drmModeAtomicReqPtr pset, DrmCrtc *crtc, DrmPlane *plane,
                const CrtMaskConfig &cfg, uint32_t w, uint32_t h);

  /* After a real (not test) commit: frees pictures no longer on screen. */
  void Committed(bool mask_shown);

 private:
  struct Buffer {
    uint32_t handle = 0;
    uint32_t fb_id = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    CrtMaskConfig cfg;
  };

  int Build(const CrtMaskConfig &cfg, uint32_t w, uint32_t h, Buffer *out);
  void Free(Buffer *b);

  DrmResources *drm_;
  Buffer cur_;
  /* the picture before a change, still on screen until the next commit is done */
  Buffer old_;
};

}  // namespace android

#endif  // ANDROID_CRT_MASK_H_
