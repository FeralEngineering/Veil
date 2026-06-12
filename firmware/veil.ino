#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include "FS.h"
#include "SD_MMC.h"
#include "img_converters.h"   // fmt2jpg
#include <math.h>
#include <Preferences.h>

// =====================
// AP Settings
// =====================
const char* AP_SSID = "VeilCam";
const char* AP_PASS = "veilveilveil";

// =====================
// Web Server
// =====================
WebServer server(80);

// =====================
// New Global
// =====================
Preferences prefs;
static uint32_t g_photoIndex = 0;
// =====================
// GPIO (NO WIRING CHANGES)
// =====================
// IMPORTANT: GPIO4 is tied to the ESP32-CAM flash circuit.
// Driving it during camera capture can cause fb_get() to return null / DMA failures.
// So: NEVER drive GPIO4 while capturing.
static const int LED_PIN = 4;
static const int BTN_PIN = 13;

// LED policy: keep it hi-z always during capture.
// After capture, can optionally pulse very briefly (safe).
static inline void ledHiZ() {
  pinMode(LED_PIN, INPUT);
}

static inline void ledPulseIdle(uint16_t ms = 40) {
  // Only call when NOT capturing.
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);     // sink = on
  delay(ms);
  ledHiZ();
}

// =====================
// Camera (AI-Thinker ESP32-CAM) Pin Map
// =====================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// =====================
// Stability settings (DO NOT GO HUGE)
// =====================
// If you push VGA + full RGB processing you WILL hit memory fragmentation fast.
// QVGA gives you reliable "it works every time" behavior while still looking decent.
static framesize_t kFrameSize = FRAMESIZE_QVGA; // 320x240
static int kJpegQuality = 12;                   // 10-14 (lower = better quality)

// =====================
// Veil Look Tuning 
// =====================
static const float PINK_WEIGHT    = 0.52f;
static const float HAZE_STRENGTH  = 0.44f;
static const int   GLOW_THRESHOLD = 172;
static const float GLOW_GAIN      = 0.90f;

static const float WARP_STRENGTH_PX = 0.40f;
static const int   WARP_PERIOD      = 48;

static const int   SOFT_BLUR_PASSES = 1;
static const float SOFT_BLEND       = 0.58f;

static const float VIGNETTE_STRENGTH = 0.26f;

static const float MIST_VARIATION = 0.45f;
static const int   MIST_SCALE_PX  = 130;

static const int   CHROMA_HI_THRESH = 18;
static const float CHROMA_SHIFT_PX  = 0.55f;
static const float CHROMA_AMOUNT    = 0.70f;

static const float DEPTH_LIE_STRENGTH = 0.45f;
static const float SHADOW_EMISSION    = 0.24f;
static const float RIFT_SHEAR_PX      = 0.55f;
static const int   RIFT_BAND_PX       = 160;

static const float SWIRL_STRENGTH = 0.30f;
static const float SWIRL_RADIUS   = 1.05f;

static const float DRIFT_PX       = 0.85f;
static const int   DRIFT_SCALE_PX = 220;

// =====================
// State + concurrency
// =====================
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_busy = false;
static volatile bool g_captureRequested = false;
static String g_status = "Booting...";
static bool g_cameraOK = false;
static bool g_sdOK = false;

static TaskHandle_t g_capTask = nullptr;

// =====================
// Helpers: no-cache headers
// =====================
static inline void sendNoCache() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

// =====================
// SD Helpers
// =====================
static bool initSD() {
  if (!SD_MMC.begin("/sdcard", true)) return false; // 1-bit mode
  return SD_MMC.cardType() != CARD_NONE;
}

static String makeFilename() {
  // sequential index stored in NVS (survives reboot)
  g_photoIndex++;
  prefs.putUInt("idx", g_photoIndex);

  char name[32];
  snprintf(name, sizeof(name), "/veil_%08lu.jpg", (unsigned long)g_photoIndex);
  return String(name);
}

// =====================
// Clamp / luma
// =====================
static inline uint8_t clamp8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

static inline uint8_t luma8(uint8_t r, uint8_t g, uint8_t b) {
  return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

// =====================
// Mist noise
// =====================
static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static inline uint8_t noise8_grid(int gx, int gy, uint32_t seed) {
  uint32_t v = (uint32_t)gx * 374761393U ^ (uint32_t)gy * 668265263U ^ seed;
  return (uint8_t)(hash32(v) & 0xFF);
}

static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
  return (uint8_t)(((uint16_t)(255 - t) * a + (uint16_t)t * b) >> 8);
}

static inline uint8_t smoothstep8(uint8_t t) {
  uint32_t tt = (uint32_t)t * (uint32_t)t;
  uint32_t a = 3U * 255U;
  uint32_t s = (tt * (a - 2U * t)) / (255U * 255U);
  return (uint8_t)s;
}

static inline float mistAt(int x, int y, uint32_t seed) {
  int scale = (MIST_SCALE_PX < 8) ? 8 : MIST_SCALE_PX;
  int gx = x / scale;
  int gy = y / scale;
  int fx = x - gx * scale;
  int fy = y - gy * scale;

  uint8_t tx = (uint8_t)((fx * 255) / (scale - 1));
  uint8_t ty = (uint8_t)((fy * 255) / (scale - 1));
  tx = smoothstep8(tx);
  ty = smoothstep8(ty);

  uint8_t v00 = noise8_grid(gx,     gy,     seed);
  uint8_t v10 = noise8_grid(gx + 1, gy,     seed);
  uint8_t v01 = noise8_grid(gx,     gy + 1, seed);
  uint8_t v11 = noise8_grid(gx + 1, gy + 1, seed);

  uint8_t vx0 = lerp8(v00, v10, tx);
  uint8_t vx1 = lerp8(v01, v11, tx);
  uint8_t vxy = lerp8(vx0, vx1, ty);

  return (float)vxy / 255.0f;
}

// =====================
// RGB565 -> RGB888
// =====================
static inline void rgb565_bytes_to_rgb888(uint8_t hi, uint8_t lo, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint16_t p = ((uint16_t)hi << 8) | lo;
  uint8_t rr = (p >> 11) & 0x1F;
  uint8_t gg = (p >>  5) & 0x3F;
  uint8_t bb = (p      ) & 0x1F;
  r = (uint8_t)(rr * 255 / 31);
  g = (uint8_t)(gg * 255 / 63);
  b = (uint8_t)(bb * 255 / 31);
}

