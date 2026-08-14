#pragma once
#include "include/cef_app.h"

/* Shared by the browser process and all subprocesses (CefExecuteProcess
 * uses the same CefApp). Command-line switches must be identical across
 * processes, which is why they are applied here. */
class App : public CefApp {
public:
  void OnBeforeCommandLineProcessing(const CefString &, CefRefPtr<CefCommandLine> cl) override {
    /* The overlay webview runs unprivileged inside a game session; the
     * setuid sandbox would need root. */
    cl->AppendSwitch("no-sandbox");
    cl->AppendSwitch("disable-gpu-sandbox");
    cl->AppendSwitch("disable-crash-reporter");
    /* OSR pages must keep rendering/animating even if Chromium considers
     * them backgrounded — otherwise rAF/CSS animations throttle to 1fps. */
    cl->AppendSwitch("disable-renderer-backgrounding");
    cl->AppendSwitch("disable-background-timer-throttling");
    cl->AppendSwitch("disable-backgrounding-occluded-windows");
  }

  IMPLEMENT_REFCOUNTING(App);
};
