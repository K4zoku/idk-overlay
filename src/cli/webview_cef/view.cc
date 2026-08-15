#include "view.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "include/cef_app.h"
#include "input.h"
#include "public/idk_producer.h"
#include "tasks.h"

/* DRM fourcc codes (BGRA/RGBA 8888 memory order, matching the CEF formats). */
#define DRM_FORMAT_ARGB8888 0x34325241u /* CEF_COLOR_TYPE_BGRA_8888  */
#define DRM_FORMAT_ABGR8888 0x34324241u /* CEF_COLOR_TYPE_RGBA_8888  */

#define DMABUF_REJECT_LIMIT 5

View::View(GroupConfig conf, bool no_dmabuf, std::string sock_path, bool sock_abstract)
    : conf_(std::move(conf)), sock_path_(std::move(sock_path)), sock_abstract_(sock_abstract), no_dmabuf_(no_dmabuf),
      render_w_(conf_.width), render_h_(conf_.height) {
  use_dmabuf_ = !no_dmabuf_;
}

View::~View() { Stop(); }

/* Raw producer name for idk_tp_init (leading '\0' = abstract namespace). */
std::string View::SockName() const { return sock_abstract_ ? std::string(1, '\0') + sock_path_ : sock_path_; }

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void View::Start() { ConnectTask(); /* init producer + chained reconnect */ }

void View::Stop() {
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
  page_ready_ = true;
  InjectScripts();
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

void View::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &, const void *buffer, int width,
                   int height) {
  /* Software OSR (shared_texture off / GPU failure): pixels → SHM. */
  if (type != PET_VIEW || !buffer)
    return;
  render_pending_ = false;
  if (!connected_ || !visible_ || pending_)
    return;
  if (width <= 0 || height <= 0)
    return;
  SendShmPixels((const uint8_t *)buffer, width, height);
}

void View::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &,
                              const CefAcceleratedPaintInfo &info) {
  if (type != PET_VIEW)
    return; /* popups not composited (v1) */
  render_pending_ = false;
  if (!connected_ || !visible_ || pending_)
    return;
  if (use_dmabuf_ && !dmabuf_failed_)
    SendFrameDmaBuf(info);
  else
    SendShmFromDmaBuf(info);
}

/* ── Frame paths ───────────────────────────────────────────────────── */

void View::SendFrameDmaBuf(const CefAcceleratedPaintInfo &info) {
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
  if (gpu_.Init() && gpu_.BlitAndExport(info, w, h, &stride, &fourcc, &modifier, &buf_id) == 0) {
    int fd = dup(gpu_.Fd());
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
    if (idk_producer_send_dma_buf(&fd, &hdr) == 0) {
      pending_ = true;
      send_time_ms_ = now_ms();
      rate_sent_++;
    }
    return;
  }

  /* Fallback: forward CEF's own pooled buffer (dup — idk_tp_send closes
   * the fds it is handed). Only correct when the compositor imports
   * linear buffers as-is. */
  static bool s_warned = false;
  if (!s_warned) {
    IDK_LOG("webview-cef", "staging blit unavailable - forwarding CEF buffer directly\n");
    s_warned = true;
  }
  if (info.plane_count < 1 || info.plane_count > 4)
    return;
  int fds[4];
  for (int i = 0; i < info.plane_count; i++)
    fds[i] = dup(info.planes[i].fd);
  for (int i = 0; i < info.plane_count; i++) {
    if (fds[i] < 0) {
      for (int j = 0; j < info.plane_count; j++)
        if (fds[j] >= 0)
          close(fds[j]);
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

  if (idk_producer_send_dma_buf(fds, &hdr) == 0) {
    pending_ = true;
    send_time_ms_ = now_ms();
  }
}

void View::SendShmFromDmaBuf(const CefAcceleratedPaintInfo &info) {
  if (info.plane_count < 1)
    return;
  const auto &p = info.planes[0];
  int w = info.extra.visible_rect.width;
  int h = info.extra.visible_rect.height;
  if (w <= 0 || h <= 0)
    return;

  if (info.modifier != 0) {
    /* Tiled buffer + SHM fallback needs a GPU readback; not supported. */
    IDK_LOG("webview-cef", "tiled dmabuf rejected and no readback - frame dropped\n");
    return;
  }

  void *map = mmap(nullptr, p.size, PROT_READ, MAP_SHARED, p.fd, 0);
  if (map == MAP_FAILED)
    return;

  /* Strip the row padding; the compositor expects tightly packed BGRA. */
  std::vector<uint8_t> px((size_t)w * h * 4);
  for (int y = 0; y < h; y++)
    memcpy(px.data() + (size_t)y * w * 4, (const uint8_t *)map + (size_t)y * p.stride, (size_t)w * 4);
  munmap(map, p.size);

  SendShmPixels(px.data(), w, h);
}

void View::SendShmPixels(const uint8_t *px, int w, int h) {
  int fd = memfd_create("idk-webview-cef", MFD_CLOEXEC);
  if (fd < 0)
    return;
  size_t size = (size_t)w * h * 4;
  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return;
  }
  void *map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    close(fd);
    return;
  }
  memcpy(map, px, size);
  munmap(map, size);

  idk_frame_header_t hdr = {};
  hdr.width = w;
  hdr.height = h;
  hdr.stride = 0; /* SHM */
  hdr.fourcc = 0; /* SHM */
  hdr.flags = IDK_FRAME_FLAG_VISIBLE;
  hdr.nfd = 1;
  hdr.buf_id = 0;

  if (idk_producer_send_frame(fd, &hdr) == 0) {
    pending_ = true;
    send_time_ms_ = now_ms();
    rate_sent_++;
  }
  /* idk_tp_send closed fd on success; on failure it did too (io.c). */
}