static bool rgb565_to_rgb888(const uint8_t* src, int w, int h, uint8_t* dst) {
  size_t n = (size_t)w * (size_t)h;
  for (size_t i = 0; i < n; i++) {
    uint8_t hi = src[i * 2 + 0];
    uint8_t lo = src[i * 2 + 1];
    uint8_t r,g,b;
    rgb565_bytes_to_rgb888(hi, lo, r, g, b);
    dst[i * 3 + 0] = r;
    dst[i * 3 + 1] = g;
    dst[i * 3 + 2] = b;
  }
  return true;
}

// =====================
// Blur helpers
// =====================
static void blur_rgb888_box_with_tmp(uint8_t* rgb, uint8_t* tmp, int w, int h) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int xm1 = (x == 0) ? 0 : x - 1;
      int xp1 = (x == w - 1) ? (w - 1) : x + 1;

      size_t i0 = ((size_t)y * w + xm1) * 3;
      size_t i1 = ((size_t)y * w + x)   * 3;
      size_t i2 = ((size_t)y * w + xp1) * 3;
      size_t o  = ((size_t)y * w + x) * 3;

      tmp[o+0] = (uint8_t)((rgb[i0+0] + rgb[i1+0] + rgb[i2+0]) / 3);
      tmp[o+1] = (uint8_t)((rgb[i0+1] + rgb[i1+1] + rgb[i2+1]) / 3);
      tmp[o+2] = (uint8_t)((rgb[i0+2] + rgb[i1+2] + rgb[i2+2]) / 3);
    }
  }

  for (int y = 0; y < h; y++) {
    int ym1 = (y == 0) ? 0 : y - 1;
    int yp1 = (y == h - 1) ? (h - 1) : y + 1;
    for (int x = 0; x < w; x++) {
      size_t i0 = ((size_t)ym1 * w + x) * 3;
      size_t i1 = ((size_t)y   * w + x) * 3;
      size_t i2 = ((size_t)yp1 * w + x) * 3;
      size_t o  = ((size_t)y   * w + x) * 3;

      rgb[o+0] = (uint8_t)((tmp[i0+0] + tmp[i1+0] + tmp[i2+0]) / 3);
      rgb[o+1] = (uint8_t)((tmp[i0+1] + tmp[i1+1] + tmp[i2+1]) / 3);
      rgb[o+2] = (uint8_t)((tmp[i0+2] + tmp[i1+2] + tmp[i2+2]) / 3);
    }
  }
}

static void blend_rgb888(uint8_t* base, const uint8_t* over, int w, int h, float alpha) {
  int a = (int)(alpha * 256.0f);
  if (a < 0) a = 0;
  if (a > 256) a = 256;

  size_t n = (size_t)w * (size_t)h * 3;
  for (size_t i = 0; i < n; i++) {
    int v = ((256 - a) * base[i] + a * over[i]) >> 8;
    base[i] = (uint8_t)v;
  }
}

static void blur_u8(uint8_t* img, int w, int h) {
  uint8_t* tmp = (uint8_t*)ps_malloc((size_t)w * h);
  if (!tmp) tmp = (uint8_t*)malloc((size_t)w * h);
  if (!tmp) return;

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int a = img[y*w + ((x==0)?0:x-1)];
      int b = img[y*w + x];
      int c = img[y*w + ((x==w-1)?w-1:x+1)];
      tmp[y*w + x] = (uint8_t)((a + b + c) / 3);
    }
  }
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int a = tmp[((y==0)?0:y-1)*w + x];
      int b = tmp[y*w + x];
      int c = tmp[((y==h-1)?h-1:y+1)*w + x];
      img[y*w + x] = (uint8_t)((a + b + c) / 3);
    }
  }
  free(tmp);
}

// =====================
// Bilinear sample
// =====================
static inline void sampleRGBBilinear(
  const uint8_t* src, int w, int h, float sx, float sy,
  uint8_t &or_, uint8_t &og, uint8_t &ob
) {
  int x0 = (int)sx;
  int y0 = (int)sy;
  float fx = sx - x0;
  float fy = sy - y0;

  if (x0 < 0) { x0 = 0; fx = 0; }
  if (y0 < 0) { y0 = 0; fy = 0; }
  if (x0 >= w - 1) { x0 = w - 2; fx = 1; }
  if (y0 >= h - 1) { y0 = h - 2; fy = 1; }

  size_t i00 = ((size_t)y0 * w + x0) * 3;
  size_t i10 = i00 + 3;
  size_t i01 = i00 + (size_t)w * 3;
  size_t i11 = i01 + 3;

  for (int c = 0; c < 3; c++) {
    float v00 = src[i00 + c], v10 = src[i10 + c];
    float v01 = src[i01 + c], v11 = src[i11 + c];
    float v0 = v00 + fx * (v10 - v00);
    float v1 = v01 + fx * (v11 - v01);
    float v  = v0  + fy * (v1  - v0);
    uint8_t out = (uint8_t)v;
    if (c == 0) or_ = out;
    else if (c == 1) og = out;
    else ob = out;
  }
}

// =====================
// Warp / rift / swirl / drift / chroma / veil grade
// =====================
static void smoothWarpInPlace(uint8_t* rgb, int w, int h, uint8_t* scratch) {
  if (WARP_STRENGTH_PX <= 0.001f) return;
  size_t len = (size_t)w * h * 3;
  memcpy(scratch, rgb, len);

  int phase = (int)(esp_random() & 0xFFFF);

  for (int y = 0; y < h; y++) {
    int t = (y * 256 / WARP_PERIOD + phase) & 255;
    int tri = (t < 128) ? t : (255 - t);
    float wave = (tri / 127.0f);
    float shift = (wave - 0.5f) * 2.0f * WARP_STRENGTH_PX;

    int s0 = (int)shift;
    float frac = shift - s0;

    for (int x = 0; x < w; x++) {
      int x0 = x + s0;
      int x1 = x0 + 1;

      if (x0 < 0) x0 = 0;
      if (x0 >= w) x0 = w - 1;
      if (x1 < 0) x1 = 0;
      if (x1 >= w) x1 = w - 1;

      size_t di = ((size_t)y * w + x) * 3;
      size_t i0 = ((size_t)y * w + x0) * 3;
      size_t i1 = ((size_t)y * w + x1) * 3;

      rgb[di+0] = (uint8_t)((1.0f - frac) * scratch[i0+0] + frac * scratch[i1+0]);
      rgb[di+1] = (uint8_t)((1.0f - frac) * scratch[i0+1] + frac * scratch[i1+1]);
      rgb[di+2] = (uint8_t)((1.0f - frac) * scratch[i0+2] + frac * scratch[i1+2]);
    }
  }
}

