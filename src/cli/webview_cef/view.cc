#include "view.h"
#include "../../core/transport/socket/protocol.h"


#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "include/cef_app.h"
#include "input.h"
#include "shm_format.h"
#include "shm_diff.h"
#include "public/idk_shm_ring.h"

#include "public/idk_producer.h"
#include "public/idk_shm_delta.h"
#include "tasks.h"

/* DRM fourcc codes (BGRA/RGBA 8888 memory order, matching the CEF formats). */
#define DRM_FORMAT_ARGB8888 0x34325241u /* CEF_COLOR_TYPE_BGRA_8888  */
#define DRM_FORMAT_ABGR8888 0x34324241u /* CEF_COLOR_TYPE_RGBA_8888  */

#define DMABUF_REJECT_LIMIT 5

View::View(GroupConfig conf, bool no_dmabuf, std::string sock_path, bool sock_abstract)
    : conf_(std::move(conf)), sock_path_(std::move(sock_path)), sock_abstract_(sock_abstract), no_dmabuf_(no_dmabuf),
      render_w_(conf_.width), render_h_(conf_.height) {
  use_dmabuf_ = !no_dmabuf_;
  const char *roi = getenv("IDK_CEF_SHM_ROI");
  shm_roi_enabled_ = roi && strcmp(roi, "1") == 0;
}

View::~View() { Stop(); }

/* Raw producer name for idk_tp_init (leading '\0' = abstract namespace). */
std::string View::SockName() const { return sock_abstract_ ? std::string(1, '\0') + sock_path_ : sock_path_; }

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void View::Start() {
  input_ = new InputThread(this, sock_path_);
  input_->Start();
  ConnectTask();
}

void View::Stop() {
  quit_.store(true, std::memory_order_release);
  if (input_) {
    input_->Stop();
    delete input_;
    input_ = nullptr;
  }
}

void View::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
  /* Windowless browsers start hidden — show the view so the page's rAF/
   * CSS animations are not throttled to 1fps. */
  browser_->GetHost()->WasHidden(false);
  LoadUrl();
  IDK_LOG("webview-cef", "browser created (%dx%d)\n", render_w_, render_h_);
}

void View::LoadUrl() {
  if (!browser_ || conf_.url.empty())
    return;
  IDK_LOG("webview-cef", "loading %s\n", conf_.url.c_str());
  browser_->GetMainFrame()->LoadURL(conf_.url);
}

void View::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) {
  if (!frame->IsMain() || frame->GetURL() == "about:blank" || page_ready_)
    return;
  page_ready_.store(true, std::memory_order_release);
  InjectScripts();
  if (request_credit_)
    NudgeFrame();
}

void View::InjectScripts() {
  if (!browser_ || conf_.scripts.empty())
    return;
  CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
  for (const auto &path : conf_.scripts) {
    std::ifstream in(path);
    if (!in) {
      IDK_LOG("webview-cef", "inject script unreadable: %s\n", path.c_str());
      continue;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string js = "(function(){\n" + ss.str() + "\n})();";
    frame->ExecuteJavaScript(js, path, 0);
    IDK_LOG("webview-cef", "injected %s\n", path.c_str());
  }
}

/* ── Render handler ────────────────────────────────────────────────── */

void View::GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) { rect = CefRect(0, 0, render_w_, render_h_); }

bool View::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo &si) {
  si.device_scale_factor = 1.0f;
  si.rect = CefRect(0, 0, render_w_, render_h_);
  return true;
}

static uint8_t cefCursorShape(cef_cursor_type_t type) {
  switch (type) {
  case CT_HAND:
    return IDK_CURSOR_POINTER;
  case CT_CROSS:
    return IDK_CURSOR_CROSSHAIR;
  case CT_IBEAM:
    return IDK_CURSOR_TEXT;
  case CT_WAIT:
    return IDK_CURSOR_WAIT;
  case CT_HELP:
    return IDK_CURSOR_HELP;
  case CT_EASTRESIZE:
    return IDK_CURSOR_E_RESIZE;
  case CT_NORTHRESIZE:
    return IDK_CURSOR_N_RESIZE;
  case CT_NORTHEASTRESIZE:
    return IDK_CURSOR_NE_RESIZE;
  case CT_NORTHWESTRESIZE:
    return IDK_CURSOR_NW_RESIZE;
  case CT_SOUTHRESIZE:
    return IDK_CURSOR_S_RESIZE;
  case CT_SOUTHEASTRESIZE:
    return IDK_CURSOR_SE_RESIZE;
  case CT_SOUTHWESTRESIZE:
    return IDK_CURSOR_SW_RESIZE;
  case CT_WESTRESIZE:
    return IDK_CURSOR_W_RESIZE;
  case CT_NORTHSOUTHRESIZE:
    return IDK_CURSOR_NS_RESIZE;
  case CT_EASTWESTRESIZE:
    return IDK_CURSOR_EW_RESIZE;
  case CT_NORTHEASTSOUTHWESTRESIZE:
    return IDK_CURSOR_NESW_RESIZE;
  case CT_NORTHWESTSOUTHEASTRESIZE:
    return IDK_CURSOR_NWSE_RESIZE;
  case CT_COLUMNRESIZE:
    return IDK_CURSOR_COL_RESIZE;
  case CT_ROWRESIZE:
    return IDK_CURSOR_ROW_RESIZE;
  case CT_MOVE:
  case CT_DND_MOVE:
    return IDK_CURSOR_MOVE;
  case CT_VERTICALTEXT:
    return IDK_CURSOR_VERTICAL_TEXT;
  case CT_CELL:
    return IDK_CURSOR_CELL;
  case CT_CONTEXTMENU:
    return IDK_CURSOR_CONTEXT_MENU;
  case CT_ALIAS:
  case CT_DND_LINK:
    return IDK_CURSOR_ALIAS;
  case CT_PROGRESS:
    return IDK_CURSOR_PROGRESS;
  case CT_NODROP:
  case CT_DND_NONE:
    return IDK_CURSOR_NO_DROP;
  case CT_COPY:
  case CT_DND_COPY:
    return IDK_CURSOR_COPY;
  case CT_NOTALLOWED:
    return IDK_CURSOR_NOT_ALLOWED;
  case CT_ZOOMIN:
    return IDK_CURSOR_ZOOM_IN;
  case CT_ZOOMOUT:
    return IDK_CURSOR_ZOOM_OUT;
  case CT_GRAB:
    return IDK_CURSOR_GRAB;
  case CT_GRABBING:
    return IDK_CURSOR_GRABBING;
  case CT_MIDDLEPANNING:
  case CT_EASTPANNING:
  case CT_NORTHPANNING:
  case CT_NORTHEASTPANNING:
  case CT_NORTHWESTPANNING:
  case CT_SOUTHPANNING:
  case CT_SOUTHEASTPANNING:
  case CT_SOUTHWESTPANNING:
  case CT_WESTPANNING:
  case CT_MIDDLE_PANNING_VERTICAL:
  case CT_MIDDLE_PANNING_HORIZONTAL:
    return IDK_CURSOR_ALL_SCROLL;
  default:
    return IDK_CURSOR_DEFAULT;
  }
}

