#include "input_receiver.h"

#include <QSocketNotifier>
#include <QDebug>
#include <QWebEngineView>
#include <QWebEnginePage>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

#include "public/idk_ipc.h"
#include "core/log.h"

InputReceiver::InputReceiver(const QString &frameSocketPath, QObject *parent)
    : QObject(parent)
{
    m_socketPath = frameSocketPath + QStringLiteral("-input");
}

InputReceiver::~InputReceiver()
{
    closeFd();
}

bool InputReceiver::connectToInput()
{
    closeFd();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    QByteArray pathBytes = m_socketPath.toUtf8();
    if (pathBytes.size() >= (int)sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::memcpy(addr.sun_path, pathBytes.constData(), pathBytes.size());

    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    m_fd = fd;

    int fl = fcntl(m_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(m_fd, F_SETFL, fl | O_NONBLOCK);

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated,
            this, &InputReceiver::onReadyRead);

    IDK_LOG("input-rx", "Connected to %s (fd=%d)\n",
            m_socketPath.toUtf8().data(), m_fd);
    return true;
}

void InputReceiver::disconnect()
{
    closeFd();
}

void InputReceiver::closeFd()
{
    if (m_notifier) {
        delete m_notifier;
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_captureState) {
        m_captureState = false;
        emit inputCaptureChanged(false);
    }
}

void InputReceiver::onReadyRead()
{
    if (m_fd < 0) return;

    idk_ipc_input_event_t ev;
    while (true) {
        ssize_t n = ::read(m_fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            if (ev.magic != IDK_INPUT_MAGIC) {
                closeFd();
                return;
            }

            InputEvent ie;
            ie.type    = ev.type;
            ie.time    = ev.time;
            ie.serial  = ev.serial;
            ie.keycode = ev.keycode;
            ie.keysym  = ev.keysym;
            ie.state   = ev.state;
            ie.button  = ev.button;
            ie.x       = ev.x;
            ie.y       = ev.y;
            ie.dx      = ev.dx;
            ie.dy      = ev.dy;
            ie.mods    = ev.mods;
            ie.capture = ev.capture;

            bool nowCaptured = (ev.capture != 0);
            if (nowCaptured != m_captureState) {
                m_captureState = nowCaptured;
                emit inputCaptureChanged(nowCaptured);
                IDK_LOG("input-rx", "capture %s\n", nowCaptured ? "ENABLED" : "DISABLED");
            }

            if (ev.type == IDK_INPUT_KEY && ev.keycode != 0) {
                IDK_LOG("input-rx", "KEY: keycode=%u keysym=0x%x state=%u mods=%u\n",
                        ev.keycode, ev.keysym, ev.state, ev.mods);
            }

            emit eventReceived(ie);

            if (m_webview) {
                switch (ev.type) {
                case IDK_INPUT_KEY:
                    if (ev.keycode != 0)
                        injectKeyboardEvent(ie);
                    break;
                case IDK_INPUT_BUTTON:
                case IDK_INPUT_MOTION:
                case IDK_INPUT_AXIS:
                    injectMouseEvent(ie);
                    break;
                }
            }
        } else if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            closeFd();
            return;
        } else if (n == 0) {
            closeFd();
            return;
        } else {
            closeFd();
            return;
        }
    }
}

/* ── JS helper templates ──────────────────────────────────────────────── */

/* Shared preamble: clear stale target, find a writable element, focus it.
 * Sets window.__idk_el for the body IIFE that follows. */
static const char *JS_PREAMBLE = R"(
window.__idk_el=null;
(function(){
try{
var el=document.activeElement;
if(!el||el===document.body){
  el=document.querySelector('input:not([type=hidden]):not([disabled]),textarea,[contenteditable]');
  if(el)el.focus();
  else{console.error('idk:no-input');return;}
}
if(el.isContentEditable||el.tagName==='INPUT'||el.tagName==='TEXTAREA'){
  window.__idk_el=el;
}
else console.error('idk:bad-el='+el.tagName);
}catch(e){console.error('idk:preamble '+e.message);}
})();

