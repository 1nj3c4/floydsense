#include "launcher.hpp"
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

// ── Design tokens ─────────────────────────────────────────────────────────────
static constexpr int WIN_W = 380;
static constexpr int WIN_H = 280;

static constexpr COLORREF C_BG         = RGB( 12,  12,  22);
static constexpr COLORREF C_PANEL      = RGB( 20,  20,  36);
static constexpr COLORREF C_ACCENT     = RGB(  0, 212, 255);
static constexpr COLORREF C_RED        = RGB(255,  55,  80);
static constexpr COLORREF C_TEXT       = RGB(210, 215, 230);
static constexpr COLORREF C_SUBTEXT    = RGB( 90,  95, 115);
static constexpr COLORREF C_BORDER     = RGB( 35,  35,  58);
static constexpr COLORREF C_BTN_HOV    = RGB( 10, 235, 255);

// ── Per-window state ──────────────────────────────────────────────────────────
struct LauncherCtx {
    std::shared_ptr<AppState>    state;
    std::shared_ptr<std::mutex>  mtx;
    std::function<void()>        onAttach;
    std::function<void()>        onDetach;
    bool attached   = false;
    bool btnHover   = false;
    bool closeHover = false;
    bool minHover   = false;
    HFONT fontLogo  = nullptr;
    HFONT fontTitle = nullptr;
    HFONT fontBody  = nullptr;
    HFONT fontSmall = nullptr;
    // Animations
    float btnHoverT = 0.0f;
    float bgAlpha   = 0.0f;
    float slideOffset = 40.0f;
    float pulseT    = 0.0f;
    float btnScale  = 1.0f;
    bool  isClosing = false;
    bool  attachedAndClosing = false; // true when closing due to successful attach
    bool  listeningKey = false;
    float time      = 0.0f;
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point closeAfter{}; // delayed close timer
    bool  pendingClose = false;
};

static float Lerp(float a, float b, float t) {
    return a + t * (b - a);
}
static COLORREF LerpColor(COLORREF a, COLORREF b, float t) {
    int r = (int)Lerp((float)GetRValue(a), (float)GetRValue(b), t);
    int g = (int)Lerp((float)GetGValue(a), (float)GetGValue(b), t);
    int bl = (int)Lerp((float)GetBValue(a), (float)GetBValue(b), t);
    return RGB(r, g, bl);
}

// ── GDI helpers ───────────────────────────────────────────────────────────────
static void fillRect(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{ x, y, x+w, y+h };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void drawRoundRect(HDC dc, int x, int y, int w, int h, int rx,
                          COLORREF fill, COLORREF border) {
    HBRUSH br    = CreateSolidBrush(fill);
    HPEN   pen   = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, br);
    HPEN   oldPn = (HPEN)  SelectObject(dc, pen);
    RoundRect(dc, x, y, x+w, y+h, rx, rx);
    SelectObject(dc, oldBr); SelectObject(dc, oldPn);
    DeleteObject(br); DeleteObject(pen);
}

// Centers text horizontally if center=true.
static void drawText(HDC dc, int x, int y, const char* text,
                     COLORREF c, HFONT font, bool center = false) {
    HFONT old = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    if (center) {
        SIZE sz{};
        GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
        x -= sz.cx / 2;
    }
    TextOutA(dc, x, y, text, (int)strlen(text));
    SelectObject(dc, old);
}