bool View::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type, const CefCursorInfo &custom) {
  idk_cursor_update_t cursor = {};
  cursor.magic = IDK_CURSOR_MAGIC;
  cursor.version = IDK_CURSOR_VERSION;
  cursor.visible = type == CT_NONE ? 0 : 1;
  cursor.shape = cefCursorShape(type);
  cursor.scale = IDK_CURSOR_SCALE_BASE;
  std::vector<uint8_t> pixels;
  if (type == CT_CUSTOM && custom.buffer && custom.size.width > 0 && custom.size.height > 0 &&
      custom.size.width <= (int)IDK_CURSOR_MAX_DIM && custom.size.height <= (int)IDK_CURSOR_MAX_DIM) {
    cursor.shape = IDK_CURSOR_CUSTOM;
    cursor.width = (uint16_t)custom.size.width;
    cursor.height = (uint16_t)custom.size.height;
    cursor.hotspot_x = (int16_t)custom.hotspot.x;
    cursor.hotspot_y = (int16_t)custom.hotspot.y;
    float scale = custom.image_scale_factor > 0.0f ? custom.image_scale_factor : 1.0f;
    cursor.scale = (uint16_t)(scale * IDK_CURSOR_SCALE_BASE + 0.5f);
    cursor.data_size = (uint32_t)cursor.width * cursor.height * 4u;
    const uint8_t *data = static_cast<const uint8_t *>(custom.buffer);
    pixels.assign(data, data + cursor.data_size);
  } else if (type == CT_CUSTOM) {
    cursor.shape = IDK_CURSOR_DEFAULT;
  }
  if (input_)
    input_->QueueCursor(cursor, std::move(pixels));
  return true;
}

void View::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &dirty_rects, const void *buffer, int width,
                   int height) {
  if (type != PET_VIEW || !buffer || !connected_ || !visible_)
    return;
  /* OnPaint supplies BGRA from the top-left; the existing SHM consumers
   * (both GL and Vulkan) expect the same RGBA row order as Qt glReadPixels. */
  std::vector<uint8_t> rgba;
  if (!idk_cef_shm::ToGlRgba((const uint8_t *)buffer, (uint32_t)width * 4,
                              width, height, idk_cef_shm::Channels::BGRA, rgba))
    return;
  if (pending_ || !request_credit_ || deferred_kind_ != DeferredKind::None) {
    CacheDeferredPixels(rgba.data(), width, height);
    dropped_since_send_ = true;
    needs_full_update_ = true;
    if (request_credit_ && !pending_) MaybeSendDeferred();
    return;
  }
  SendShmPixels(rgba.data(), width, height, &dirty_rects);
  if (pending_) {
    request_credit_ = false;
    needs_full_update_ = true; /* Wait for consumer ACK before using the baseline. */
    capture_counter_valid_ = false;
    dropped_since_send_ = false;
  }
}

void View::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &dirty_rects,
                              const CefAcceleratedPaintInfo &info) {
  if (type != PET_VIEW)
    return;
  if (!connected_ || !visible_)
    return;
  if (pending_ || !request_credit_ || deferred_kind_ != DeferredKind::None) {
    CacheDeferredGpu(info);
    dropped_since_send_ = true;
    needs_full_update_ = true;
    capture_counter_valid_ = false;
    if (request_credit_ && !pending_) MaybeSendDeferred();
    return;
  }

  const auto &extra = info.extra;
  if (!needs_full_update_ && extra.has_capture_update_rect &&
      (extra.capture_update_rect.width <= 0 || extra.capture_update_rect.height <= 0)) {
    rate_unchanged_++;
    return;
  }
  bool can_blit_dirty = !needs_full_update_ && extra.has_capture_counter && capture_counter_valid_ &&
                        extra.capture_counter == last_capture_counter_ + 1 && extra.has_capture_update_rect &&
                        extra.visible_rect.x == 0 && extra.visible_rect.y == 0 &&
                        extra.coded_size.width == extra.visible_rect.width &&
                        extra.coded_size.height == extra.visible_rect.height;
  if (use_dmabuf_ && !dmabuf_failed_)
    SendFrameDmaBuf(info, dirty_rects, !can_blit_dirty);
  else
    SendShmFromDmaBuf(info, dirty_rects);
  if (pending_) {
    request_credit_ = false;
    needs_full_update_ = true; /* Re-enable partials only after consumer ACK. */
    dropped_since_send_ = false;
    capture_counter_valid_ = extra.has_capture_counter;
    if (capture_counter_valid_)
      last_capture_counter_ = extra.capture_counter;
  } else {
    needs_full_update_ = true;
  }
}

/* ── Frame paths ───────────────────────────────────────────────────── */