(function(){
try{
var el=window.__idk_el;
if(!el)return;
)";

/* Suffix: close the body IIFE and catch */
static const char *JS_SUFFIX = R"(
}catch(e){console.error('idk:exec '+e.message);}
})();
)";

/* Insert a single character at cursor position.
 * Uses InputEvent (not Event) so React/Vue intercept it. */
static const char *JS_INSERT_CHAR = R"(
var start=el.selectionStart,end=el.selectionEnd;
el.value=el.value.substring(0,start)+'%1'+el.value.substring(end);
el.setSelectionRange(start+1,start+1);
el.dispatchEvent(new InputEvent('input',{inputType:'insertText',bubbles:true,cancelable:true}));
)";

/* Backspace: delete character before cursor */
static const char *JS_BACKSPACE = R"(
if(el.isContentEditable){document.execCommand('deleteContentBackward');return;}
var start=el.selectionStart,end=el.selectionEnd;
if(start>0||end>0){
  if(start===end)start--;
  el.value=el.value.substring(0,start)+el.value.substring(end);
  el.setSelectionRange(start,start);
  el.dispatchEvent(new InputEvent('input',{inputType:'deleteContentBackward',bubbles:true,cancelable:true}));
}
)";

/* Delete: delete character after cursor */
static const char *JS_DELETE = R"(
if(el.isContentEditable){document.execCommand('deleteContentForward');return;}
var start=el.selectionStart,end=el.selectionEnd;
if(start<el.value.length||end<el.value.length){
  if(start===end)end++;
  el.value=el.value.substring(0,start)+el.value.substring(end);
  el.setSelectionRange(start,start);
  el.dispatchEvent(new InputEvent('input',{inputType:'deleteContentForward',bubbles:true,cancelable:true}));
}
)";

/* Enter on input/textarea */
static const char *JS_ENTER = R"(
if(el.tagName==='INPUT'||el.tagName==='TEXTAREA'){
  el.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true,cancelable:true}));
  el.dispatchEvent(new KeyboardEvent('keypress',{key:'Enter',bubbles:true,cancelable:true}));
  el.dispatchEvent(new KeyboardEvent('keyup',{key:'Enter',bubbles:true,cancelable:true}));
  el.dispatchEvent(new InputEvent('beforeinput',{inputType:'insertLineBreak',bubbles:true}));
  el.dispatchEvent(new InputEvent('input',{inputType:'insertLineBreak',bubbles:true,cancelable:true}));
}else if(el.isContentEditable){
  document.execCommand('insertParagraph');
}
)";

/* Generic KeyboardEvent dispatch (Escape, Tab, arrows, F-keys, Space)
 * Uses document.activeElement (no preamble needed — works on any page). */
static const char *JS_KEY_EVENT_FMT = R"(
var el=document.activeElement||document.body;
el.dispatchEvent(new KeyboardEvent('keydown',{key:'%1',bubbles:true,cancelable:true}));
el.dispatchEvent(new KeyboardEvent('keyup',{key:'%1',bubbles:true,cancelable:true}));
)";

/* Insert space character */
static const char *JS_INSERT_SPACE = R"(
var start=el.selectionStart,end=el.selectionEnd;
el.value=el.value.substring(0,start)+' '+el.value.substring(end);
el.setSelectionRange(start+1,start+1);
el.dispatchEvent(new InputEvent('input',{inputType:'insertText',bubbles:true,cancelable:true,data:' '}));
)";

/* Mouse move — standalone, no preamble needed */
static const char *JS_MOUSE_MOVE = R"(
(function(){
try{
var el=document.elementFromPoint(%1,%2)||document.body;
el.dispatchEvent(new MouseEvent('mousemove',{clientX:%1,clientY:%2,bubbles:true,view:window}));
}catch(e){console.error('idk:mouse '+e.message);}
})();
)";