static void riftShearInPlace(uint8_t* rgb, int w, int h, uint8_t* scratch, uint32_t seed) {
  if (RIFT_SHEAR_PX <= 0.001f) return;
  size_t len = (size_t)w * h * 3;
  memcpy(scratch, rgb, len);

  int band = (RIFT_BAND_PX < 32) ? 32 : RIFT_BAND_PX;

  for (int y = 0; y < h; y++) {
    float m = mistAt(0, y, seed ^ 0x11335577);
    float s = (m - 0.5f) * 2.0f;
    int by = y / band;
    float mb = (noise8_grid(by, 7, seed) / 255.0f - 0.5f) * 2.0f;

    float shift = (0.65f * s + 0.35f * mb) * RIFT_SHEAR_PX;
    int s0 = (int)shift;
    float frac = shift - s0;

    for (int x = 0; x < w; x++) {
      int x0 = x + s0;
      int x1 = x0 + 1;
      if (x0 < 0) x0 = 0;
      if (x0 >= w) x0 = w - 1;
      if (x1 < 0) x1 = 0;
      if (x1 >= w) x1 = w - 1;

      size_t di = ((size_t)y * w + x) * 3;
      size_t i0 = ((size_t)y * w + x0) * 3;
      size_t i1 = ((size_t)y * w + x1) * 3;

      rgb[di+0] = (uint8_t)((1.0f - frac) * scratch[i0+0] + frac * scratch[i1+0]);
      rgb[di+1] = (uint8_t)((1.0f - frac) * scratch[i0+1] + frac * scratch[i1+1]);
      rgb[di+2] = (uint8_t)((1.0f - frac) * scratch[i0+2] + frac * scratch[i1+2]);
    }
  }
}

static void portalSwirlInPlace(uint8_t* rgb, int w, int h, uint8_t* scratch, uint32_t seed) {
  if (SWIRL_STRENGTH <= 0.001f) return;

  size_t len = (size_t)w * h * 3;
  memcpy(scratch, rgb, len);

  float cx = (w - 1) * 0.5f;
  float cy = (h - 1) * 0.5f;

  float maxR = ((cx < cy) ? cx : cy) * SWIRL_RADIUS;
  if (maxR < 1.0f) maxR = 1.0f;
  float invMaxR2 = 1.0f / (maxR * maxR);

  float phase = (float)((seed >> 8) & 1023) / 1023.0f;

  for (int y = 0; y < h; y++) {
    float dy = (float)y - cy;
    for (int x = 0; x < w; x++) {
      float dx = (float)x - cx;

      float r2n = (dx*dx + dy*dy) * invMaxR2;
      if (r2n > 1.0f) r2n = 1.0f;

      float k = r2n * r2n;

      float m = mistAt(x + (int)(phase * 97), y + (int)(phase * 131), seed ^ 0x77AA55CC);
      float mod = 0.82f + 0.36f * m;

      float a = SWIRL_STRENGTH * k * mod;
      if (a > 1.2f) a = 1.2f;

      float sx = cx + (dx - dy * a);
      float sy = cy + (dy + dx * a);

      uint8_t rr, gg, bb;
      sampleRGBBilinear(scratch, w, h, sx, sy, rr, gg, bb);

      size_t di = ((size_t)y * w + x) * 3;
      rgb[di+0] = rr;
      rgb[di+1] = gg;
      rgb[di+2] = bb;
    }
  }
}

static void veilDriftInPlace(uint8_t* rgb, int w, int h, uint8_t* scratch, uint32_t seed) {
  if (DRIFT_PX <= 0.001f) return;

  size_t len = (size_t)w * h * 3;
  memcpy(scratch, rgb, len);

  int scale = (DRIFT_SCALE_PX < 32) ? 32 : DRIFT_SCALE_PX;

  float cx = (w - 1) * 0.5f;
  float cy = (h - 1) * 0.5f;
  float invMaxD2 = 1.0f / (cx*cx + cy*cy + 1.0f);

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int xs = (x * 256) / scale;
      int ys = (y * 256) / scale;

      float m1 = mistAt(xs,     ys,     seed ^ 0x1F2E3D4C);
      float m2 = mistAt(xs + 7, ys + 9, seed ^ 0xBADC0FFE);

      float vx = (m1 - 0.5f) * 2.0f;
      float vy = (m2 - 0.5f) * 2.0f;

      float dx = (float)x - cx;
      float dy = (float)y - cy;
      float edge = (dx*dx + dy*dy) * invMaxD2;
      if (edge > 1.0f) edge = 1.0f;

      float strength = DRIFT_PX * (0.45f + 0.75f * edge);

      float sx = (float)x + vx * strength;
      float sy = (float)y + vy * strength;

      uint8_t rr, gg, bb;
      sampleRGBBilinear(scratch, w, h, sx, sy, rr, gg, bb);

      size_t di = ((size_t)y * w + x) * 3;
      rgb[di+0] = rr;
      rgb[di+1] = gg;
      rgb[di+2] = bb;
    }
  }
}