void View::SendFrameDmaBuf(const CefAcceleratedPaintInfo &info, const RectList &dirty_rects, bool full_update) {
  staged_send_ = false;
  int w = info.extra.visible_rect.width;
  int h = info.extra.visible_rect.height;
  if (w <= 0 || h <= 0)
    return;

  /* Preferred: blit CEF's (linear) buffer into our driver-native staging
   * buffer — the compositor imports with the driver's default tiled
   * layout, so a linear buffer would be misread (see gpubuf.h). */
  uint32_t stride = 0, fourcc = 0;
  uint64_t modifier = 0;
  uint16_t buf_id = 0;
  GpuBuf &target = gpu_inflight_ == &gpu_ ? deferred_gpu_ : gpu_;
  if (&target == gpu_last_acked_)
    gpu_last_acked_ = nullptr;
  if (target.Init() && target.BlitAndExport(info, w, h, dirty_rects, full_update,
                                           &stride, &fourcc, &modifier, &buf_id) == 0) {
    int fd = dup(target.Fd());
    if (fd < 0)
      return;
    idk_frame_header_t hdr = {};
    hdr.modifier = modifier;
    hdr.width = w;
    hdr.height = h;
    hdr.stride = stride;
    hdr.fourcc = fourcc;
    hdr.flags = IDK_FRAME_FLAG_VISIBLE;
    hdr.nfd = 1;
    hdr.buf_id = buf_id;
    int rc = idk_producer_send_dma_buf(&fd, &hdr);
    close(fd);
    if (rc == 0) {
      pending_ = true;
      rate_sent_++;
      staged_send_ = true;
      last_sent_shm_ = false;
      gpu_inflight_ = &target;
      sent_w_ = w; sent_h_ = h;
      if (target.LastBlitPartial())
        rate_partial_++;
      else
        rate_full_++;
    }
    return;
  }
  /* Fallback: forward CEF's own pooled buffer (dup). Only correct when the
   * compositor imports linear buffers as-is. */
  static bool s_warned = false;
  if (!s_warned) {
    IDK_LOG("webview-cef", "staging blit unavailable - forwarding CEF buffer directly\n");
    s_warned = true;
  }
  if (info.plane_count < 1 || info.plane_count > 4)
    return;
  int fds[4] = {-1, -1, -1, -1};
  for (int i = 0; i < info.plane_count; i++)
    fds[i] = dup(info.planes[i].fd);
  for (int i = 0; i < info.plane_count; i++) {
    if (fds[i] < 0) {
      for (int j = 0; j < info.plane_count; j++) {
        if (fds[j] >= 0)
          close(fds[j]);
      }
      return;
    }
  }

  idk_frame_header_t hdr = {};
  hdr.modifier = info.modifier;
  hdr.width = w;
  hdr.height = h;
  hdr.stride = info.planes[0].stride;
  hdr.fourcc = info.format == CEF_COLOR_TYPE_RGBA_8888 ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_ARGB8888;
  hdr.flags = IDK_FRAME_FLAG_VISIBLE; /* DMABUF bit set by send_dma_buf */
  hdr.nfd = info.plane_count;
  hdr.buf_id = 0;

  int rc = idk_producer_send_dma_buf(fds, &hdr);
  for (int i = 0; i < info.plane_count; i++)
    close(fds[i]);
  if (rc == 0) {
    pending_ = true;
    last_sent_shm_ = false;
    sent_w_ = w; sent_h_ = h;
    rate_sent_++;
  }
}

void View::SendShmFromDmaBuf(const CefAcceleratedPaintInfo &info, const RectList &dirty_rects) {
  if (info.plane_count < 1)
    return;
  const auto &p = info.planes[0];
  int w = info.extra.visible_rect.width;
  int h = info.extra.visible_rect.height;
  if (w <= 0 || h <= 0 || p.stride < (uint64_t)w * 4 ||
      (uint64_t)p.stride * h > p.size)
    return;

  if (info.modifier != 0) {
    /* Tiled buffer + SHM fallback needs a GPU readback; not supported. */
    IDK_LOG("webview-cef", "tiled dmabuf rejected and no readback - frame dropped\n");
    return;
  }

  void *map = mmap(nullptr, p.size, PROT_READ, MAP_SHARED, p.fd, 0);
  if (map == MAP_FAILED)
    return;

  /* CEF's native-pixmap rows have top-left origin; pixel channels are
   * declared by info.format. Normalize before SHM transport (including ROI). */
  std::vector<uint8_t> rgba;
  bool ok = idk_cef_shm::ToGlRgba((const uint8_t *)map, p.stride, w, h,
               info.format == CEF_COLOR_TYPE_RGBA_8888 ? idk_cef_shm::Channels::RGBA
                                                      : idk_cef_shm::Channels::BGRA, rgba);
  munmap(map, p.size);
  if (ok)
    SendShmPixels(rgba.data(), w, h, &dirty_rects);
}

/* The wire carries exactly one bounding rectangle; multiple CEF dirty rects
 * are merged only if their bounding box costs less than half a full frame.
 * Rect coordinates are top-left and match CEF SHM pixel row order. */