/* Mouse down/up — standalone, no preamble needed */
static const char *JS_MOUSE_DOWN = R"(
(function(){
try{
var el=document.elementFromPoint(%1,%2)||document.body;
el.dispatchEvent(new MouseEvent('mousedown',{clientX:%1,clientY:%2,button:%3,bubbles:true,view:window}));
}catch(e){console.error('idk:mouse '+e.message);}
})();
)";

static const char *JS_MOUSE_UP = R"(
(function(){
try{
var el=document.elementFromPoint(%1,%2)||document.body;
el.dispatchEvent(new MouseEvent('mouseup',{clientX:%1,clientY:%2,button:%3,bubbles:true,view:window}));
}catch(e){console.error('idk:mouse '+e.message);}
})();
)";

/* Scroll via scrollBy — standalone, no preamble needed. */
static const char *JS_SCROLL = R"(
(function(){
try{
var el=document.elementFromPoint(%1,%2);
while(el&&el!==document.body&&el.scrollHeight<=el.clientHeight)el=el.parentElement;
if(!el||el===document.body)el=document.scrollingElement||document.documentElement||document.body;
el.scrollBy(%3,%4);
}catch(e){console.error('idk:scroll '+e.message);}
})();
)";

/* ── key name map for JS dispatch ──────────────────────────────────────── */
struct KeyName { quint32 ks; const char *name; };
static const KeyName KEY_NAMES[] = {
    { 0xff08, "Backspace" },
    { 0xff09, "Tab" },
    { 0xff0d, "Enter" },
    { 0xff1b, "Escape" },
    { 0xff50, "Home" },
    { 0xff51, "ArrowLeft" },
    { 0xff52, "ArrowUp" },
    { 0xff53, "ArrowRight" },
    { 0xff54, "ArrowDown" },
    { 0xff55, "PageUp" },
    { 0xff56, "PageDown" },
    { 0xff57, "End" },
    { 0xff63, "Insert" },
    { 0xffff, "Delete" },
    { 0xffbe, "F1" },  { 0xffbf, "F2" },  { 0xffc0, "F3" },
    { 0xffc1, "F4" },  { 0xffc2, "F5" },  { 0xffc3, "F6" },
    { 0xffc4, "F7" },  { 0xffc5, "F8" },  { 0xffc6, "F9" },
    { 0xffc7, "F10" }, { 0xffc8, "F11" }, { 0xffc9, "F12" },
    { 0, nullptr }
};

static const char *keysymToKeyName(quint32 ks)
{
    for (int i = 0; KEY_NAMES[i].name; i++)
        if (KEY_NAMES[i].ks == ks) return KEY_NAMES[i].name;
    return nullptr;
}

static void runJs(QWebEnginePage *page, QWebEngineView *view, const QString &js)
{
    if (!page) return;
    page->runJavaScript(js, [view](const QVariant &result) {
        if (result.isValid() && result.canConvert<QVariantMap>()) {
            QVariantMap m = result.toMap();
            if (m.contains("error"))
                qWarning("[idk-js] %s", m["error"].toString().toUtf8().data());
        }
        /* JS has executed — capture result in next render. */
        if (view) {
            if (auto *fp = view->focusProxy())
                fp->update();
        }
    });
    /* Best-effort immediate paint for low-latency feedback.
     * May capture stale state (JS hasn't executed yet), but the
     * callback above guarantees a correct frame follows shortly. */
    if (view) {
        if (auto *fp = view->focusProxy())
            fp->update();
    }
}