static void chromaSplitHighlights(uint8_t* rgb, int w, int h, const uint8_t* glow, int qw, int qh, uint8_t* scratch) {
  if (!glow || CHROMA_SHIFT_PX <= 0.01f || CHROMA_AMOUNT <= 0.01f) return;

  size_t len = (size_t)w * h * 3;
  memcpy(scratch, rgb, len);

  float shift = CHROMA_SHIFT_PX;
  int s0 = (int)shift;
  float frac = shift - s0;

  for (int y = 0; y < h; y++) {
    int gy = y / 4; if (gy >= qh) gy = qh - 1;
    for (int x = 0; x < w; x++) {
      int gx = x / 4; if (gx >= qw) gx = qw - 1;
      int gv = glow[gy*qw + gx];
      if (gv < CHROMA_HI_THRESH) continue;

      float gk = (gv - CHROMA_HI_THRESH) / 48.0f;
      if (gk < 0) gk = 0;
      if (gk > 1) gk = 1;
      float amt = CHROMA_AMOUNT * gk;

      int xl0 = x - s0;
      int xl1 = xl0 - 1;
      int xr0 = x + s0;
      int xr1 = xr0 + 1;

      if (xl0 < 0) xl0 = 0;
      if (xl1 < 0) xl1 = 0;
      if (xr0 >= w) xr0 = w - 1;
      if (xr1 >= w) xr1 = w - 1;

      size_t di = ((size_t)y * w + x) * 3;

      size_t il0 = ((size_t)y * w + xl0) * 3;
      size_t il1 = ((size_t)y * w + xl1) * 3;
      size_t ir0 = ((size_t)y * w + xr0) * 3;
      size_t ir1 = ((size_t)y * w + xr1) * 3;

      float rl = (1.0f - frac) * scratch[il0+0] + frac * scratch[il1+0];
      float br = (1.0f - frac) * scratch[ir0+2] + frac * scratch[ir1+2];

      int r = rgb[di+0];
      int b = rgb[di+2];

      r = (int)((1.0f - amt) * r + amt * rl);
      b = (int)((1.0f - amt) * b + amt * br);

      rgb[di+0] = clamp8(r);
      rgb[di+2] = clamp8(b);
    }
  }
}

static void applyVeilGrade(uint8_t* rgb, int w, int h) {
  size_t len = (size_t)w * h * 3;
  uint8_t* scratch = (uint8_t*)ps_malloc(len);
  if (!scratch) scratch = (uint8_t*)malloc(len);
  if (!scratch) return;

  uint32_t seed = esp_random() ^ 0xA5A5BEEF;

  smoothWarpInPlace(rgb, w, h, scratch);
  riftShearInPlace(rgb, w, h, scratch, seed);
  portalSwirlInPlace(rgb, w, h, scratch, seed);
  veilDriftInPlace(rgb, w, h, scratch, seed);

  int qw = w / 4;
  int qh = h / 4;
  uint8_t* glow = (uint8_t*)ps_malloc((size_t)qw * qh);
  if (!glow) glow = (uint8_t*)malloc((size_t)qw * qh);

  if (glow) {
    for (int y = 0; y < qh; y++) {
      for (int x = 0; x < qw; x++) {
        int sx = x * 4;
        int sy = y * 4;
        size_t i = ((size_t)sy * w + sx) * 3;
        uint8_t r = rgb[i+0], g = rgb[i+1], b = rgb[i+2];
        uint8_t L = luma8(r,g,b);
        glow[y*qw + x] = (L > GLOW_THRESHOLD) ? (uint8_t)(L - GLOW_THRESHOLD) : 0;
      }
    }
    blur_u8(glow, qw, qh);
    blur_u8(glow, qw, qh);
  }

  const int fogR = 190, fogG = 90, fogB = 210;

  int cx = w / 2;
  int cy = h / 2;
  float invMaxD2 = 1.0f / (float)(cx*cx + cy*cy + 1);

  for (int y = 0; y < h; y++) {
    int dy = y - cy;
    for (int x = 0; x < w; x++) {
      size_t i = ((size_t)y * w + x) * 3;

      int r = rgb[i+0];
      int g = rgb[i+1];
      int b = rgb[i+2];

      uint8_t L8 = luma8((uint8_t)r,(uint8_t)g,(uint8_t)b);
      float l = L8 / 255.0f;

      float pw = PINK_WEIGHT;
      int r2 = (int)(r * (1.00f + 0.14f*pw) + b * (0.10f*pw));
      int g2 = (int)(g * (1.00f - 0.28f*pw));
      int b2 = (int)(b * (1.00f + 0.22f*pw) + r * (0.06f*pw));

      int mid = 128;
      r2 = mid + (int)((r2 - mid) * 1.02f);
      g2 = mid + (int)((g2 - mid) * 1.01f);
      b2 = mid + (int)((b2 - mid) * 1.03f);

      float sh = (1.0f - l);
      float hi = l;
      b2 += (int)(sh * 18);
      r2 += (int)(hi * 14);

      float m = mistAt(x, y, seed);
      float mv = (m - 0.5f) * 2.0f * MIST_VARIATION;

      float hazeBase = HAZE_STRENGTH * (1.0f - (l * 0.55f));
      hazeBase *= (0.85f + 0.30f * m);

      float depthLie = DEPTH_LIE_STRENGTH * ((m - 0.5f) * 2.0f) * (l - 0.35f);

      float haze = hazeBase * (1.0f + mv) + depthLie;
      if (haze < 0.0f) haze = 0.0f;
      if (haze > 0.88f) haze = 0.88f;

      r2 = (int)((1.0f - haze) * r2 + haze * fogR);
      g2 = (int)((1.0f - haze) * g2 + haze * fogG);
      b2 = (int)((1.0f - haze) * b2 + haze * fogB);

      float shadowK = (1.0f - l);
      shadowK = shadowK * shadowK;
      float emit = SHADOW_EMISSION * shadowK * (0.55f + 0.45f * m);
      if (emit > 0.001f) {
        r2 += (int)(emit * 85);
        g2 += (int)(emit * 14);
        b2 += (int)(emit * 105);
      }

      if (glow) {
        int gx = x / 4;
        int gy = y / 4;
        int gv = glow[gy*qw + gx];
        if (gv > 0) {
          float ggain = GLOW_GAIN * (gv / 64.0f);
          r2 += (int)(ggain * 54);
          g2 += (int)(ggain * 10);
          b2 += (int)(ggain * 66);
        }
      }

      int dx = x - cx;
      float d2 = (float)(dx*dx + dy*dy) * invMaxD2;
      float vig = 1.0f - VIGNETTE_STRENGTH * d2;
      if (vig < 0.0f) vig = 0.0f;

      r2 = (int)(r2 * vig);
      g2 = (int)(g2 * vig);
      b2 = (int)(b2 * vig);

      rgb[i+0] = clamp8(r2);
      rgb[i+1] = clamp8(g2);
      rgb[i+2] = clamp8(b2);
    }
  }

  chromaSplitHighlights(rgb, w, h, glow, qw, qh, scratch);

  if (glow) free(glow);

  if (SOFT_BLUR_PASSES > 0 && SOFT_BLEND > 0.01f) {
    memcpy(scratch, rgb, len);

    uint8_t* tmp = (uint8_t*)ps_malloc(len);
    if (!tmp) tmp = (uint8_t*)malloc(len);

    if (tmp) {
      for (int p = 0; p < SOFT_BLUR_PASSES; p++) {
        blur_rgb888_box_with_tmp(scratch, tmp, w, h);
      }
      blend_rgb888(rgb, scratch, w, h, SOFT_BLEND);
      free(tmp);
    }
  }

  free(scratch);
}