static bool choose_shm_roi(const View::RectList &dirty, int w, int h, idk_shm_delta_t &roi) {
  if (dirty.empty() || dirty.size() > 32 || w <= 0 || h <= 0)
    return false;
  int64_t x0 = w, y0 = h, x1 = 0, y1 = 0;
  for (const auto &r : dirty) {
    if (r.width <= 0 || r.height <= 0)
      return false;
    int64_t left = std::max<int64_t>(0, r.x);
    int64_t top = std::max<int64_t>(0, r.y);
    int64_t right = std::min<int64_t>(w, (int64_t)r.x + r.width);
    int64_t bottom = std::min<int64_t>(h, (int64_t)r.y + r.height);
    if (left >= right || top >= bottom)
      return false;
    x0 = std::min(x0, left); y0 = std::min(y0, top);
    x1 = std::max(x1, right); y1 = std::max(y1, bottom);
  }
  int64_t area = (x1 - x0) * (y1 - y0);
  if (area * 2 >= (int64_t)w * h)
    return false;
  /* The packed RGBA buffer is in GL bottom-left row order. */
  roi = {IDK_SHM_DELTA_MAGIC, (uint32_t)x0,
         (uint32_t)idk_cef_shm::ToGlY((int)y0, (int)(y1 - y0), h),
         (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
  return true;
}

void View::SendShmPixels(const uint8_t *px, int w, int h, const RectList *dirty_rects,
                         const idk_shm_delta_t *verified_roi) {
  if (w <= 0 || h <= 0 || (uint64_t)w * h * 4 > SIZE_MAX)
    return;
  /* Never send an ROI until the consumer has acknowledged the full baseline.
   * On the next callback after a dropped frame we deliberately send full. */
  idk_shm_delta_t roi{};
  bool partial = false;
  if (shm_roi_enabled_ && shm_baseline_valid_) {
    if (verified_roi && idk_shm_delta_validate(verified_roi, w, h,
                                               sizeof(*verified_roi) + (size_t)verified_roi->width * verified_roi->height * 4)) {
      roi = *verified_roi;
      partial = true;
    } else if (!needs_full_update_ && dirty_rects) {
      partial = choose_shm_roi(*dirty_rects, w, h, roi);
    }
  }
  size_t payload = partial ? (size_t)roi.width * roi.height * 4 + sizeof(roi) : (size_t)w * h * 4;
  std::vector<uint8_t> packed(payload);
  if (partial) {
    if (!idk_shm_delta_pack(packed.data(), packed.size(), px, (uint32_t)w, (uint32_t)h, &roi))
      return;
  } else {
    memcpy(packed.data(), px, payload);
  }
  const uint8_t *ring_data = packed.data();
  size_t ring_payload = payload;
  bool ring_partial = partial;
  idk_shm_delta_t ring_roi = roi;
  std::vector<uint8_t> ring_full;
  if (ring_partial && shm_ring_bootstrapped_ && shm_ring_ready_ && shm_ring_last_full_frame_id_ == 0) {
    ring_full.assign(px, px + (size_t)w * h * 4);
    ring_data = ring_full.data();
    ring_payload = ring_full.size();
    ring_partial = false;
  }
  bool ring_sent = SendShmRingFrame(ring_data, ring_payload, w, h, ring_partial, ring_partial ? &ring_roi : nullptr);
  bool sent = ring_sent;

  int fd = -1;
  if (!sent) {
    if (shm_ring_ready_)
      shm_ring_last_full_frame_id_ = 0;
    fd = memfd_create("idk-webview-cef", MFD_CLOEXEC);
    if (fd < 0)
      return;
    if (ftruncate(fd, (off_t)payload) < 0) {
      close(fd);
      return;
    }
    void *map = mmap(nullptr, payload, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
      close(fd);
      return;
    }
    memcpy(map, packed.data(), payload);
    munmap(map, payload);
    idk_frame_header_t hdr = {};
    hdr.width = w;
    hdr.height = h;
    hdr.flags = IDK_FRAME_FLAG_VISIBLE | (partial ? IDK_FRAME_FLAG_SHM_RECT : 0);
    hdr.nfd = 1;
    sent = idk_producer_send_frame(fd, &hdr) == 0;
    close(fd);
  }
  if (sent) {
    if (partial && shm_shadow_w_ == w && shm_shadow_h_ == h && shm_shadow_.size() == (size_t)w * h * 4) {
      for (uint32_t row = 0; row < roi.height; ++row)
        memcpy(shm_shadow_.data() + ((size_t)(roi.y + row) * w + roi.x) * 4,
               px + ((size_t)(roi.y + row) * w + roi.x) * 4, (size_t)roi.width * 4);
    } else {
      shm_shadow_.assign(px, px + (size_t)w * h * 4);
    }
    shm_shadow_w_ = w;
    shm_shadow_h_ = h;
    pending_ = true;
    request_credit_ = false;
    last_sent_shm_ = true;
    sent_w_ = w;
    sent_h_ = h;
    rate_sent_++;
    bytes_shm_sent_ += ring_sent ? ring_payload : payload;
    if (ring_sent ? ring_partial : partial)
      rate_shm_roi_++;
  }
}

/* Capture a paint that arrived while the game was processing an older
 * frame. The CEF callback buffer expires at return; own a full snapshot so
 * the last update of a static page cannot vanish. Retain only the newest. */
void View::CacheDeferredPixels(const uint8_t *pixels, int w, int h) {
  if (!pixels || w <= 0 || h <= 0 || (uint64_t)w * h * 4 > 256ull * 1024 * 1024)
    return;
  deferred_pixels_.assign(pixels, pixels + (size_t)w * h * 4);
  deferred_w_ = w;
  deferred_h_ = h;
  deferred_gpu_ptr_ = nullptr;
  deferred_kind_ = DeferredKind::Pixels;
  capture_counter_valid_ = false;
}

void View::CacheDeferredGpu(const CefAcceleratedPaintInfo &info) {
  int w = info.extra.visible_rect.width, h = info.extra.visible_rect.height;
  if (w <= 0 || h <= 0)
    return;
  /* Select the staging BO not currently referenced by an in-flight IPC frame.
   * If one deferred snapshot already exists, it is safe to replace it here. */
  GpuBuf &target = gpu_inflight_ == &gpu_ ? deferred_gpu_ : gpu_;
  uint32_t stride = 0, fourcc = 0;
  uint64_t modifier = 0;
  uint16_t id = 0;
  if (&target == gpu_last_acked_)
    gpu_last_acked_ = nullptr;
  if (target.Init() && target.BlitAndExport(info, w, h, {}, true, &stride, &fourcc, &modifier, &id) == 0) {
    deferred_gpu_ptr_ = &target;
    deferred_w_ = w;
    deferred_h_ = h;
    deferred_pixels_.clear();
    deferred_kind_ = DeferredKind::Gpu;
    capture_counter_valid_ = false;
    return;
  }
  /* If GPU staging is unavailable, retain a CPU copy for linear buffers. */
  if (info.plane_count < 1 || info.modifier != 0)
    return;
  const auto &plane = info.planes[0];
  if (plane.stride < (uint32_t)w * 4 || (uint64_t)plane.stride * h > plane.size ||
      (uint64_t)w * h * 4 > 256ull * 1024 * 1024)
    return;
  void *map = mmap(nullptr, plane.size, PROT_READ, MAP_SHARED, plane.fd, 0);
  if (map == MAP_FAILED)
    return;
  bool converted = idk_cef_shm::ToGlRgba((const uint8_t *)map, plane.stride, w, h,
                     info.format == CEF_COLOR_TYPE_RGBA_8888 ? idk_cef_shm::Channels::RGBA
                                                            : idk_cef_shm::Channels::BGRA,
                     deferred_pixels_);
  munmap(map, plane.size);
  if (!converted)
    return;
  deferred_w_ = w;
  deferred_h_ = h;
  deferred_gpu_ptr_ = nullptr;
  deferred_kind_ = DeferredKind::Pixels;
  capture_counter_valid_ = false;
}

bool View::SendOwnedGpu(GpuBuf &buffer, int w, int h) {
  /* GPU staging exposes properties through the same metadata cached at blit
   * time, so no CEF buffer handle needs to outlive its callback. */
  if (buffer.Fd() < 0)
    return false;
  int fd = dup(buffer.Fd());
  if (fd < 0)
    return false;
  idk_frame_header_t hdr{};
  hdr.width = w;
  hdr.height = h;
  hdr.stride = buffer.Stride();
  hdr.fourcc = buffer.Fourcc();
  hdr.modifier = buffer.Modifier();
  hdr.flags = IDK_FRAME_FLAG_VISIBLE;
  hdr.nfd = 1;
  hdr.buf_id = buffer.BufferId();
  int rc = idk_producer_send_dma_buf(&fd, &hdr);
  close(fd);
  if (rc != 0)
    return false;
  pending_ = true;
  last_sent_shm_ = false;
  staged_send_ = true;
  gpu_inflight_ = &buffer;
  sent_w_ = w; sent_h_ = h;
  ++rate_sent_;
  ++rate_deferred_;
  return true;
}

void View::MaybeSendDeferred() {
  if (!request_credit_ || pending_ || !connected_ || !visible_ || deferred_kind_ == DeferredKind::None)
    return;
  bool sent = false;
  if (deferred_kind_ == DeferredKind::Gpu && deferred_gpu_ptr_) {
    if (!use_dmabuf_ || dmabuf_failed_) {
      /* Slow one-time readback for SHM-only or rejected static frames; no Invalidate. */
      if (deferred_gpu_ptr_->ReadPixels(deferred_pixels_)) {
        deferred_kind_ = DeferredKind::Pixels;
        deferred_gpu_ptr_ = nullptr;
      }
    }
    if (deferred_kind_ == DeferredKind::Gpu && use_dmabuf_ && !dmabuf_failed_)
      sent = SendOwnedGpu(*deferred_gpu_ptr_, deferred_w_, deferred_h_);
    /* A rejected DMA-BUF can be retried from the owned staging BO without
     * a fresh Chromium capture. SHM conversion needs an explicit readback. */
  }
  if (!sent && deferred_kind_ == DeferredKind::Pixels && !deferred_pixels_.empty()) {
    idk_shm_delta_t delta{};
    const idk_shm_delta_t *verified_roi = nullptr;
    if (shm_roi_enabled_ && shm_baseline_valid_ &&
        deferred_w_ == shm_shadow_w_ && deferred_h_ == shm_shadow_h_ &&
        deferred_pixels_.size() == shm_shadow_.size()) {
      auto result = idk_cef_shm::FindDelta(deferred_pixels_.data(), shm_shadow_.data(),
                                            deferred_w_, deferred_h_, delta);
      if (result == idk_cef_shm::DeltaResult::Unchanged) {
        deferred_kind_ = DeferredKind::None;
        deferred_pixels_.clear();
        request_credit_ = false;
        needs_full_update_ = false;
        dropped_since_send_ = false;
        ++rate_unchanged_;
        return;
      }
      if (result == idk_cef_shm::DeltaResult::Partial)
        verified_roi = &delta;
    }
    SendShmPixels(deferred_pixels_.data(), deferred_w_, deferred_h_, nullptr, verified_roi);
    sent = pending_;
    if (sent) ++rate_deferred_;
  }
  if (sent) {
    deferred_kind_ = DeferredKind::None;
    deferred_pixels_.clear();
    deferred_gpu_ptr_ = nullptr;
    request_credit_ = false;
    needs_full_update_ = true;
    dropped_since_send_ = false;
  }
}

/* ── Producer connect + pacing ─────────────────────────────────────── */

bool View::BootstrapShmRing() {
  if (shm_ring_bootstrapped_ || !connected_)
    return shm_ring_bootstrapped_;
  const char *socket_mode = std::getenv("IDK_TP_SOCKET");
  if (!socket_mode || std::strcmp(socket_mode, "seqpacket") != 0)
    return false;
  uint64_t generation = idk_producer_session_generation();
  constexpr uint32_t slot_count = IDK_SHM_RING_MAX_SLOTS;
  constexpr size_t slot_capacity = 16u * 1024u * 1024u;
  if (generation == 0)
    return false;
  shm_ring_ = std::make_unique<ShmRing>();
  if (!shm_ring_->Init(generation, slot_count, slot_capacity, slot_capacity * slot_count)) {
    shm_ring_.reset();
    return false;
  }
  idk_shm_ring_create_t record = {
      .magic = IDK_SHM_RING_MAGIC,
      .version = IDK_SHM_RING_VERSION,
      .slot_count = slot_count,
      .slot_capacity = static_cast<uint32_t>(slot_capacity),
      .ring_bytes = slot_capacity * slot_count,
      .session_generation = generation,
      .width = static_cast<uint32_t>(render_w_),
      .height = static_cast<uint32_t>(render_h_),
      .stride = static_cast<uint32_t>(render_w_ > 0 ? render_w_ * 4 : 0),
  };
  int fd = shm_ring_->fd();
  if (idk_producer_send_control(IDK_SOCKET_V2_RING_CREATE, &record, sizeof(record), &fd, 1) < 0) {
    shm_ring_.reset();
    return false;
  }
  shm_ring_frame_id_ = 0;
  shm_ring_last_full_frame_id_ = 0;
  shm_ring_inflight_frame_id_ = 0;
  shm_ring_inflight_full_ = false;
  shm_ring_ready_ = false;
  shm_ring_bootstrapped_ = true;
  return true;
}

void View::ResetShmRing() {
  shm_ring_bootstrapped_ = false;
  shm_ring_ready_ = false;
  shm_ring_frame_id_ = 0;
  shm_ring_last_full_frame_id_ = 0;
  shm_ring_inflight_frame_id_ = 0;
  shm_ring_inflight_full_ = false;
  shm_ring_.reset();
}

void View::DrainShmRingReleases() {
  if (!shm_ring_bootstrapped_ || !shm_ring_ || !idk_producer_is_connected())
    return;
  for (;;) {
    uint16_t type = 0;
    int peek = idk_producer_peek_type(&type);
    if (peek <= 0)
      return;
    if (type == IDK_SOCKET_V2_RING_READY) {
      idk_shm_ring_ready_t ready = {};
      if (idk_producer_recv_control(IDK_SOCKET_V2_RING_READY, &ready, sizeof(ready)) != 1 ||
          ready.session_generation != shm_ring_->generation() ||
          ready.slot_count != shm_ring_->slot_count()) {
        ResetShmRing();
        return;
      }
      shm_ring_ready_ = true;
      continue;
    }
    if (type != IDK_SOCKET_V2_SLOT_RELEASE)
      return;
    idk_shm_ring_slot_release_t release = {};
    if (idk_producer_recv_control(IDK_SOCKET_V2_SLOT_RELEASE, &release, sizeof(release)) != 1 ||
        !shm_ring_->Release(static_cast<int>(release.slot_index), release.session_generation, release.frame_id)) {
      ResetShmRing();
      return;
    }
  }
}

bool View::SendShmRingFrame(const uint8_t *payload, size_t payload_bytes, int w, int h, bool partial,
                            const idk_shm_delta_t *roi) {
  if (!shm_ring_bootstrapped_ || !shm_ring_ready_ || !shm_ring_ || !payload || payload_bytes == 0 ||
      payload_bytes > shm_ring_->slot_capacity()) {
    return false;
  }
  if (partial && shm_ring_last_full_frame_id_ == 0)
    return false;
  uint64_t frame_id = ++shm_ring_frame_id_;
  if (frame_id == 0)
    return false;
  int slot = shm_ring_->Acquire(frame_id, payload_bytes);
  if (slot < 0 || !shm_ring_->Write(slot, 0, payload, payload_bytes)) {
    if (slot >= 0)
      shm_ring_->Release(slot, shm_ring_->generation(), frame_id);
    return false;
  }
  idk_shm_ring_frame_slot_t record = {
      .session_generation = shm_ring_->generation(),
      .frame_id = frame_id,
      .base_frame_id = partial && roi ? shm_ring_last_full_frame_id_ : 0,
      .slot_index = static_cast<uint32_t>(slot),
      .offset = static_cast<uint32_t>(static_cast<size_t>(slot) * shm_ring_->slot_capacity()),
      .payload_bytes = static_cast<uint32_t>(payload_bytes),
      .width = static_cast<uint32_t>(w),
      .height = static_cast<uint32_t>(h),
      .stride = static_cast<uint32_t>(w * 4),
      .pixel_format = 0,
      .x = partial && roi ? roi->x : 0,
      .y = partial && roi ? roi->y : 0,
      .roi_width = partial && roi ? roi->width : 0,
      .roi_height = partial && roi ? roi->height : 0,
      .flags = static_cast<uint16_t>(partial ? IDK_SHM_RING_FRAME_ROI : IDK_SHM_RING_FRAME_FULL),
  };
  if (idk_producer_send_control(IDK_SOCKET_V2_FRAME_SLOT, &record, sizeof(record), nullptr, 0) < 0) {
    shm_ring_->Release(slot, shm_ring_->generation(), frame_id);
    return false;
  }
  shm_ring_inflight_frame_id_ = frame_id;
  shm_ring_inflight_full_ = !partial;
  return true;
}

/* ── Producer connect + pacing ─────────────────────────────────────── */

void View::ConnectTask() {
  if (quit_.load(std::memory_order_acquire))
    return;
  connect_task_scheduled_ = false;
  const bool now = idk_producer_is_connected();

  if (now && !connected_) {
    connected_ = true;
    connect_attempts_ = 0;
    deferred_kind_ = DeferredKind::None;
    gpu_inflight_ = nullptr;
    gpu_last_acked_ = nullptr;
    shm_shadow_.clear();
    needs_full_update_ = true;
    shm_baseline_valid_ = false;
    capture_counter_valid_ = false;
    IDK_LOG("webview-cef", "compositor connected\n");
    BootstrapShmRing();
    if (!input_) {
      input_ = new InputThread(this, sock_path_);
      input_->Start();
    } else {
      input_->NotifyTransportAvailable();
    }
  } else if (!now && connected_) {
    ResetShmRing();
    connected_ = false;
    deferred_kind_ = DeferredKind::None;
    deferred_pixels_.clear();
    gpu_inflight_ = nullptr;
    gpu_last_acked_ = nullptr;
    shm_shadow_.clear();
    shm_baseline_valid_ = false;
    pending_ = false;
    request_credit_ = false;
    IDK_LOG("webview-cef", "compositor disconnected - reconnecting\n");
  } else if (!now && !connected_) {
    connect_attempts_++;
    if (idk_producer_init(SockName().c_str()) == 0 && idk_producer_is_connected()) {
      int attempts = connect_attempts_;
      connected_ = true;
      connect_attempts_ = 0;
      deferred_kind_ = DeferredKind::None;
      gpu_inflight_ = nullptr;
      gpu_last_acked_ = nullptr;
      shm_shadow_.clear();
      needs_full_update_ = true;
      capture_counter_valid_ = false;
      IDK_LOG("webview-cef", "compositor connected after %d attempts\n", attempts);
      BootstrapShmRing();
      if (!input_) {
        input_ = new InputThread(this, sock_path_);
        input_->Start();
      } else {
        input_->NotifyTransportAvailable();
      }
    } else {
      bool log_it = connect_attempts_ == 1 || connect_attempts_ == 5 || connect_attempts_ == 30 ||
                    (connect_attempts_ > 30 && connect_attempts_ % 60 == 0);
      if (log_it)
        IDK_LOG("webview-cef", "waiting for compositor (attempt %d)\n", connect_attempts_);
    }
  }

  const char *stats_env = std::getenv("IDK_CEF_STATS");
  if (stats_env && stats_env[0] == '1') {
    if (connected_)
      IDK_LOG("webview-cef", "rate: sent=%d req=%d ack=%d unchanged=%d gpu_partial=%d gpu_full=%d shm_roi=%d shm_bytes=%llu deferred=%d\n",
              rate_sent_, rate_req_, rate_ack_, rate_unchanged_, rate_partial_, rate_full_,
              rate_shm_roi_, (unsigned long long)bytes_shm_sent_, rate_deferred_);
    rate_sent_ = rate_req_ = rate_ack_ = rate_unchanged_ = rate_partial_ = rate_full_ = rate_shm_roi_ = 0;
    bytes_shm_sent_ = 0;
    rate_deferred_ = 0;
  }

  if (!connected_) {
    connect_task_scheduled_ = true;
    PostToUIDelayed([self = CefRefPtr<View>(this)] {
      if (!self->quit_.load(std::memory_order_acquire))
        self->ConnectTask();
    }, 1000);
  }
}

/* Chromium may complete a BeginFrame without producing any paint callback.
 * A REQUEST therefore grants permission to submit one frame; it must not
 * create a pending capture or require a corresponding OnPaint to finish. */
void View::PollSocket() {
  if (!connected_)
    return;
  DrainShmRingReleases();

  if (pending_) {
    idk_ack_msg_t ack;
    if (idk_producer_wait_ack(&ack, 0) == 0) {
      ++rate_ack_;
      ProcessAck(ack);
    }
  }

  // Always process SHUTDOWN, even when a frame is waiting for game ACK.
  // While pending, the socket path must not consume the incomplete/late ACK.
  bool requested = false;
  idk_request_msg_t req;
  for (int i = 0; i < 256; ++i) {
    int rc = pending_ ? idk_producer_recv_request_preserving_ack(&req, 0)
                      : idk_producer_recv_request(&req, 0);
    if (rc != 0)
      break;
    if (req.type == IDK_REQUEST_SHUTDOWN) {
      quit_.store(true);
      return;
    }
    if (req.type == IDK_REQUEST_NEXT_FRAME) {
      ++rate_req_;
      requested = true;
    }
  }
  if (!idk_producer_is_connected()) {
    connected_ = false;
    deferred_kind_ = DeferredKind::None;
    deferred_pixels_.clear();
    gpu_inflight_ = nullptr;
    gpu_last_acked_ = nullptr;
    shm_shadow_.clear();
    shm_baseline_valid_ = false;
    pending_ = false;
    request_credit_ = false;
    if (!connect_task_scheduled_) {
      connect_task_scheduled_ = true;
      PostToUIDelayed([self = CefRefPtr<View>(this)] {
        if (!self->quit_.load(std::memory_order_acquire))
          self->ConnectTask();
      }, 0);
    }
    return;
  }
  if (!visible_)
    return;
  if (requested)
    request_credit_ = true;
  if (!pending_ && request_credit_ && page_ready_) {
    MaybeSendDeferred();
    if (!pending_ && deferred_kind_ == DeferredKind::None)
      NudgeFrame();
  }
}

void View::NudgeFrame() {
  if (browser_ && connected_ && visible_ && !pending_ && page_ready_)
    browser_->GetHost()->SendExternalBeginFrame();
}

void View::ProcessAck(const idk_ack_msg_t &ack) {
  const bool accepted = ack.ack == IDK_ACK_ACCEPTED;
  if (shm_ring_inflight_frame_id_ != 0) {
    if (accepted && shm_ring_inflight_full_)
      shm_ring_last_full_frame_id_ = shm_ring_inflight_frame_id_;
    else if (!accepted && shm_ring_inflight_full_)
      shm_ring_last_full_frame_id_ = 0;
    shm_ring_inflight_frame_id_ = 0;
    shm_ring_inflight_full_ = false;
  }
  if (accepted) {
    if (!last_sent_shm_ && gpu_inflight_) {
      gpu_last_acked_ = gpu_inflight_;
      gpu_last_w_ = sent_w_;
      gpu_last_h_ = sent_h_;
    } else if (last_sent_shm_) {
      gpu_last_acked_ = nullptr;
    }
  }
  if (accepted && !visible_ && deferred_kind_ == DeferredKind::None) {
    if (last_sent_shm_ && !shm_shadow_.empty()) {
      deferred_pixels_ = shm_shadow_;
      deferred_w_ = shm_shadow_w_;
      deferred_h_ = shm_shadow_h_;
      deferred_kind_ = DeferredKind::Pixels;
    } else if (gpu_last_acked_) {
      deferred_gpu_ptr_ = gpu_last_acked_;
      deferred_w_ = gpu_last_w_;
      deferred_h_ = gpu_last_h_;
      deferred_kind_ = DeferredKind::Gpu;
    }
  }
  shm_baseline_valid_ = accepted && last_sent_shm_ && visible_;
  if (accepted && visible_ && !dropped_since_send_ && deferred_kind_ == DeferredKind::None)
    needs_full_update_ = false;
  if (!accepted) {
    needs_full_update_ = true;
    capture_counter_valid_ = false;
    if (ack.ack == IDK_ACK_REJECTED && use_dmabuf_ && !dmabuf_failed_) {
      ++dmabuf_reject_count_;
      if (dmabuf_reject_count_ >= DMABUF_REJECT_LIMIT) {
        dmabuf_failed_ = true;
        IDK_LOG("webview-cef", "dmabuf rejected %d times - falling back to SHM\n", dmabuf_reject_count_);
      }
    }
    /* Do not replace a newer deferred paint with this older rejected frame. */
    if (last_sent_shm_ && deferred_kind_ == DeferredKind::None && !shm_shadow_.empty()) {
      deferred_pixels_ = shm_shadow_;
      deferred_w_ = shm_shadow_w_;
      deferred_h_ = shm_shadow_h_;
      deferred_gpu_ptr_ = nullptr;
      deferred_kind_ = DeferredKind::Pixels;
    }
    if (gpu_inflight_ && deferred_kind_ == DeferredKind::None) {
      deferred_w_ = sent_w_;
      deferred_h_ = sent_h_;
      if (dmabuf_failed_ && gpu_inflight_->ReadPixels(deferred_pixels_)) {
        deferred_kind_ = DeferredKind::Pixels;
        deferred_gpu_ptr_ = nullptr;
      } else {
        deferred_kind_ = DeferredKind::Gpu;
        deferred_gpu_ptr_ = gpu_inflight_;
      }
    }
  }
  gpu_inflight_ = nullptr;
  pending_ = false;

  if (ack.w > 0 && ack.h > 0 && (ack.w != render_w_ || ack.h != render_h_)) {
    render_w_ = ack.w;
    render_h_ = ack.h;
    needs_full_update_ = true;
    shm_baseline_valid_ = false;
    capture_counter_valid_ = false;
    deferred_kind_ = DeferredKind::None;
    deferred_pixels_.clear();
    deferred_gpu_ptr_ = nullptr;
    shm_shadow_.clear();
    gpu_last_acked_ = nullptr;
    dmabuf_failed_ = false;
    dmabuf_reject_count_ = 0;
    IDK_LOG("webview-cef", "game resize: %dx%d\n", render_w_, render_h_);
    if (browser_) {
      browser_->GetHost()->WasResized();
      browser_->GetHost()->NotifyMoveOrResizeStarted();
    }
  }
}

/* ── Input (UI-thread half) ────────────────────────────────────────── */

void View::SendKeyEventUI(const CefKeyEvent &ev) {
  if (browser_)
    browser_->GetHost()->SendKeyEvent(ev);
}

void View::SendMouseMoveUI(int x, int y, int mods) {
  if (!browser_)
    return;
  CefMouseEvent me;
  me.x = x;
  me.y = y;
  me.modifiers = mods;
  browser_->GetHost()->SendMouseMoveEvent(me, false);
}

void View::SendMouseClickUI(int x, int y, int mods, int btn, bool mouse_up) {
  if (!browser_ || btn < 0)
    return;
  CefMouseEvent me;
  me.x = x;
  me.y = y;
  me.modifiers = mods;
  browser_->GetHost()->SendMouseClickEvent(me, (cef_mouse_button_type_t)btn, mouse_up, 1);
}

void View::SendMouseWheelUI(int x, int y, int mods, int dx, int dy) {
  if (!browser_)
    return;
  CefMouseEvent me;
  me.x = x;
  me.y = y;
  me.modifiers = mods;
  browser_->GetHost()->SendMouseWheelEvent(me, dx, dy);
}

void View::SetCapture(bool on) {
  capture_ = on;
  if (browser_)
    browser_->GetHost()->SetFocus(on);
  FireCaptureEvents(on);
}

void View::SetOverlayVisible(bool v) {
  if (visible_ == v)
    return;
  visible_ = v;
  request_credit_ = false;
  if (!v && deferred_kind_ == DeferredKind::None && pending_ && gpu_inflight_) {
    deferred_gpu_ptr_ = gpu_inflight_;
    deferred_w_ = sent_w_;
    deferred_h_ = sent_h_;
    deferred_kind_ = DeferredKind::Gpu;
  } else if (!v && deferred_kind_ == DeferredKind::None && !shm_shadow_.empty() && last_sent_shm_) {
    deferred_pixels_ = shm_shadow_;
    deferred_w_ = shm_shadow_w_;
    deferred_h_ = shm_shadow_h_;
    deferred_gpu_ptr_ = nullptr;
    deferred_kind_ = DeferredKind::Pixels;
  } else if (!v && deferred_kind_ == DeferredKind::None && gpu_last_acked_) {
    deferred_gpu_ptr_ = gpu_last_acked_;
    deferred_w_ = gpu_last_w_;
    deferred_h_ = gpu_last_h_;
    deferred_kind_ = DeferredKind::Gpu;
  }
  needs_full_update_ = true;
  shm_baseline_valid_ = false;
  capture_counter_valid_ = false;
  FireVisibleEvents(v);
}

/* ── JS events (mirror the Qt webview: window CustomEvents) ─────────── */

void View::FireJs(const std::string &events) {
  if (!browser_)
    return;
  std::string js =
      "(function(){var evts=[" + events + "];for(var i=0;i<evts.length;i++)window.dispatchEvent(evts[i]);})()";
  browser_->GetMainFrame()->ExecuteJavaScript(js, "", 0);
}

void View::FireCaptureEvents(bool captured) {
  std::string evts =
      std::string("new CustomEvent('overlaycapturechanged',{detail:{captured:") + (captured ? "true" : "false") + "}})";
  if (captured && !js_capture_)
    evts = "new CustomEvent('overlaycapturestart')," + evts;
  else if (!captured && js_capture_)
    evts = "new CustomEvent('overlaycaptureend')," + evts;
  js_capture_ = captured;
  FireJs(evts);
}

void View::FireVisibleEvents(bool visible) {
  std::string evts =
      std::string("new CustomEvent('overlayvisiblechanged',{detail:{visible:") + (visible ? "true" : "false") + "}})";
  if (visible && !js_visible_)
    evts = "new CustomEvent('overlayshow')," + evts;
  else if (!visible && js_visible_)
    evts = "new CustomEvent('overlayhide')," + evts;
  js_visible_ = visible;
  FireJs(evts);
}