/* ── Keyboard injection via runJavaScript ──────────────────────────────── */
void InputReceiver::injectKeyboardEvent(const InputEvent &ev)
{
    if (!m_webview) return;

    if (ev.state != 1) return;

    QWebEnginePage *page = m_webview->page();
    if (!page) return;

    /* Printable ASCII — insert character at cursor */
    if (ev.keysym >= 0x20 && ev.keysym < 0x7f) {
        QString c = QChar(ev.keysym);
        if (c == QLatin1Char('\'')) c = QStringLiteral("\\'");
        if (c == QLatin1Char('\\')) c = QStringLiteral("\\\\");
        if (c == QLatin1Char('\n')) c = QStringLiteral("\\n");
        if (c == QLatin1Char('\r')) c = QStringLiteral("\\r");
        QString js = QString(JS_PREAMBLE) + QString(JS_INSERT_CHAR).arg(c) + JS_SUFFIX;
        runJs(page, m_webview, js);
        IDK_LOG("input-rx", "JS INSERT CHAR: '%c'\n", (char)ev.keysym);
        return;
    }

    /* Special keys with dedicated handlers */
    if (ev.keysym == 0xff08) { /* Backspace */
        runJs(page, m_webview, QString(JS_PREAMBLE) + JS_BACKSPACE + JS_SUFFIX);
        IDK_LOG("input-rx", "JS BACKSPACE\n");
        return;
    }
    if (ev.keysym == 0xffff) { /* Delete */
        runJs(page, m_webview, QString(JS_PREAMBLE) + JS_DELETE + JS_SUFFIX);
        IDK_LOG("input-rx", "JS DELETE\n");
        return;
    }
    if (ev.keysym == 0xff0d) { /* Enter */
        runJs(page, m_webview, QString(JS_PREAMBLE) + JS_ENTER + JS_SUFFIX);
        IDK_LOG("input-rx", "JS ENTER\n");
        return;
    }
    if (ev.keysym == 0x0020) { /* Space */
        runJs(page, m_webview, QString(JS_PREAMBLE) + JS_INSERT_SPACE + JS_SUFFIX);
        IDK_LOG("input-rx", "JS INSERT SPACE\n");
        return;
    }

    /* All other keys — dispatch keydown/keyup */
    const char *keyName = keysymToKeyName(ev.keysym);
    if (!keyName) return;
    QString js = QString(JS_PREAMBLE) + QString(JS_KEY_EVENT_FMT).arg(keyName) + JS_SUFFIX;
    runJs(page, m_webview, js);
    IDK_LOG("input-rx", "JS DISPATCH KEY: %s\n", keyName);
}

/* ── Mouse injection via runJavaScript ─────────────────────────────────── */
void InputReceiver::injectMouseEvent(const InputEvent &ev)
{
    if (!m_webview) return;

    m_mouseX = ev.x;
    m_mouseY = ev.y;
    emit cursorMoved(m_mouseX, m_mouseY);

    QWebEnginePage *page = m_webview->page();
    if (!page) return;

    switch (ev.type) {
    case IDK_INPUT_MOTION: {
        QString js = QString(JS_MOUSE_MOVE).arg(m_mouseX).arg(m_mouseY);
        runJs(page, m_webview, js);
        break;
    }
    case IDK_INPUT_BUTTON: {
        int btn = 0;
        if (ev.button == 0x110) btn = 0;
        else if (ev.button == 0x111) btn = 2;
        else if (ev.button == 0x112) btn = 1;
        else return;

        const char *tmpl = (ev.state == 1) ? JS_MOUSE_DOWN : JS_MOUSE_UP;
        QString js = QString(tmpl).arg(m_mouseX).arg(m_mouseY).arg(btn);
        runJs(page, m_webview, js);

        const char *btn_name = btn == 0 ? "left" : btn == 2 ? "right" : "middle";
        IDK_LOG("input-rx", "JS MOUSE %s (%s) at %d,%d\n",
                (ev.state == 1) ? "DOWN" : "UP", btn_name, m_mouseX, m_mouseY);
        break;
    }
    case IDK_INPUT_AXIS: {
        IDK_LOG("input-rx", "AXIS: dx=%d dy=%d at %d,%d\n",
                ev.dx, ev.dy, m_mouseX, m_mouseY);
        QString js = QString(JS_SCROLL)
            .arg(m_mouseX).arg(m_mouseY)
            .arg(ev.dx * 20).arg(-ev.dy * 20);
        runJs(page, m_webview, js);
        break;
    }
    }
}