// =====================
// Camera init (RGB565 for veilgrade processing)
// =====================
static bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;

  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;

  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = kFrameSize;
  config.jpeg_quality = kJpegQuality;  // not used for RGB565, but leave sane
  config.grab_mode    = CAMERA_GRAB_LATEST;

  // Keep buffers conservative for stability
  config.fb_count     = psramFound() ? 2 : 1;
  config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);
  return (err == ESP_OK);
}

// =====================
// Capture core (runs in task)
// =====================
static String doCaptureAndSave() {
  // NEVER touch GPIO4 here. Keep it hi-z.
  ledHiZ();

  if (!g_sdOK || SD_MMC.cardType() == CARD_NONE) return "SD not ready";
  if (!g_cameraOK) return "Camera not initialized";

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return "Camera capture failed (fb null)";
  if (fb->format != PIXFORMAT_RGB565) {
    esp_camera_fb_return(fb);
    return "Unexpected pixel format";
  }

  int w = fb->width;
  int h = fb->height;

  size_t rgbLen = (size_t)w * (size_t)h * 3;
  uint8_t* rgb = (uint8_t*)ps_malloc(rgbLen);
  if (!rgb) rgb = (uint8_t*)malloc(rgbLen);

  if (!rgb) {
    esp_camera_fb_return(fb);
    return "Out of memory (rgb888)";
  }

  rgb565_to_rgb888((const uint8_t*)fb->buf, w, h, rgb);
  esp_camera_fb_return(fb);

  applyVeilGrade(rgb, w, h);

  uint8_t* jpgBuf = nullptr;
  size_t jpgLen = 0;
  bool ok = fmt2jpg(rgb, rgbLen, w, h, PIXFORMAT_RGB888, kJpegQuality, &jpgBuf, &jpgLen);

  free(rgb);

  if (!ok || !jpgBuf || jpgLen < 100) {
    if (jpgBuf) free(jpgBuf);
    return "JPEG encode failed";
  }

  String filename = makeFilename();
  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    free(jpgBuf);
    return "Failed to open file on SD";
  }

  file.write(jpgBuf, jpgLen);
  file.close();
  free(jpgBuf);

  // Now it’s safe to pulse the LED (camera is idle).
  ledPulseIdle(35);

  return "Saved: " + filename;
}