// ── Paint ─────────────────────────────────────────────────────────────────────
static void paint(HWND hwnd, LauncherCtx* ctx) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - ctx->lastFrame).count();
    ctx->lastFrame = now;
    if (dt > 0.1f) dt = 0.1f;
    ctx->time += dt;

    ctx->bgAlpha = Lerp(ctx->bgAlpha, ctx->isClosing ? 0.0f : 255.0f, dt * 8.0f);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)ctx->bgAlpha, LWA_ALPHA);

    ctx->btnHoverT  = Lerp(ctx->btnHoverT, ctx->btnHover ? 1.0f : 0.0f, dt * 10.0f);
    ctx->slideOffset = Lerp(ctx->slideOffset, ctx->isClosing ? 20.0f : 0.0f, dt * 6.0f);
    ctx->btnScale   = Lerp(ctx->btnScale, ctx->btnHover ? 1.02f : 1.0f, dt * 10.0f);
    
    // Pulsing effect for "LIVE" status
    if (ctx->attached) {
        ctx->pulseT = 0.5f + 0.5f * sinf(ctx->time * 4.0f);
    } else {
        ctx->pulseT = 0.0f;
    }

    PAINTSTRUCT ps;
    HDC realDc = BeginPaint(hwnd, &ps);
    HDC dc     = CreateCompatibleDC(realDc);
    HBITMAP bmp    = CreateCompatibleBitmap(realDc, WIN_W, WIN_H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(dc, bmp);

    int offY = (int)ctx->slideOffset;

    // ── Background ────────────────────────────────────────────────────────────
    fillRect(dc, 0, 0, WIN_W, WIN_H, C_BG);

    // Header bar
    fillRect(dc, 0, offY, WIN_W, 50, C_PANEL);
    fillRect(dc, 0, offY + 49, WIN_W, 1, C_BORDER); // divider

    // Accent left strip
    fillRect(dc, 0, 0, 3, WIN_H, C_ACCENT);

    // ── Logo ──────────────────────────────────────────────────────────────────
    {
        HFONT old = (HFONT)SelectObject(dc, ctx->fontLogo);
        SetBkMode(dc, TRANSPARENT);
        SIZE s1{}, s2{};
        GetTextExtentPoint32A(dc, "FLOYD", 5, &s1);
        int y = offY + (50 - s1.cy) / 2;
        SetTextColor(dc, C_ACCENT);
        TextOutA(dc, 18, y, "FLOYD", 5);
        SetTextColor(dc, C_TEXT);
        TextOutA(dc, 18 + s1.cx, y, "SENSE", 5);
        SelectObject(dc, old);
    }

    // ── Status LED + label ────────────────────────────────────────────────────
    {
        COLORREF baseLED = ctx->attached ? C_ACCENT : RGB(50, 54, 72);
        COLORREF glowLED = ctx->attached ? RGB(100, 240, 255) : baseLED;
        COLORREF ledC = LerpColor(baseLED, glowLED, ctx->pulseT);
        
        HBRUSH br   = CreateSolidBrush(ledC);
        HPEN   noPen = CreatePen(PS_NULL, 0, 0);
        HPEN   op   = (HPEN)  SelectObject(dc, noPen);
        HBRUSH ob   = (HBRUSH)SelectObject(dc, br);
        Ellipse(dc, WIN_W - 90, offY + 19, WIN_W - 79, offY + 30);
        
        // Subtle glow ring when LIVE
        if (ctx->attached) {
            HPEN glowPen = CreatePen(PS_SOLID, 1, LerpColor(C_ACCENT, RGB(12,12,22), 1.0f - ctx->pulseT));
            SelectObject(dc, glowPen);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            int r = (int)(2.0f * ctx->pulseT);
            Ellipse(dc, WIN_W - 90 - r, offY + 19 - r, WIN_W - 79 + r, offY + 30 + r);
            DeleteObject(glowPen);
        }

        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(br); DeleteObject(noPen);
        drawText(dc, WIN_W - 74, offY + 18, ctx->attached ? "LIVE" : "IDLE",
                 ctx->attached ? C_ACCENT : C_SUBTEXT, ctx->fontSmall);
    }

    // ── Window control buttons ────────────────────────────────────────────────
    drawText(dc, WIN_W - 50, offY + 15, "_",
             ctx->minHover ? C_TEXT : C_SUBTEXT, ctx->fontBody);
    drawText(dc, WIN_W - 26, offY + 13, "x",
             ctx->closeHover ? C_RED : C_SUBTEXT, ctx->fontBody);

    // ── Status area ───────────────────────────────────────────────────────────
    {
        std::string status;
        { std::lock_guard lk(*ctx->mtx); status = ctx->state->status; }
        if (status.size() > 54) status = status.substr(0, 51) + "...";

        drawText(dc, 18, offY + 66, "STATUS", C_SUBTEXT, ctx->fontSmall);
        drawText(dc, 18, offY + 82, status.c_str(), C_TEXT, ctx->fontBody);
    }

    // ── Player count (when attached) ──────────────────────────────────────────
    if (ctx->attached) {
        size_t n = 0;
        { std::lock_guard lk(*ctx->mtx); n = ctx->state->players.size(); }
        char buf[8];
        wsprintfA(buf, "%d", (int)n);
        int cx = WIN_W - 60;
        drawText(dc, cx, offY + 66, "FOUND", C_SUBTEXT, ctx->fontSmall, true);
        drawText(dc, cx, offY + 80, buf, C_ACCENT, ctx->fontTitle, true);
    }

    // ── ATTACH / DETACH button ────────────────────────────────────────────────
    {
        int bw = 180, bh = 38;
        // Scale button on hover
        bw = (int)(bw * ctx->btnScale);
        bh = (int)(bh * ctx->btnScale);
        
        int bx = (WIN_W - bw) / 2, by = offY + 134 - (bh - 38)/2;
        COLORREF baseC = ctx->attached ? C_RED : C_ACCENT;
        COLORREF hovC  = ctx->attached ? RGB(255, 100, 120) : C_BTN_HOV;
        COLORREF btnC  = LerpColor(baseC, hovC, ctx->btnHoverT);
        drawRoundRect(dc, bx, by, bw, bh, 6, btnC, btnC);

        HFONT old = (HFONT)SelectObject(dc, ctx->fontTitle);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(8, 8, 18));
        const char* lbl = ctx->attached ? "DETACH" : "ATTACH";
        SIZE sz{};
        GetTextExtentPoint32A(dc, lbl, (int)strlen(lbl), &sz);
        TextOutA(dc, bx + (bw - sz.cx)/2, by + (bh - sz.cy)/2, lbl, (int)strlen(lbl));
        SelectObject(dc, old);
    }

    // ── Hint lines ────────────────────────────────────────────────────────────
    int hintY = offY + 186;
    drawText(dc, WIN_W/2, hintY + 54, "HOME = menu   F1 = teammates   F2 = health", C_SUBTEXT, ctx->fontSmall, true);

    bool aimEnabled = false; int aimKey = 0; int aimBone = 6;
    { std::lock_guard lk(*ctx->mtx); aimEnabled = ctx->state->aimbotEnabled; aimKey = ctx->state->aimKeybind; aimBone = ctx->state->targetBone; }
    drawText(dc, WIN_W/2, hintY, aimEnabled ? "Aimbot: ON (Click here to toggle)" : "Aimbot: OFF (Click here to toggle)", aimEnabled ? C_ACCENT : C_SUBTEXT, ctx->fontSmall, true);
    
    std::string keyStr;
    if (ctx->listeningKey) {
        keyStr = "Key: [ ... ] (Press any key)";
    } else {
        keyStr = "Key: [ " + VKToString(aimKey) + " ] (Click to change)";
    }
    drawText(dc, WIN_W/2, hintY + 18, keyStr.c_str(), C_SUBTEXT, ctx->fontSmall, true);

    const char* boneStr = "Target: [ Head ] (Right Click)";
    if (aimBone == 5) boneStr = "Target: [ Neck ] (Right Click)";
    else if (aimBone == 4) boneStr = "Target: [ Chest ] (Right Click)";
    else if (aimBone == 2) boneStr = "Target: [ Stomach ] (Right Click)";
    drawText(dc, WIN_W/2, hintY + 36, boneStr, C_SUBTEXT, ctx->fontSmall, true);

    // ── Outer border ─────────────────────────────────────────────────────────
    {
        HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN   bp = CreatePen(PS_SOLID, 1, C_BORDER);
        HPEN   op = (HPEN)  SelectObject(dc, bp);
        HBRUSH ob = (HBRUSH)SelectObject(dc, nb);
        Rectangle(dc, 0, 0, WIN_W, WIN_H);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(bp);
    }

    BitBlt(realDc, 0, 0, WIN_W, WIN_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// ── Hit testing ───────────────────────────────────────────────────────────────
static bool hitBtn  (int x, int y){ int bx=(WIN_W-180)/2,by=134; return x>=bx&&x<=bx+180&&y>=by&&y<=by+38; }
static bool hitClose(int x, int y){ return x>=WIN_W-34&&x<=WIN_W-14&&y>=8&&y<=32; }
static bool hitMin  (int x, int y){ return x>=WIN_W-58&&x<=WIN_W-38&&y>=8&&y<=32; }
static bool hitAimBtn(int x, int y) { return x >= WIN_W/2 - 100 && x <= WIN_W/2 + 100 && y >= 176 && y <= 196; }
static bool hitAimKey(int x, int y) { return x >= WIN_W/2 - 100 && x <= WIN_W/2 + 100 && y >= 196 && y <= 216; }

// ── Window procedure ──────────────────────────────────────────────────────────
static LRESULT CALLBACK LauncherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<LauncherCtx*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_PAINT:    if (ctx) paint(hwnd, ctx); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_NCHITTEST: {
        POINT pt{ (LONG)(short)LOWORD(lp), (LONG)(short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.y < 50 && !hitClose(pt.x,pt.y) && !hitMin(pt.x,pt.y))
            return HTCAPTION;
        return HTCLIENT;
    }

    case WM_LBUTTONUP: {
        if (!ctx) break;
        int x = LOWORD(lp), y = HIWORD(lp);
        if (ctx->listeningKey) return 0;

        if (hitClose(x,y)) { ctx->isClosing = true; return 0; }
        if (hitMin(x,y))   { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
        if (hitBtn(x,y)) {
            ctx->attached = !ctx->attached;
            if (ctx->attached) {
                ctx->onAttach();
                // Delay close by 800ms so the overlay thread can fully initialize
                ctx->pendingClose = true;
                ctx->attachedAndClosing = true;
                ctx->closeAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            } else {
                ctx->onDetach();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (hitAimBtn(x,y)) {
            std::lock_guard lk(*ctx->mtx);
            ctx->state->aimbotEnabled = !ctx->state->aimbotEnabled;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (hitAimKey(x,y)) {
            ctx->listeningKey = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        if (!ctx) break;
        int x = LOWORD(lp), y = HIWORD(lp);
        if (ctx->listeningKey) return 0;
        
        // Use the same hit box as the keybind text, but on right click cycle the bone
        if (hitAimKey(x,y) || (y >= 210 && y <= 235)) {
            std::lock_guard lk(*ctx->mtx);
            int current = ctx->state->targetBone;
            if (current == 6) ctx->state->targetBone = 5;
            else if (current == 5) ctx->state->targetBone = 4;
            else if (current == 4) ctx->state->targetBone = 2;
            else ctx->state->targetBone = 6;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!ctx) break;
        int x = LOWORD(lp), y = HIWORD(lp);
        bool b = hitBtn(x,y), c = hitClose(x,y), m = hitMin(x,y);
        if (b!=ctx->btnHover||c!=ctx->closeHover||m!=ctx->minHover) {
            ctx->btnHover=b; ctx->closeHover=c; ctx->minHover=m;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (ctx && (ctx->btnHover||ctx->closeHover||ctx->minHover)) {
            ctx->btnHover=ctx->closeHover=ctx->minHover=false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ── Entry point ───────────────────────────────────────────────────────────────
void runLauncher(
    std::shared_ptr<AppState>    state,
    std::shared_ptr<std::mutex>  mtx,
    std::atomic<bool>&           stop,
    std::function<void()>        onAttach,
    std::function<void()>        onDetach)
{
    HINSTANCE hInst = GetModuleHandleA(nullptr);

    LauncherCtx ctx;
    ctx.state    = state;
    ctx.mtx      = mtx;
    ctx.onAttach = std::move(onAttach);
    ctx.onDetach = std::move(onDetach);

    ctx.fontLogo  = CreateFontA(26,0,0,0,FW_BOLD,    0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    ctx.fontTitle = CreateFontA(15,0,0,0,FW_SEMIBOLD, 0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    ctx.fontBody  = CreateFontA(13,0,0,0,FW_NORMAL,   0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    ctx.fontSmall = CreateFontA(11,0,0,0,FW_NORMAL,   0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");

    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = LauncherProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "FsLauncher";
    RegisterClassExA(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExA(
        WS_EX_APPWINDOW | WS_EX_LAYERED,
        "FsLauncher", "FloydSense",
        WS_POPUP | WS_VISIBLE,
        (sx - WIN_W)/2, (sy - WIN_H)/2, WIN_W, WIN_H,
        nullptr, nullptr, hInst, nullptr);

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));
    SetTimer(hwnd, 1, 16, nullptr); // 60fps animations + status refresh

    MSG msg{};
    while (!stop.load(std::memory_order_relaxed)) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            if (msg.message == WM_TIMER) {
                if (ctx.listeningKey) {
                    for (int i = 1; i < 256; ++i) {
                        if (i == VK_HOME || i == VK_ESCAPE || i == VK_LBUTTON) continue;
                        if (GetAsyncKeyState(i) & 0x8000) {
                            std::lock_guard lk(*ctx.mtx);
                            ctx.state->aimKeybind = i;
                            ctx.listeningKey = false;
                            break;
                        }
                    }
                }
                // Check delayed close after attach
                if (ctx.pendingClose && std::chrono::steady_clock::now() >= ctx.closeAfter) {
                    ctx.pendingClose = false;
                    ctx.isClosing = true;
                }
                if (ctx.isClosing && ctx.bgAlpha <= 1.0f) goto done;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
done:
    KillTimer(hwnd, 1);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    DeleteObject(ctx.fontLogo);
    DeleteObject(ctx.fontTitle);
    DeleteObject(ctx.fontBody);
    DeleteObject(ctx.fontSmall);
    UnregisterClassA("FsLauncher", hInst);
    // Only signal stop if we didn't exit due to a successful attach.
    // If we attached, the overlay thread stays alive independently.
    if (!ctx.attachedAndClosing) {
        stop.store(true, std::memory_order_relaxed);
    }
}
