#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"

#include "config.h"
#include "gpubuf.h"
#include "public/idk_frames.h"
#include "public/idk_shm_delta.h"

class InputThread;

/* One windowless CEF browser feeding the idk compositor.
 *
 * Frame flow (all on the CEF UI thread):
 *   compositor REQUEST → external BeginFrame → (only on damage)
 *   OnAcceleratedPaint → send DMA-BUF or SHM → game ACK.
 *
 * The input socket lives on a dedicated thread (input.cc) that posts
 * CefKeyEvent/CefMouseEvent work here.
 */
class View : public CefClient,
             public CefRenderHandler,
             public CefLifeSpanHandler,
             public CefLoadHandler,
             public CefDisplayHandler {
public:
  /* |sock_path| is the producer socket path; |sock_abstract| marks an
   * abstract-namespace socket (IDK_TP_ABSTRACT, broker mode). */
  View(GroupConfig conf, bool no_dmabuf, std::string sock_path, bool sock_abstract);
  ~View();

  /* UI thread. Initializes the producer + starts the connect task. */
  void Start();
  /* UI thread. Stops the input thread (call before process exit). */
  void Stop();

  /* UI thread, called by the main loop whenever the producer socket is
   * readable (or on a timeout): drains ACK/REQUEST non-blocking. */
  void PollSocket();
  bool QuitRequested() const { return quit_.load(); }
  bool PageReady() const { return page_ready_.load(std::memory_order_acquire); }

  /* ── UI-thread entry points (input.cc posts these via tasks.h) ── */
  void SendKeyEventUI(const CefKeyEvent &ev);
  void SendMouseMoveUI(int x, int y, int mods);
  void SendMouseClickUI(int x, int y, int mods, int btn, bool mouse_up);
  void SendMouseWheelUI(int x, int y, int mods, int dx, int dy);
  void SetCapture(bool on);
  void SetOverlayVisible(bool v);

private:
  /* CefClient */
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  /* CefRenderHandler */
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) override;
  bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo &si) override;
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList &, const void *buffer, int width,
               int height) override;
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList &,
                          const CefAcceleratedPaintInfo &info) override;
  void OnPopupShow(CefRefPtr<CefBrowser>, bool) override {}
  void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect &) override {}

  /* CefLifeSpanHandler */
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser>) override { return false; }

  /* CefLoadHandler */
  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int) override;

  /* CefDisplayHandler */
  bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t, const CefCursorInfo &) override;

  /* Producer lifecycle + pacing (UI thread). */
  void ConnectTask();
  void ProcessAck(const idk_ack_msg_t &ack);
  void NudgeFrame();
  void MaybeSendDeferred();
  void CacheDeferredPixels(const uint8_t *pixels, int w, int h);
  void CacheDeferredGpu(const CefAcceleratedPaintInfo &info);
  bool SendOwnedGpu(GpuBuf &buffer, int w, int h);

  /* Frame paths. OnAcceleratedPaint delivers the fd only for the duration
   * of the callback, so each path copies/dups before returning. */
  void SendFrameDmaBuf(const CefAcceleratedPaintInfo &info, const RectList &dirty_rects, bool full_update);
  void SendShmFromDmaBuf(const CefAcceleratedPaintInfo &info, const RectList &dirty_rects);
  void SendShmPixels(const uint8_t *px, int w, int h, const RectList *dirty_rects = nullptr,
                     const idk_shm_delta_t *verified_roi = nullptr);

  void InjectScripts();
  void LoadUrl();
  std::string SockName() const;
  void FireJs(const std::string &events);
  void FireCaptureEvents(bool captured);
  void FireVisibleEvents(bool visible);

  GroupConfig conf_;
  CefRefPtr<CefBrowser> browser_;
  InputThread *input_ = nullptr;

  std::string sock_path_;
  bool sock_abstract_ = false;
  bool no_dmabuf_;
  bool connected_ = false;
  int connect_attempts_ = 0;
  std::atomic<bool> quit_{false};

  int render_w_;
  int render_h_;

  bool use_dmabuf_ = true;
  bool dmabuf_failed_ = false;
  int dmabuf_reject_count_ = 0;

  bool pending_ = false;        /* frame in flight, awaiting ACK */
  bool visible_ = true;         /* overlay visibility */
  bool request_credit_ = false; /* game has requested an updated frame */
  bool needs_full_update_ = true; /* no reliable staging/consumer baseline */
  bool capture_counter_valid_ = false;
  bool staged_send_ = false;
  enum class DeferredKind { None, Pixels, Gpu };
  DeferredKind deferred_kind_ = DeferredKind::None;
  std::vector<uint8_t> deferred_pixels_;
  int deferred_w_ = 0, deferred_h_ = 0;
  GpuBuf deferred_gpu_; /* second staging buffer: never write the in-flight buffer */
  GpuBuf *deferred_gpu_ptr_ = nullptr;
  GpuBuf *gpu_inflight_ = nullptr;
  GpuBuf *gpu_last_acked_ = nullptr;
  int gpu_last_w_ = 0, gpu_last_h_ = 0;
  bool dropped_since_send_ = false;
  int sent_w_ = 0, sent_h_ = 0;
  bool shm_roi_enabled_ = false; /* explicit opt-in until both consumers are upgraded */
  bool shm_baseline_valid_ = false; /* only after consumer ACK of full SHM frame */
  bool last_sent_shm_ = false;
  std::vector<uint8_t> shm_shadow_; /* producer-owned full snapshot for SHM replay */
  int shm_shadow_w_ = 0, shm_shadow_h_ = 0;
  uint64_t last_capture_counter_ = 0;
  std::atomic<bool> page_ready_{false}; /* main URL reached OnLoadEnd */
  bool capture_ = false;
  bool js_capture_ = false; /* last capture state seen by JS (edge events) */
  bool js_visible_ = true;  /* last visibility state seen by JS */

  int rate_sent_ = 0; /* 1s diagnostics counters */
  int rate_req_ = 0;
  int rate_ack_ = 0;
  int rate_unchanged_ = 0;
  int rate_partial_ = 0;
  int rate_full_ = 0;
  int rate_shm_roi_ = 0;
  int rate_deferred_ = 0;
  uint64_t bytes_shm_sent_ = 0;

  GpuBuf gpu_; /* staging blit (CEF linear → driver-native tiled) */

  IMPLEMENT_REFCOUNTING(View);
};