// =====================
// Capture task (keeps UI responsive)
// =====================
void captureTask(void* arg) {
  (void)arg;
  for (;;) {
    bool doIt = false;

    portENTER_CRITICAL(&g_mux);
    if (g_captureRequested && !g_busy) {
      g_captureRequested = false;
      g_busy = true;
      g_status = "Capturing...";
      doIt = true;
    }
    portEXIT_CRITICAL(&g_mux);

    if (doIt) {
      String result = doCaptureAndSave();

      portENTER_CRITICAL(&g_mux);
      g_status = result;
      g_busy = false;
      portEXIT_CRITICAL(&g_mux);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}



// =====================
// Web UI - ASCII ONLY
// =====================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <title>VEIL</title>
  <style>
    :root{
      /* OCCULT NEON BLOOD RITUAL */
      --bg:#020104;
      --panel:#07030a;
      --panel2:#050209;

      /* brighter, visible edges */
      --border: rgba(255,31,74,0.32);
      --borderSoft: rgba(210,178,108,0.18);
      --borderInk: rgba(70,255,179,0.14);

      --text:#f7f2f7;
      --muted:#c7a9bb;

      --blood:#b60f2a;
      --bloodHot:#ff1f4a;
      --gold:#d2b26c;
      --silver:#cfd6e2;

      /* scry green + violet ink */
      --scry:#46ffb3;
      --ink:#9b5cff;

      --warn:#ff2a52;

      --shadow: rgba(0,0,0,0.78);
      --glass2: rgba(0,0,0,0.34);

      --scan: rgba(255,255,255,0.012);

      --glowBlood: rgba(255,31,74,0.26);
      --glowGold: rgba(210,178,108,0.18);
      --glowScry: rgba(70,255,179,0.14);
      --glowInk: rgba(155,92,255,0.14);

      --font: "Courier New", Courier, ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", monospace;
    }

    *{box-sizing:border-box}

    body{
      margin:0;
      padding:16px;
      color:var(--text);

      /* richer, more vivid occult neon field */
      background:
        radial-gradient(920px 600px at 14% -14%, rgba(255,31,74,0.30) 0%, rgba(0,0,0,0) 62%),
        radial-gradient(860px 560px at 92% 12%, rgba(210,178,108,0.20) 0%, rgba(0,0,0,0) 62%),
        radial-gradient(880px 560px at 78% 92%, rgba(70,255,179,0.12) 0%, rgba(0,0,0,0) 62%),
        radial-gradient(980px 720px at 46% 120%, rgba(155,92,255,0.14) 0%, rgba(0,0,0,0) 58%),
        linear-gradient(180deg, #010102 0%, var(--bg) 72%);

      font-family: var(--font);
    }

    .wrap{max-width:860px;margin:0 auto}

    /* CRT scanlines */
    body:before{
      content:"";
      position:fixed;
      inset:0;
      pointer-events:none;
      background:
        repeating-linear-gradient(180deg, var(--scan) 0px, var(--scan) 1px, rgba(0,0,0,0) 3px, rgba(0,0,0,0) 8px);
      opacity:0.22;
      mix-blend-mode: overlay;
      z-index:0;
    }

    header, .grid{position:relative; z-index:1;}

    header{
      display:flex;
      flex-direction:column;
      gap:8px;
      margin: 2px 0 14px 0;
    }

    .title{
      font-weight:900;
      letter-spacing:0.20em;
      font-size:22px;
      line-height:1.05;
      text-transform:uppercase;

      background: linear-gradient(90deg, var(--bloodHot) 0%, var(--gold) 50%, var(--scry) 100%);
      -webkit-background-clip:text;
      background-clip:text;
      color: transparent;

      text-shadow:
        0 0 10px rgba(255,31,74,0.20),
        0 0 22px rgba(255,31,74,0.10),
        0 0 26px rgba(70,255,179,0.08);
    }

    .tag{
      color:var(--muted);
      letter-spacing:0.18em;
      font-size:12px;
      text-transform:lowercase;
      opacity:0.95;
    }

    .grid{
      display:grid;
      grid-template-columns: 1fr;
      gap:12px;
    }

    .card{
      position:relative;
      background:
        linear-gradient(180deg, rgba(255,255,255,0.02) 0%, rgba(0,0,0,0.30) 100%),
        linear-gradient(180deg, var(--panel) 0%, var(--panel2) 100%);
      border:1px solid var(--border);
      border-radius:14px;
      padding:12px;
      box-shadow:
        0 14px 42px var(--shadow),
        0 0 0 1px rgba(255,31,74,0.10) inset,
        0 0 0 1px rgba(210,178,108,0.06);
      overflow:hidden;
    }

    /* neon edge kiss (subtle but visible) */
    .card:after{
      content:"";
      position:absolute;
      inset:-1px;
      pointer-events:none;
      border-radius:14px;
      background:
        radial-gradient(700px 220px at 20% 0%, rgba(255,31,74,0.12), rgba(0,0,0,0) 60%),
        radial-gradient(700px 220px at 80% 22%, rgba(70,255,179,0.08), rgba(0,0,0,0) 60%),
        radial-gradient(700px 220px at 52% 110%, rgba(210,178,108,0.08), rgba(0,0,0,0) 60%);
      opacity:0.60;
      mix-blend-mode: screen;
    }

    .row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}

    .btn{
      appearance:none;
      font-family: var(--font);
      border:1px solid rgba(255,31,74,0.34);
      background: rgba(0,0,0,0.30);
      color:var(--text);
      padding:10px 12px;
      border-radius:12px;
      font-weight:900;
      letter-spacing:0.14em;
      text-transform:uppercase;
      cursor:pointer;
      position:relative;
      transition: transform 0.06s ease, filter 0.12s ease, box-shadow 0.14s ease;
    }

    .btn:hover{
      filter: brightness(1.10);
      box-shadow:
        0 0 0 1px rgba(255,31,74,0.14) inset,
        0 0 18px rgba(155,92,255,0.10),
        0 0 24px rgba(255,31,74,0.08);
    }

    .btn.primary{
      border-color: rgba(255,31,74,0.72);
      background:
        radial-gradient(240px 70px at 28% 0%, rgba(210,178,108,0.16) 0%, rgba(0,0,0,0) 60%),
        radial-gradient(220px 70px at 72% 120%, rgba(70,255,179,0.10) 0%, rgba(0,0,0,0) 62%),
        linear-gradient(180deg, rgba(255,31,74,0.34), rgba(182,15,42,0.14));
      box-shadow:
        0 0 0 1px rgba(255,31,74,0.18) inset,
        0 0 28px rgba(255,31,74,0.18),
        0 0 46px rgba(255,31,74,0.08);
    }

    .btn.primary:after{
      content:"";
      position:absolute;
      left:10px; right:10px; bottom:-1px;
      height:2px;
      background: linear-gradient(90deg, rgba(70,255,179,0), rgba(255,31,74,0.70), rgba(210,178,108,0.35), rgba(70,255,179,0));
      opacity:0.75;
      filter: blur(0.35px);
    }

    .btn:active{transform:translateY(1px)}
    .btn:disabled{opacity:0.55;cursor:not-allowed; filter:saturate(0.8); box-shadow:none}

    .status{
      margin:10px 0 0 0;
      color:var(--muted);
      font-size:13px;
      line-height:1.35;
      word-break:break-word;
    }

    .kv{
      margin-top:10px;
      display:flex;
      gap:10px;
      flex-wrap:wrap;
      color:var(--muted);
      font-size:12px;
    }

    .pill{
      border:1px solid rgba(255,31,74,0.30);
      border-radius:999px;
      padding:6px 10px;
      background: rgba(0,0,0,0.34);
      box-shadow:
        0 0 0 1px rgba(210,178,108,0.08) inset;
    }

    .ok{
      color:var(--scry);
      text-shadow: 0 0 10px rgba(70,255,179,0.14);
    }
    .bad{
      color:var(--warn);
      text-shadow: 0 0 12px rgba(255,42,82,0.16);
    }

    .split{
      display:grid;
      grid-template-columns: 1fr;
      gap:12px;
    }
    @media (min-width: 860px){
      .split{grid-template-columns: 1.1fr 0.9fr;}
    }

    .list{
      max-height: 42vh;
      overflow:auto;
      border:1px solid rgba(255,31,74,0.36);
      border-radius:12px;
      background: rgba(0,0,0,0.34);
      box-shadow:
        0 0 0 1px rgba(210,178,108,0.10) inset,
        0 0 22px rgba(255,31,74,0.05);
    }

    .item{
      display:flex;
      align-items:center;
      gap:10px;
      padding:10px 12px;
      border-bottom:1px solid rgba(210,178,108,0.16);
      cursor:pointer;
      user-select:none;
    }
    .item:last-child{border-bottom:0}
    .item:hover{background:rgba(255,31,74,0.10)}
    .item:active{background:rgba(70,255,179,0.07)}

    .fname{
      font-size:12px;
      color:var(--text);
      opacity:0.95;
    }

    .sub{
      margin-top:8px;
      color:var(--muted);
      font-size:12px;
      line-height:1.35;
    }

    .viewer{
      border:1px solid rgba(255,31,74,0.36);
      border-radius:12px;
      background: rgba(0,0,0,0.34);
      padding:12px;
      min-height: 240px;
      display:flex;
      flex-direction:column;
      gap:10px;
      box-shadow:
        0 0 0 1px rgba(210,178,108,0.10) inset;
    }

    .viewerTop{
      display:flex;
      justify-content:space-between;
      align-items:center;
      gap:8px;
      flex-wrap:wrap;
    }

    .viewerName{
      font-size:12px;
      color:var(--muted);
      overflow:hidden;
      text-overflow:ellipsis;
      white-space:nowrap;
      max-width: 100%;
    }

    .viewerImgWrap{
      flex:1;
      display:flex;
      justify-content:center;
      align-items:center;
      overflow:hidden;
      border-radius:10px;
      border:1px solid rgba(210,178,108,0.18);
      background:
        radial-gradient(320px 200px at 50% 42%, rgba(255,31,74,0.16), rgba(0,0,0,0) 62%),
        radial-gradient(320px 200px at 50% 70%, rgba(210,178,108,0.10), rgba(0,0,0,0) 62%),
        radial-gradient(360px 220px at 78% 18%, rgba(70,255,179,0.08), rgba(0,0,0,0) 60%),
        rgba(0,0,0,0.34);
      padding:10px;
      min-height: 180px;
    }

    img#viewerImg{
      max-width: 100%;
      max-height: 60vh;
      transform: rotate(90deg);
      transform-origin: center center;
      border-radius:10px;
      display:block;
      box-shadow:
        0 14px 44px rgba(0,0,0,0.50),
        0 0 26px rgba(255,31,74,0.08);
    }

    .hint{
      color:var(--muted);
      font-size:12px;
      line-height:1.35;
    }

    .label{
      font-weight:900;
      letter-spacing:0.16em;
      text-transform:uppercase;
      font-size:12px;
      color: rgba(247,242,247,0.92);
    }

    .list::-webkit-scrollbar{width:10px}
    .list::-webkit-scrollbar-track{background:rgba(0,0,0,0.18);border-radius:12px}
    .list::-webkit-scrollbar-thumb{
      background:rgba(255,31,74,0.18);
      border-radius:12px;
      border:1px solid rgba(255,31,74,0.30);
    }
  </style>
</head>

<body>
  <div class="wrap">
    <header>
      <div class="title">VEIL</div>
      <div class="tag">ritual hardware</div>
    </header>

    <div class="grid">
      <div class="card">
        <div class="row">
          <button class="btn primary" id="btnInvoke" onclick="capture()">INVOKE</button>
          <button class="btn" id="btnSync" onclick="loadGallery(true)">SYNC ARCHIVE</button>
        </div>

        <p class="status" id="status">stand by...</p>

        <div class="kv">
          <div class="pill">CAMERA: <span id="cam">?</span></div>
          <div class="pill">STORAGE: <span id="sd">?</span></div>
          <div class="pill">STATE: <span id="busy">?</span></div>
        </div>

        <div class="sub">Tip: GPIO13 button also invokes capture.</div>
      </div>

      <div class="card split">
        <div>
          <div class="row" style="justify-content:space-between;margin-bottom:8px">
            <div class="label">ARCHIVE</div>
            <div class="hint" id="count">-</div>
          </div>

          <div class="list" id="list">
            <div class="item"><div class="fname">loading...</div></div>
          </div>

          <div class="sub">Tap a file to view it below (rotated via CSS).</div>
        </div>

        <div class="viewer">
          <div class="viewerTop">
            <div class="viewerName" id="viewerName">no selection</div>
            <button class="btn" onclick="clearViewer()">CLEAR</button>
          </div>

          <div class="viewerImgWrap">
            <img id="viewerImg" alt="" style="display:none;">
            <div id="viewerEmpty" class="hint">select an item from the archive</div>
          </div>

          <div class="hint">If images feel stale, sync archive (cache busting is on).</div>
        </div>
      </div>
    </div>
  </div>

<script>
let polling = false;
let busyPrev = null;

function setCaptureUI(isBusy){
  const b1 = document.getElementById('btnInvoke');
  const b2 = document.getElementById('btnSync');
  if(b1) b1.disabled = !!isBusy;
  if(b2) b2.disabled = !!isBusy;
}

async function capture(){
  try{
    setCaptureUI(true);
    const r = await fetch('/capture', {method:'POST'});
    const t = await r.text();
    document.getElementById('status').textContent = t;
    if(!polling){
      polling = true;
      pollStatus();
    }
  }catch(e){
    document.getElementById('status').textContent = "error: " + e;
    setCaptureUI(false);
  }
}

async function pollStatus(){
  try{
    const r = await fetch('/status', {cache:'no-store'});
    const j = await r.json();

    document.getElementById('status').textContent = j.status || '...';

    const camEl = document.getElementById('cam');
    const sdEl  = document.getElementById('sd');
    const bEl   = document.getElementById('busy');

    camEl.textContent = j.camera ? "OK" : "FAIL";
    sdEl.textContent  = j.sd ? "OK" : "FAIL";
    bEl.textContent   = j.busy ? "BUSY" : "IDLE";

    camEl.className = j.camera ? "ok" : "bad";
    sdEl.className  = j.sd ? "ok" : "bad";
    bEl.className   = j.busy ? "bad" : "ok";

    setCaptureUI(j.busy);

    if (busyPrev === null) busyPrev = j.busy;

    if(!j.busy){
      polling = false;
      if (busyPrev === true){
        await loadGallery(false);
      }
      busyPrev = false;
      return;
    } else {
      busyPrev = true;
    }
  }catch(e){
    setCaptureUI(false);
  }
  setTimeout(pollStatus, 500);
}

function clearViewer(){
  const img = document.getElementById('viewerImg');
  img.style.display = 'none';
  img.src = '';
  document.getElementById('viewerEmpty').style.display = 'block';
  document.getElementById('viewerName').textContent = 'no selection';
}

function openInViewer(filename){
  const img = document.getElementById('viewerImg');
  const bust = Date.now();
  img.src = '/img?name=' + encodeURIComponent(filename) + '&v=' + bust;

  document.getElementById('viewerName').textContent = filename;
  document.getElementById('viewerEmpty').style.display = 'none';
  img.style.display = 'block';
}

async function loadGallery(force){
  const list = document.getElementById('list');
  list.innerHTML = "<div class='item'><div class='fname'>loading...</div></div>";

  try{
    const bust = force ? ('?v=' + Date.now()) : '';
    const r = await fetch('/list' + bust, {cache:'no-store'});
    const files = await r.json();

    document.getElementById('count').textContent =
      files.length ? (files.length + " files") : "empty";

    if(!files.length){
      list.innerHTML = "<div class='item'><div class='fname'>ARCHIVE EMPTY</div></div>";
      clearViewer();
      return;
    }

    let html = "";
    for(const f of files){
      html += "<div class='item' onclick='openInViewer(" + JSON.stringify(f) + ")'>" +
              "<div class='fname'>" + escapeHtml(f) + "</div></div>";
    }
    list.innerHTML = html;
  }catch(e){
    list.innerHTML = "<div class='item'><div class='fname'>archive load failed</div></div>";
  }
}

function escapeHtml(s){
  return (s+'').replace(/[&<>"']/g, m => ({
    '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
  }[m]));
}

