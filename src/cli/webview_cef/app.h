#pragma once

#include <atomic>

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "pump.h"

inline std::atomic<CefPump *> g_cef_pump{nullptr};

/* Shared by the browser process and all subprocesses (CefExecuteProcess
 * uses the same CefApp). Command-line switches must be identical across
 * processes, which is why they are applied here. */
class App : public CefApp, public CefBrowserProcessHandler {
public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

  void OnScheduleMessagePumpWork(int64_t delay_ms) override {
    CefPump *pump = g_cef_pump.load(std::memory_order_acquire);
    if (pump)
      pump->Schedule(delay_ms);
  }

  void OnBeforeCommandLineProcessing(const CefString &, CefRefPtr<CefCommandLine> cl) override {
    /* The overlay webview runs unprivileged inside a game session; the
     * setuid sandbox would need root. */
    cl->AppendSwitch("no-sandbox");
    cl->AppendSwitch("disable-gpu-sandbox");
    cl->AppendSwitch("disable-crash-reporter");
    /* Chromium's Linux stack-guard fork mode crashes CEF utility
     * subprocesses with stack smashing on affected kernels. */
    cl->AppendSwitchWithValue("change-stack-guard-on-fork", "disable");
    /* OSR pages must keep rendering/animating even if Chromium considers
     * them backgrounded — otherwise rAF/CSS animations throttle to 1fps. */
    cl->AppendSwitch("disable-renderer-backgrounding");
    cl->AppendSwitch("disable-background-timer-throttling");
    cl->AppendSwitch("disable-backgrounding-occluded-windows");
    cl->AppendSwitch("enable-unsafe-webgpu");
  }

  IMPLEMENT_REFCOUNTING(App);
};