/* ── Producer connect + pacing ─────────────────────────────────────── */

void View::ConnectTask() {
  bool now = idk_producer_is_connected();

  if (now && !connected_) {
    connected_ = true;
    connect_attempts_ = 0;
    IDK_LOG("webview-cef", "compositor connected\n");
    if (!input_) {
      input_ = new InputThread(this, sock_path_);
      input_->Start();
    }
  } else if (!now && connected_) {
    connected_ = false;
    IDK_LOG("webview-cef", "compositor disconnected - reconnecting\n");
  } else if (!now && !connected_) {
    connect_attempts_++;
    if (idk_producer_init(SockName().c_str()) == 0 && idk_producer_is_connected()) {
      int attempts = connect_attempts_;
      connected_ = true;
      connect_attempts_ = 0;
      IDK_LOG("webview-cef", "compositor connected after %d attempts\n", attempts);
      if (!input_) {
        input_ = new InputThread(this, sock_path_);
        input_->Start();
      }
    } else {
      bool log_it = connect_attempts_ == 1 || connect_attempts_ == 5 || connect_attempts_ == 30 ||
                    (connect_attempts_ > 30 && connect_attempts_ % 60 == 0);
      if (log_it)
        IDK_LOG("webview-cef", "waiting for compositor (attempt %d)\n", connect_attempts_);
    }
  }

  /* 1s diagnostics: game request/ack rate vs our send rate. */
  if (connected_)
    IDK_LOG("webview-cef", "rate: sent=%d req=%d ack=%d (last 1s)\n", rate_sent_, rate_req_, rate_ack_);
  rate_sent_ = rate_req_ = rate_ack_ = 0;

  PostToUIDelayed([self = CefRefPtr<View>(this)] { self->ConnectTask(); }, 1000);
}

/* ACK/REQUEST drain (called by the main loop when the socket is ready). */
void View::PollSocket() {
  if (!connected_ || !visible_)
    return;
  if (pending_) {
    idk_ack_msg_t ack;
    if (idk_producer_wait_ack(&ack, 0) == 0) {
      rate_ack_++;
      ProcessAck(ack);
    } else if ((now_ms() - send_time_ms_) > 100) {
      /* Lost ACK (compositor busy/restarted) — unlock like the Qt backend. */
      pending_ = false;
      IDK_LOG("webview-cef", "ACK timeout - force-unlock pending\n");
    }
  }
  if (!pending_) {
    idk_request_msg_t req;
    if (idk_producer_recv_request(&req, 0) == 0) {
      if (req.type == IDK_REQUEST_SHUTDOWN) {
        IDK_LOG("webview-cef", "compositor shutdown request\n");
        quit_.store(true);
        return;
      }
      rate_req_++;
      /* Game asks for the next frame → render exactly one. */
      if (!render_pending_)
        NudgeFrame();
    }
  }
}

/* Issue one external begin frame. SendExternalBeginFrame is ignored while
 * a previous begin frame is pending, so NudgeRetry keeps re-issuing until
 * a paint actually arrives (OnAcceleratedPaint clears render_pending_). */
void View::NudgeFrame() {
  if (!browser_ || !page_ready_)
    return;
  render_pending_ = true;
  browser_->GetHost()->Invalidate(PET_VIEW);
  browser_->GetHost()->SendExternalBeginFrame();
  PostToUIDelayed([self = CefRefPtr<View>(this)] { self->NudgeRetry(); }, 25);
}

void View::NudgeRetry() {
  if (quit_.load() || !render_pending_)
    return;
  if (browser_) {
    browser_->GetHost()->Invalidate(PET_VIEW);
    browser_->GetHost()->SendExternalBeginFrame();
  }
  PostToUIDelayed([self = CefRefPtr<View>(this)] { self->NudgeRetry(); }, 25);
}

void View::ProcessAck(const idk_ack_msg_t &ack) {
  if (ack.ack == 1 && use_dmabuf_ && !dmabuf_failed_) {
    dmabuf_reject_count_++;
    if (dmabuf_reject_count_ >= DMABUF_REJECT_LIMIT) {
      dmabuf_failed_ = true;
      IDK_LOG("webview-cef", "dmabuf rejected %d times - falling back to SHM\n", dmabuf_reject_count_);
    }
  }

  if (ack.w > 0 && ack.h > 0 && (ack.w != render_w_ || ack.h != render_h_)) {
    render_w_ = ack.w;
    render_h_ = ack.h;
    dmabuf_failed_ = false; /* retry dmabuf at the new size */
    dmabuf_reject_count_ = 0;
    IDK_LOG("webview-cef", "game resize: %dx%d\n", render_w_, render_h_);
    if (browser_) {
      browser_->GetHost()->WasResized();
      browser_->GetHost()->NotifyMoveOrResizeStarted();
    }
  }

  pending_ = false;
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
  FireVisibleEvents(v);
  if (v)
    NudgeFrame(); /* fresh frame after show */
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