pollStatus();
loadGallery(true);
</script>
</body>
</html>
)HTML";

// =====================
// Web handlers
// =====================
void handleRoot() {
  sendNoCache();
  server.send(200, "text/html", FPSTR(INDEX_HTML));
}

void handleStatus() {
  bool busy, cam, sd;
  String st;
  portENTER_CRITICAL(&g_mux);
  busy = g_busy;
  st = g_status;
  cam = g_cameraOK;
  sd = g_sdOK;
  portEXIT_CRITICAL(&g_mux);

  st.replace("\\", "\\\\");
  st.replace("\"", "\\\"");

  String out = "{\"busy\":";
  out += (busy ? "true" : "false");
  out += ",\"camera\":";
  out += (cam ? "true" : "false");
  out += ",\"sd\":";
  out += (sd ? "true" : "false");
  out += ",\"status\":\"";
  out += st;
  out += "\"}";

  sendNoCache();
  server.send(200, "application/json", out);
}

void handleCaptureStart() {
  bool accepted = false;

  portENTER_CRITICAL(&g_mux);
  if (!g_busy && !g_captureRequested) {
    g_captureRequested = true;
    g_status = "Capture queued...";
    accepted = true;
  }
  portEXIT_CRITICAL(&g_mux);

  sendNoCache();
  if (accepted) server.send(202, "text/plain", "Capture queued...");
  else server.send(409, "text/plain", "Busy.");
}

static uint32_t parseVeilIndex(const String &path) {
  // accepts "veil_00000012.jpg" or "/veil_00000012.jpg"
  int s = path.indexOf("veil_");
  if (s < 0) return 0;
  s += 5; // after "veil_"
  int e = path.lastIndexOf(".jpg");
  if (e < 0 || e <= s) return 0;
  return (uint32_t) path.substring(s, e).toInt();
}

void handleList() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");

  if (!g_sdOK) {
    server.send(200, "application/json", "[]");
    return;
  }

  File root = SD_MMC.open("/");
  if (!root) {
    server.send(200, "application/json", "[]");
    return;
  }

  const int MAX_FILES = 80;          // keep UI snappy + low RAM
  String files[MAX_FILES];
  uint32_t idxs[MAX_FILES];
  int count = 0;

  File f = root.openNextFile();
  while (f && count < MAX_FILES) {
    String n = String(f.name());
    bool ok = (!f.isDirectory() && n.endsWith(".jpg") && n.indexOf("veil_") != -1);
    f.close();

    if (ok) {
      if (!n.startsWith("/")) n = "/" + n;
      uint32_t id = parseVeilIndex(n);
      if (id > 0) {
        files[count] = n;
        idxs[count] = id;
        count++;
      }
    }

    f = root.openNextFile();
  }
  root.close();

  // sort newest-first by numeric index
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (idxs[j] > idxs[i]) {
        uint32_t ti = idxs[i]; idxs[i] = idxs[j]; idxs[j] = ti;
        String ts = files[i]; files[i] = files[j]; files[j] = ts;
      }
    }
  }

  String out = "[";
  for (int i = 0; i < count; i++) {
    if (i) out += ",";
    out += "\"" + files[i] + "\"";
  }
  out += "]";
  server.send(200, "application/json", out);
}
void handleImg() {
  if (!g_sdOK) { server.send(404, "text/plain", "SD not ready"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing ?name="); return; }

  String name = server.arg("name");
  if (!name.startsWith("/")) name = "/" + name;

  File f = SD_MMC.open(name, FILE_READ);
  if (!f) { server.send(404, "text/plain", "Not found"); return; }

  // Streaming file is fine; browser may cache image, that's okay.
  server.streamFile(f, "image/jpeg");
  f.close();
}

// =====================
// Setup / Loop
// =====================
void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println("=== VeilCam boot (stable) ===");
  Serial.print("PSRAM found: ");
  Serial.println(psramFound() ? "YES" : "NO");

  ledHiZ();
  pinMode(BTN_PIN, INPUT_PULLUP);

  // Camera FIRST
  g_cameraOK = initCamera();
  Serial.print("Camera init: ");
  Serial.println(g_cameraOK ? "OK" : "FAILED");

  // SD next
  g_sdOK = initSD();
  Serial.print("SD init: ");
  Serial.println(g_sdOK ? "OK" : "FAILED");
  // NEW
  if (g_sdOK) {
  prefs.begin("veilcam", false);
  g_photoIndex = prefs.getUInt("idx", 0);
}

  // WiFi/AP last
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  bool apOK = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  Serial.print("AP start: ");
  Serial.println(apOK ? "OK" : "FAILED");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/list", handleList);
  server.on("/img", handleImg);
  server.on("/capture", HTTP_POST, handleCaptureStart);
  server.begin();
  server.enableDelay(false);

  portENTER_CRITICAL(&g_mux);
  g_status = (g_cameraOK ? "Idle." : "Camera init failed.");
  portEXIT_CRITICAL(&g_mux);

  // Capture task pinned away from network stack
  xTaskCreatePinnedToCore(captureTask, "capTask", 8192, nullptr, 2, &g_capTask, 1);

  ledHiZ();
  Serial.println("Web UI ready: http://192.168.4.1/");
}

static uint8_t lastBtn = HIGH;
static uint32_t lastEdgeMs = 0;
static const uint32_t DEBOUNCE_MS = 180;

void loop() {
  server.handleClient();

  // Button -> queue capture (NEVER capture inline here)
  uint8_t b = (uint8_t)digitalRead(BTN_PIN);
  uint32_t now = millis();

  if (lastBtn == HIGH && b == LOW) {
    if ((now - lastEdgeMs) > DEBOUNCE_MS) {
      lastEdgeMs = now;

      portENTER_CRITICAL(&g_mux);
      if (!g_busy && !g_captureRequested) {
        g_captureRequested = true;
        g_status = "Capture queued (button)...";
      }
      portEXIT_CRITICAL(&g_mux);
    }
  }
  lastBtn = b;
}