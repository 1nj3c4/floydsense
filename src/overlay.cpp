#include "overlay.hpp"
#include "buttons.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <tuple>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace btn = cs2_dumper::buttons;

struct Vector2 { float x, y; };

static Vector2 CalcAngle(const Vec3& src, const Vec3& dst) {
    Vector2 angles;
    Vec3 delta = { dst.x - src.x, dst.y - src.y, dst.z - src.z };
    float hyp = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    
    angles.x = static_cast<float>(atan2(-delta.z, hyp) * (180.0 / M_PI));
    angles.y = static_cast<float>(atan2(delta.y, delta.x) * (180.0 / M_PI));
    
    return angles;
}

static float GetFOV(const Vector2& viewAngle, const Vector2& aimAngle) {
    Vector2 delta;
    delta.x = aimAngle.x - viewAngle.x;
    delta.y = aimAngle.y - viewAngle.y;

    while (delta.y > 180.f) delta.y -= 360.f;
    while (delta.y < -180.f) delta.y += 360.f;
    while (delta.x > 89.f) delta.x -= 180.f;
    while (delta.x < -89.f) delta.x += 180.f;

    return std::sqrt(delta.x * delta.x + delta.y * delta.y);
}

// Hotkey IDs
static constexpr int HK_PANEL     = 1;
static constexpr int HK_TEAMMATES = 2;
static constexpr int HK_HEALTH    = 3;

// Color presets (6 per category, index 0 = default)
static constexpr COLORREF kEnemyColors[6] = {
    RGB(255, 50, 70), RGB(255,140,  0), RGB(255,220,  0),
    RGB(240,240,240), RGB(180, 80,255), RGB(255, 60,200),
};
static constexpr COLORREF kTeamColors[6] = {
    RGB(  0,220,255), RGB( 50,130,255), RGB( 50,220,100),
    RGB(240,240,240), RGB(255,220,  0), RGB(160,255, 60),
};
static constexpr COLORREF kHealthColors[6] = {
    RGB(  0,195,100), RGB(255, 50, 70), RGB(240,240,240),
    RGB(255,220,  0), RGB(  0,220,255), RGB(255,140,  0),
};
static constexpr int kNumPresets = 6;

// Panel design tokens 
static constexpr COLORREF P_BG      = RGB( 24,  24,  27); // zinc-900
static constexpr COLORREF P_SIDEBAR = RGB( 15,  15,  17); // darker
static constexpr COLORREF P_HEADER  = RGB( 24,  24,  27); 
static constexpr COLORREF P_SECTION = RGB( 39,  39,  42); // zinc-800
static constexpr COLORREF P_BORDER  = RGB( 63,  63,  70); // zinc-700
static constexpr COLORREF P_ACCENT  = RGB( 99, 102, 241); // indigo-500
static constexpr COLORREF P_TEXT    = RGB(244, 244, 245); // zinc-50
static constexpr COLORREF P_SUB     = RGB(161, 161, 170); // zinc-400
static constexpr COLORREF P_ON      = RGB( 34, 197,  94); // emerald-500
static constexpr COLORREF P_OFF     = RGB( 82,  82,  91); // zinc-600

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::optional<std::pair<int, int>> project(
    float x, float y, float z,
    const std::array<float, 16>& m, float sw, float sh)
{
    float w = m[12]*x + m[13]*y + m[14]*z + m[15];
    if (w < 0.001f) return std::nullopt;
    float px = m[0]*x + m[1]*y + m[2]*z + m[3];
    float py = m[4]*x + m[5]*y + m[6]*z + m[7];
    return std::make_pair(
        static_cast<int>((sw / 2.f) * (1.f + px / w)),
        static_cast<int>((sh / 2.f) * (1.f - py / w))
    );
}

static std::optional<std::tuple<int,int,int,int>> playerRect(
    const Vec3& pos, const std::array<float, 16>& m, float sw, float sh)
{
    auto feet = project(pos.x, pos.y, pos.z,        m, sw, sh);
    auto head = project(pos.x, pos.y, pos.z + 72.f, m, sw, sh);
    if (!feet || !head) return std::nullopt;

    auto [fx, fy] = *feet;
    auto [hx, hy] = *head;

    float h = static_cast<float>(std::abs(fy - hy));
    if (h < 4.f) h = 4.f;
    float w = h / 2.5f;

    return std::make_tuple(
        static_cast<int>(fx - w / 2.f), hy,
        static_cast<int>(fx + w / 2.f), fy
    );
}

static void drawEsp(
    HDC dc, const std::vector<EspPlayer>& players,
    const std::array<float, 16>& vm, float sw, float sh,
    bool showTeammates, bool showHealthBars, bool showNames, bool showArmor, bool showSkeleton,
    int32_t localTeam, int boxThick, int boxStyle,
    COLORREF enemyColor, COLORREF teamColor, COLORREF healthColor,
    HFONT labelFont)
{
    for (const auto& p : players) {
        if (!showTeammates && localTeam != 0 && p.team == localTeam)
            continue;

        auto rect = playerRect(p.pos, vm, sw, sh);
        if (!rect) continue;

        auto [l, t, r, b] = *rect;
        int boxH = b - t;
        if (boxH < 5 || boxH > static_cast<int>(sh)) continue;

        COLORREF boxColor;
        if (localTeam != 0)
            boxColor = (p.team == localTeam) ? teamColor : enemyColor;
        else
            boxColor = (p.team == 3) ? teamColor : enemyColor;

        if (showSkeleton && p.hasBones) {
            HPEN skelPen = CreatePen(PS_SOLID, 1, RGB(220, 220, 220)); 
            HPEN oldSkelPen = static_cast<HPEN>(SelectObject(dc, skelPen));
            
            auto drawBoneLine = [&](int b1, int b2) {
                auto pt1 = project(p.bones[b1].x, p.bones[b1].y, p.bones[b1].z, vm, sw, sh);
                auto pt2 = project(p.bones[b2].x, p.bones[b2].y, p.bones[b2].z, vm, sw, sh);
                if (pt1 && pt2) {
                    MoveToEx(dc, pt1->first, pt1->second, nullptr);
                    LineTo(dc, pt2->first, pt2->second);
                }
            };
            
            drawBoneLine(0, 1); drawBoneLine(1, 2); drawBoneLine(2, 3);
            drawBoneLine(2, 4); drawBoneLine(4, 5); drawBoneLine(5, 6);
            drawBoneLine(2, 7); drawBoneLine(7, 8); drawBoneLine(8, 9);
            drawBoneLine(3, 10); drawBoneLine(10, 11); drawBoneLine(11, 12);
            drawBoneLine(3, 13); drawBoneLine(13, 14); drawBoneLine(14, 15);
            
            SelectObject(dc, oldSkelPen); DeleteObject(skelPen);
        }

        if (showNames && p.name[0]) {
            HFONT oldF = (HFONT)SelectObject(dc, labelFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 240, 240));
            int nl = (int)strnlen(p.name, 127);
            SIZE sz{}; GetTextExtentPoint32A(dc, p.name, nl, &sz);
            TextOutA(dc, l + (r - l) / 2 - sz.cx / 2, t - sz.cy - 2, p.name, nl);
            SelectObject(dc, oldF);
        }

        if (boxStyle > 0) {
            HPEN   pen    = CreatePen(PS_SOLID, boxThick, boxColor);
            HPEN   oldPen = static_cast<HPEN>(SelectObject(dc, pen));
            HBRUSH oldBr  = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
            
            if (boxStyle == 1) {
                Rectangle(dc, l, t, r, b);
            } else if (boxStyle == 2) {
                int w = r - l;
                int len = w / 4;
                if (len < 4) len = 4;
                MoveToEx(dc, l, t + len, nullptr); LineTo(dc, l, t); LineTo(dc, l + len, t);
                MoveToEx(dc, r - len, t, nullptr); LineTo(dc, r, t); LineTo(dc, r, t + len);
                MoveToEx(dc, l, b - len, nullptr); LineTo(dc, l, b); LineTo(dc, l + len, b);
                MoveToEx(dc, r - len, b, nullptr); LineTo(dc, r, b); LineTo(dc, r, b - len);
            }
            
            SelectObject(dc, oldPen); SelectObject(dc, oldBr); DeleteObject(pen);
        }

        if (showHealthBars) {
            int barH = static_cast<int>(boxH * (std::clamp(p.health, 0, 100) / 100.f));
            RECT bar{ l - 6, b - barH, l - 3, b };
            HBRUSH hbr = CreateSolidBrush(healthColor);
            FillRect(dc, &bar, hbr);
            DeleteObject(hbr);
        }

        if (showArmor) {
            int barH = static_cast<int>(boxH * (std::clamp(p.armor, 0, 100) / 100.f));
            RECT bar{ r + 3, b - barH, r + 6, b };
            HBRUSH abr = CreateSolidBrush(RGB(0, 140, 255));
            FillRect(dc, &bar, abr);
            DeleteObject(abr);
        }

        char buf[8];
        int  len = wsprintfA(buf, "%d", p.health);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, boxColor);
        TextOutA(dc, l + (r - l) / 2 - 8, b + 2, buf, len);
    }
}

static constexpr int PNL_W = 760, PNL_H = 560;
static constexpr int SIDEBAR_W = 180;

struct UIElement {
    int id;
    int type; // 0=Toggle, 1=Slider, 2=Button, 3=Swatch
    RECT bounds;
    void* data; 
    float minV, maxV;
};

struct PanelCtx {
    std::shared_ptr<AppState>    state;
    std::shared_ptr<std::mutex>  mtx;
    HFONT fTitle = nullptr;
    HFONT fLarge = nullptr;
    HFONT fBody  = nullptr;
    HFONT fSmall = nullptr;
    
    int activeTab = 0; 
    int hoverTab = -1;
    float tabHoverT[4] = {0};
    int hoverRow = -1;
    
    float togglePos[64] = {0};
    float rowHoverT[64] = {0};
    
    std::vector<UIElement> elements;
    int activeSlider = -1;
    
    float bgAlpha     = 0.0f;
    float slideOffset = 0.0f;
    float time        = 0.0f;
    bool isClosing    = false;
    bool listeningKey = false;
    
    bool isDragging = false;
    POINT dragStartMouse{};
    POINT dragStartWindow{};
    
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
};

static float Lerp(float a, float b, float t) { return a + t * (b - a); }
static float EaseInOutCubic(float t) { return t < 0.5f ? 4.0f * t * t * t : 1.0f - pow(-2.0f * t + 2.0f, 3.0f) / 2.0f; }
static COLORREF LerpColor(COLORREF a, COLORREF b, float t) {
    t = EaseInOutCubic(t);
    int r = (int)Lerp((float)GetRValue(a), (float)GetRValue(b), t);
    int g = (int)Lerp((float)GetGValue(a), (float)GetGValue(b), t);
    int bl = (int)Lerp((float)GetBValue(a), (float)GetBValue(b), t);
    return RGB(r, g, bl);
}
static bool HitRect(int x, int y, int w, int h, int mx, int my) { return mx >= x && mx <= x + w && my >= y && my <= y + h; }

static void pFill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{x,y,x+w,y+h}; HBRUSH br = CreateSolidBrush(c); FillRect(dc, &r, br); DeleteObject(br);
}
static void pText(HDC dc, int x, int y, const char* s, COLORREF c, HFONT f) {
    HFONT old = (HFONT)SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, x, y, s, (int)strlen(s)); SelectObject(dc, old);
}
static void pRoundRect(HDC dc, int x, int y, int w, int h, int rx, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, br); HPEN oldPn = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, x, y, x+w, y+h, rx, rx);
    SelectObject(dc, oldBr); SelectObject(dc, oldPn); DeleteObject(br); DeleteObject(pen);
}
static void pToggle(HDC dc, int x, int y, bool, float pos) {
    COLORREF c = LerpColor(P_OFF, P_ON, pos);
    HBRUSH bg = CreateSolidBrush(c); HPEN np = CreatePen(PS_NULL, 0, 0);
    HPEN op = (HPEN)SelectObject(dc, np); HBRUSH ob = (HBRUSH)SelectObject(dc, bg);
    RoundRect(dc, x, y, x+36, y+18, 18, 18);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(bg); DeleteObject(np);
    
    int kx = x + 4 + static_cast<int>(pos * 18.0f);
    HBRUSH kb = CreateSolidBrush(RGB(244,244,245)); HPEN kp = CreatePen(PS_NULL,0,0);
    op = (HPEN)SelectObject(dc,kp); ob = (HBRUSH)SelectObject(dc,kb);
    Ellipse(dc, kx, y+3, kx+12, y+15);
    SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(kb); DeleteObject(kp);
}
static void pSwatch(HDC dc, int x, int y, COLORREF c, bool selected) {
    HBRUSH br = CreateSolidBrush(c); HPEN np = CreatePen(PS_NULL,0,0);
    HPEN op = (HPEN)SelectObject(dc, np); HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, x, y, x+18, y+18);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br); DeleteObject(np);
    if (selected) {
        HPEN ring = CreatePen(PS_SOLID, 2, RGB(255,255,255)); HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
        op = (HPEN)SelectObject(dc, ring); ob = (HBRUSH)SelectObject(dc, nb);
        Ellipse(dc, x-2, y-2, x+20, y+20);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(ring);
    }
}

static void paintPanel(HWND hwnd, PanelCtx* ctx) {
    bool tm, nm, hb, armor, skeleton, am, fov, rcs, bhop, autoStrafe, visOnly, ignJump;
    int ep, tp, hp, thick, style, aimKey, bone, targetSel;
    float smooth, rcsVert, rcsHoriz, aimFov;
    {
        std::lock_guard lk(*ctx->mtx);
        tm    = ctx->state->showTeammates; nm    = ctx->state->showNames;
        hb    = ctx->state->showHealthBars; armor = ctx->state->showArmor;
        skeleton = ctx->state->showSkeleton; style = ctx->state->boxStyle;
        thick = ctx->state->boxThickness;

        am    = ctx->state->aimbotEnabled; fov   = ctx->state->showFOVCircle;
        aimFov = ctx->state->aimbotFOV; smooth = ctx->state->aimbotSmoothness;
        rcs   = ctx->state->rcsEnabled; rcsVert = ctx->state->rcsVertical;
        rcsHoriz = ctx->state->rcsHorizontal; aimKey = ctx->state->aimKeybind;
        bone  = ctx->state->targetBone; targetSel = ctx->state->targetSelection;
        visOnly = ctx->state->aimVisibleOnly; ignJump = ctx->state->aimIgnoreJumping;

        bhop  = ctx->state->bhopEnabled; autoStrafe = ctx->state->bhopAutoStrafe;

        ep    = ctx->state->enemyPreset; tp    = ctx->state->teamPreset; hp    = ctx->state->healthPreset;
    }

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - ctx->lastFrame).count();
    ctx->lastFrame = now;
    if (dt > 0.1f) dt = 0.1f;
    ctx->time += dt;

    ctx->bgAlpha = Lerp(ctx->bgAlpha, ctx->isClosing ? 0.0f : 255.0f, dt * 7.0f);
    ctx->slideOffset = Lerp(ctx->slideOffset, ctx->isClosing ? -40.0f : 0.0f, dt * 7.0f);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)ctx->bgAlpha, LWA_ALPHA);

    PAINTSTRUCT ps;
    HDC realDc = BeginPaint(hwnd, &ps);
    HDC dc     = CreateCompatibleDC(realDc);
    HBITMAP bmp    = CreateCompatibleBitmap(realDc, PNL_W, PNL_H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    int offX = (int)ctx->slideOffset;
    pFill(dc, offX, 0, SIDEBAR_W, PNL_H, P_SIDEBAR);
    pFill(dc, offX + SIDEBAR_W, 0, PNL_W - SIDEBAR_W, PNL_H, P_BG);
    pFill(dc, offX + SIDEBAR_W, 0, 1, PNL_H, P_BORDER);

    {
        HFONT old = (HFONT)SelectObject(dc, ctx->fLarge);
        SIZE s1{}; GetTextExtentPoint32A(dc, "FLOYD", 5, &s1);
        SetTextColor(dc, P_ACCENT); TextOutA(dc, offX + 24, 24, "FLOYD", 5);
        SetTextColor(dc, P_TEXT);   TextOutA(dc, offX + 24+s1.cx, 24, "SENSE", 5);
        SelectObject(dc, old);
    }
    
    static const char* kTabLabels[4] = {"ESP Visuals", "Aimbot", "Movement", "Settings"};
    for (int i = 0; i < 4; ++i) {
        ctx->tabHoverT[i] = Lerp(ctx->tabHoverT[i], (ctx->activeTab == i || ctx->hoverTab == i) ? 1.0f : 0.0f, dt * 10.0f);
        int ty = 80 + i * 48;
        pFill(dc, offX, ty, SIDEBAR_W, 48, LerpColor(P_SIDEBAR, P_SECTION, ctx->tabHoverT[i]));
        if (ctx->activeTab == i) pFill(dc, offX, ty, 3, 48, P_ACCENT);
        pText(dc, offX + 24, ty + 16, kTabLabels[i], (ctx->activeTab == i) ? P_TEXT : P_SUB, ctx->fTitle);
    }
    
    int cX = offX + SIDEBAR_W;
    int cW = PNL_W - SIDEBAR_W;
    
    pFill(dc, cX, 0, cW, 64, P_BG);
    pText(dc, cX + 32, 24, kTabLabels[ctx->activeTab], P_TEXT, ctx->fLarge);
    pFill(dc, cX, 63, cW, 1, P_BORDER);
    pText(dc, cX + cW - 120, 28, "[HOME] to hide", P_SUB, ctx->fSmall);

    ctx->elements.clear();

    auto addElement = [&](int id, int type, int x, int y, int w, int h, void* data = nullptr, float minV = 0, float maxV = 0) {
        ctx->elements.push_back({id, type, {x, y, x+w, y+h}, data, minV, maxV});
    };

    auto doToggle = [&](int id, const char* label, bool val, int x, int y, int w = 240) {
        ctx->rowHoverT[id] = Lerp(ctx->rowHoverT[id], ctx->hoverRow == id ? 1.0f : 0.0f, dt * 8.0f);
        ctx->togglePos[id] = Lerp(ctx->togglePos[id], val ? 1.0f : 0.0f, dt * 12.0f);
        pFill(dc, x, y, w, 32, LerpColor(P_BG, P_SECTION, ctx->rowHoverT[id]));
        pText(dc, x + 12, y + 8, label, P_TEXT, ctx->fBody);
        pToggle(dc, x + w - 46, y + 7, val, ctx->togglePos[id]);
        addElement(id, 0, x, y, w, 32, nullptr);
    };

    auto doSlider = [&](int id, const char* label, float val, float minV, float maxV, const char* fmt, int x, int y, int w = 240) {
        ctx->rowHoverT[id] = Lerp(ctx->rowHoverT[id], ctx->hoverRow == id ? 1.0f : 0.0f, dt * 8.0f);
        pFill(dc, x, y, w, 46, LerpColor(P_BG, P_SECTION, ctx->rowHoverT[id]));
        pText(dc, x + 12, y + 6, label, P_TEXT, ctx->fBody);
        char buf[32]; int valInt = (int)val; int valDec = std::abs((int)((val - valInt) * 10.f));
        if (strchr(fmt, '.')) wsprintfA(buf, fmt, valInt, valDec);
        else wsprintfA(buf, fmt, valInt);
        SIZE sz{}; GetTextExtentPoint32A(dc, buf, (int)strlen(buf), &sz);
        pText(dc, x + w - 12 - sz.cx, y + 6, buf, P_SUB, ctx->fSmall);
        int barX = x + 12, barY = y + 30, barW = w - 24;
        pFill(dc, barX, barY, barW, 4, P_OFF);
        float pct = std::clamp((val - minV) / (maxV - minV), 0.0f, 1.0f);
        pFill(dc, barX, barY, (int)(barW * pct), 4, P_ACCENT);
        int thumbX = barX + (int)(barW * pct) - 6;
        pRoundRect(dc, thumbX, barY - 4, 12, 12, 6, P_TEXT, P_TEXT);
        addElement(id, 1, x, y, w, 46, nullptr, minV, maxV);
    };

    auto doButton = [&](int id, const char* label, const char* valStr, int x, int y, int w = 240) {
        ctx->rowHoverT[id] = Lerp(ctx->rowHoverT[id], ctx->hoverRow == id ? 1.0f : 0.0f, dt * 8.0f);
        pFill(dc, x, y, w, 32, LerpColor(P_BG, P_SECTION, ctx->rowHoverT[id]));
        pText(dc, x + 12, y + 8, label, P_TEXT, ctx->fBody);
        SIZE sz{}; GetTextExtentPoint32A(dc, valStr, (int)strlen(valStr), &sz);
        pText(dc, x + w - 12 - sz.cx, y + 8, valStr, P_ACCENT, ctx->fBody);
        addElement(id, 2, x, y, w, 32, nullptr);
    };

    auto doCard = [&](const char* title, int x, int y, int w, int h) {
        pRoundRect(dc, x, y, w, h, 6, P_BG, P_BORDER);
        pText(dc, x + 16, y + 12, title, P_SUB, ctx->fSmall);
        pFill(dc, x, y + 32, w, 1, P_BORDER);
    };

    if (ctx->activeTab == 0) {
        doCard("OVERLAY", cX + 24, 88, 250, 210);
        doToggle(10, "Show Teammates", tm, cX + 28, 128);
        doToggle(11, "Show Names", nm, cX + 28, 160);
        doToggle(12, "Show Health Bars", hb, cX + 28, 192);
        doToggle(13, "Show Shield Bars", armor, cX + 28, 224);
        doToggle(14, "Skeleton ESP", skeleton, cX + 28, 256);

        doCard("BOX STYLE", cX + 294, 88, 250, 114);
        const char* sStr = style == 1 ? "2D Box" : style == 2 ? "Corner Box" : "Off";
        doButton(15, "Style", sStr, cX + 298, 128);
        char thickStr[16]; wsprintfA(thickStr, "%d px", thick);
        doButton(16, "Thickness", thickStr, cX + 298, 160);

    } else if (ctx->activeTab == 1) {
        doCard("ASSISTANCE", cX + 24, 88, 250, 242);
        doToggle(20, "Enable Aimbot", am, cX + 28, 128);
        doButton(21, "Aim Keybind", ctx->listeningKey ? "..." : VKToString(aimKey).c_str(), cX + 28, 160);
        const char* boneStr = bone == 6 ? "Head" : bone == 5 ? "Neck" : bone == 4 ? "Chest" : "Stomach";
        doButton(22, "Target Joint", boneStr, cX + 28, 192);
        const char* tsStr = targetSel == 0 ? "FOV" : targetSel == 1 ? "Distance" : "Health";
        doButton(23, "Target Selection", tsStr, cX + 28, 224);
        doToggle(24, "Visible Only", visOnly, cX + 28, 256);
        doToggle(25, "Ignore Jumping", ignJump, cX + 28, 288);

        doCard("CONFIGURATION", cX + 294, 88, 250, 180);
        doToggle(26, "Show FOV Circle", fov, cX + 298, 128);
        doSlider(27, "Aimbot FOV", aimFov, 0.5f, 30.0f, "%d.%d\xb0", cX + 298, 160);
        doSlider(28, "Smoothness", smooth, 1.0f, 30.0f, "%d.%d", cX + 298, 206);

        doCard("RECOIL CONTROL", cX + 24, 344, 520, 150);
        doToggle(29, "Enable RCS", rcs, cX + 28, 384, 512);
        doSlider(30, "Vertical Force", rcsVert, 0.0f, 100.0f, "%d%%", cX + 28, 416, 250);
        doSlider(31, "Horizontal Force", rcsHoriz, 0.0f, 100.0f, "%d%%", cX + 290, 416, 250);

    } else if (ctx->activeTab == 2) {
        doCard("ASSISTANCE", cX + 24, 88, 250, 114);
        doToggle(40, "Bunnyhop", bhop, cX + 28, 128);
        doToggle(41, "Auto-Strafer", autoStrafe, cX + 28, 160);
        
    } else if (ctx->activeTab == 3) {
        static const char* kSwLabels[3] = {"Enemy Color", "Team Color", "Health Color"};
        const int kPresets[3] = {ep, tp, hp};
        const COLORREF* kArrays[3] = {kEnemyColors, kTeamColors, kHealthColors};

        for (int row = 0; row < 3; ++row) {
            int ry = 88 + row * 46;
            pText(dc, cX + 24, ry + 16, kSwLabels[row], P_SUB, ctx->fBody);
            for (int col = 0; col < 6; ++col) {
                int sx = cX + 130 + col * 32;
                pSwatch(dc, sx, ry + 13, kArrays[row][col], kPresets[row] == col);
                addElement(50 + row*6 + col, 3, sx - 2, ry + 11, 24, 24);
            }
        }

        int ry = PNL_H - 72;
        ctx->rowHoverT[60] = Lerp(ctx->rowHoverT[60], ctx->hoverRow == 60 ? 1.0f : 0.0f, dt * 10.0f);
        COLORREF btnC = LerpColor(RGB(180, 20, 40), RGB(230, 40, 60), ctx->rowHoverT[60]);
        pRoundRect(dc, cX + 24, ry, cW - 48, 44, 6, btnC, RGB(255, 60, 80));
        const char* shutStr = "SHUTDOWN FLOYDSENSE";
        SIZE sz{}; GetTextExtentPoint32A(dc, shutStr, (int)strlen(shutStr), &sz);
        pText(dc, cX + 24 + (cW - 48 - sz.cx)/2, ry + 14, shutStr, P_TEXT, ctx->fTitle);
        addElement(60, 2, cX + 24, ry, cW - 48, 44);
    }

    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN   bp = CreatePen(PS_SOLID, 1, P_BORDER);
    HPEN   op = (HPEN)SelectObject(dc, bp);
    HBRUSH ob = (HBRUSH)SelectObject(dc, nb);
    Rectangle(dc, 0, 0, PNL_W, PNL_H);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(bp);

    BitBlt(realDc, 0, 0, PNL_W, PNL_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PanelCtx* ctx = reinterpret_cast<PanelCtx*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_PAINT:
        if (ctx) paintPanel(hwnd, ctx); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTCLIENT;

    case WM_LBUTTONDOWN: {
        if (!ctx) break;
        int mx = LOWORD(lp), my = HIWORD(lp);
        if (ctx->listeningKey) return 0;

        if (my < 64) {
            ctx->isDragging = true;
            GetCursorPos(&ctx->dragStartMouse);
            RECT r; GetWindowRect(hwnd, &r);
            ctx->dragStartWindow = { r.left, r.top };
            SetCapture(hwnd);
            return 0;
        }

        if (mx < SIDEBAR_W) {
            for (int i=0; i<4; ++i) {
                if (my >= 80+i*48 && my < 80+(i+1)*48) {
                    ctx->activeTab = i; ctx->hoverRow = -1;
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
            }
        } else {
            std::lock_guard lk(*ctx->mtx);
            for (auto& el : ctx->elements) {
                if (HitRect(el.bounds.left, el.bounds.top, el.bounds.right - el.bounds.left, el.bounds.bottom - el.bounds.top, mx, my)) {
                    if (el.type == 0) {
                        int id = el.id;
                        if (id==10) ctx->state->showTeammates = !ctx->state->showTeammates;
                        if (id==11) ctx->state->showNames = !ctx->state->showNames;
                        if (id==12) ctx->state->showHealthBars = !ctx->state->showHealthBars;
                        if (id==13) ctx->state->showArmor = !ctx->state->showArmor;
                        if (id==14) ctx->state->showSkeleton = !ctx->state->showSkeleton;
                        if (id==20) ctx->state->aimbotEnabled = !ctx->state->aimbotEnabled;
                        if (id==24) ctx->state->aimVisibleOnly = !ctx->state->aimVisibleOnly;
                        if (id==25) ctx->state->aimIgnoreJumping = !ctx->state->aimIgnoreJumping;
                        if (id==26) ctx->state->showFOVCircle = !ctx->state->showFOVCircle;
                        if (id==29) ctx->state->rcsEnabled = !ctx->state->rcsEnabled;
                        if (id==40) ctx->state->bhopEnabled = !ctx->state->bhopEnabled;
                        if (id==41) ctx->state->bhopAutoStrafe = !ctx->state->bhopAutoStrafe;
                    } else if (el.type == 1) {
                        ctx->activeSlider = el.id;
                        float pct = std::clamp((float)(mx - el.bounds.left - 12) / (el.bounds.right - el.bounds.left - 24), 0.0f, 1.0f);
                        float val = el.minV + pct * (el.maxV - el.minV);
                        if (el.id==27) ctx->state->aimbotFOV = val;
                        if (el.id==28) ctx->state->aimbotSmoothness = val;
                        if (el.id==30) ctx->state->rcsVertical = val;
                        if (el.id==31) ctx->state->rcsHorizontal = val;
                        SetCapture(hwnd);
                    } else if (el.type == 2) {
                        int id = el.id;
                        if (id==15) { ctx->state->boxStyle = (ctx->state->boxStyle + 1) % 3; }
                        if (id==16) { ctx->state->boxThickness++; if(ctx->state->boxThickness>3) ctx->state->boxThickness=1; }
                        if (id==21) { ctx->listeningKey = true; }
                        if (id==22) { 
                            int c = ctx->state->targetBone;
                            ctx->state->targetBone = (c==6)?5 : (c==5)?4 : (c==4)?2 : 6;
                        }
                        if (id==23) { ctx->state->targetSelection = (ctx->state->targetSelection + 1) % 3; }
                        if (id==60) { ctx->state->exitRequested = true; ctx->isClosing = true; }
                    } else if (el.type == 3) {
                        int id = el.id;
                        if (id >= 50 && id < 56) ctx->state->enemyPreset = id - 50;
                        if (id >= 56 && id < 62) ctx->state->teamPreset = id - 56;
                        if (id >= 62 && id < 68) ctx->state->healthPreset = id - 62;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!ctx) break;
        if (ctx->isDragging) { ctx->isDragging = false; ReleaseCapture(); return 0; }
        if (ctx->activeSlider != -1) { ctx->activeSlider = -1; ReleaseCapture(); return 0; }
        break;
    }

    case WM_MOUSEMOVE: {
        if (!ctx) break;
        int mx = LOWORD(lp), my = HIWORD(lp);

        if (ctx->isDragging) {
            POINT pt; GetCursorPos(&pt);
            int dx = pt.x - ctx->dragStartMouse.x;
            int dy = pt.y - ctx->dragStartMouse.y;
            SetWindowPos(hwnd, nullptr, ctx->dragStartWindow.x + dx, ctx->dragStartWindow.y + dy, 0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            return 0;
        }

        if (ctx->activeSlider != -1) {
            std::lock_guard lk(*ctx->mtx);
            for (auto& el : ctx->elements) {
                if (el.id == ctx->activeSlider && el.type == 1) {
                    float pct = std::clamp((float)(mx - el.bounds.left - 12) / (el.bounds.right - el.bounds.left - 24), 0.0f, 1.0f);
                    float val = el.minV + pct * (el.maxV - el.minV);
                    if (el.id==27) ctx->state->aimbotFOV = val;
                    if (el.id==28) ctx->state->aimbotSmoothness = val;
                    if (el.id==30) ctx->state->rcsVertical = val;
                    if (el.id==31) ctx->state->rcsHorizontal = val;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }

        int newHoverTab = -1, newHoverRow = -1;
        if (mx < SIDEBAR_W) {
            for (int i=0; i<4; ++i) if (my >= 80+i*48 && my < 80+(i+1)*48) newHoverTab = i;
        } else {
            for (auto& el : ctx->elements) {
                if (HitRect(el.bounds.left, el.bounds.top, el.bounds.right - el.bounds.left, el.bounds.bottom - el.bounds.top, mx, my)) {
                    newHoverRow = el.id; break;
                }
            }
        }
        
        if (newHoverTab != ctx->hoverTab || newHoverRow != ctx->hoverRow) {
            ctx->hoverTab = newHoverTab; ctx->hoverRow = newHoverRow;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (ctx && (ctx->hoverRow != -1 || ctx->hoverTab != -1)) {
            ctx->hoverRow = -1; ctx->hoverTab = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_TIMER: {
        if (ctx && ctx->listeningKey) {
            for (int i = 1; i < 256; ++i) {
                if (i == VK_HOME || i == VK_ESCAPE || i == VK_LBUTTON) continue;
                if (GetAsyncKeyState(i) & 0x8000) {
                    std::lock_guard lk(*ctx->mtx);
                    ctx->state->aimKeybind = i; ctx->listeningKey = false;
                    break;
                }
            }
        }
        if (ctx && ctx->isClosing && ctx->bgAlpha <= 1.0f) { DestroyWindow(hwnd); } 
        else { InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }

    case WM_DESTROY:
        if (ctx) {
            DeleteObject(ctx->fTitle); DeleteObject(ctx->fLarge);
            DeleteObject(ctx->fBody); DeleteObject(ctx->fSmall);
            delete ctx;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND createPanelHwnd(HINSTANCE hInst, std::shared_ptr<AppState> state, std::shared_ptr<std::mutex> mtx, int ox, int oy) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = PanelProc; wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "FsPanel";
        RegisterClassExA(&wc); registered = true;
    }
    auto* ctx = new PanelCtx{};
    ctx->state = state; ctx->mtx = mtx;
    ctx->fTitle = CreateFontA(14,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    ctx->fLarge = CreateFontA(24,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    ctx->fBody  = CreateFontA(13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
    ctx->fSmall = CreateFontA(12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");

    HWND hwnd = CreateWindowExA(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_APPWINDOW, "FsPanel", "FloydSense Panel",
        WS_POPUP|WS_VISIBLE, ox+20, oy+20, PNL_W, PNL_H, nullptr, nullptr, hInst, nullptr);

    if (!hwnd) { delete ctx; return nullptr; }
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    SetTimer(hwnd, 1, 16, nullptr);
    return hwnd;
}

struct RainDrop {
    float x, y, speed, length;
    COLORREF color;
};

void overlayThreadProc(
    std::shared_ptr<AppState>    state,
    std::shared_ptr<std::mutex>  mtx,
    const std::atomic<bool>&     stop)
{
    // Locate CS2 window for initial position and re-anchoring
    HWND gameWnd = FindWindowW(nullptr, L"Counter-Strike 2");

    int ox = 0, oy = 0, ow, oh;
    {
        RECT r{};
        if (gameWnd && GetWindowRect(gameWnd, &r)) {
            ox = r.left; oy = r.top;
            ow = static_cast<int>(std::max(1L, r.right  - r.left));
            oh = static_cast<int>(std::max(1L, r.bottom - r.top));
        } else {
            ow = GetSystemMetrics(SM_CXSCREEN);
            oh = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    HINSTANCE hInst = GetModuleHandleA(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"FsEspOverlay";
    RegisterClassExW(&wc);

    // Create without WS_VISIBLE — colorkey must be applied before the first paint.
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"FsEspOverlay", nullptr,
        WS_POPUP,
        ox, oy, ow, oh,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        UnregisterClassW(L"FsEspOverlay", hInst);
        return;
    }

    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    // Create black dimming overlay window
    HWND dimHwnd = nullptr;
    {
        WNDCLASSEXW wcdim{};
        wcdim.cbSize        = sizeof(wcdim);
        wcdim.lpfnWndProc   = DefWindowProcW;
        wcdim.hInstance     = hInst;
        wcdim.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wcdim.lpszClassName = L"FsDimOverlay";
        RegisterClassExW(&wcdim);

        dimHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"FsDimOverlay", nullptr,
            WS_POPUP,
            ox, oy, ow, oh,
            nullptr, nullptr, hInst, nullptr);
        if (dimHwnd) {
            SetLayeredWindowAttributes(dimHwnd, 0, 110, LWA_ALPHA); // ~43% opacity
            ShowWindow(dimHwnd, SW_HIDE);
        }
    }

    // Double-buffered GDI
    HDC     hdcWin = GetDC(hwnd);
    HDC     hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP hbmp   = CreateCompatibleBitmap(hdcWin, ow, oh);
    SelectObject(hdcMem, hbmp);
    ReleaseDC(hwnd, hdcWin);

    // ESP label font (Tahoma 13px for health numbers and names)
    HFONT hFont = CreateFontA(
        13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
        "Tahoma");
    HFONT prevFont = static_cast<HFONT>(SelectObject(hdcMem, hFont));
    SetBkMode(hdcMem, TRANSPARENT);

    RegisterHotKey(hwnd, HK_PANEL,     0, VK_HOME);
    RegisterHotKey(hwnd, HK_TEAMMATES, 0, VK_F1);
    RegisterHotKey(hwnd, HK_HEALTH,    0, VK_F2);

    HWND panelHwnd  = nullptr;
    RECT lastRect{ ox, oy, ox + ow, oy + oh };

    std::optional<Process> proc;
    uintptr_t clientBase = 0;
    auto lastStatusTick  = std::chrono::steady_clock::now();

    // Initialize rain system
    std::vector<RainDrop> rain;
    for (int i = 0; i < 200; ++i) {
        rain.push_back({
            (float)(rand() % (ow > 0 ? ow : 2000)), (float)(rand() % (oh > 0 ? oh : 1000)),
            10.0f + (rand() % 20), 15.0f + (rand() % 30),
            RGB(0, 100 + rand() % 155, 150 + rand() % 105)
        });
    }

    while (!stop.load(std::memory_order_relaxed)) {
        // Shut down cleanly if requested via the GUI button
        bool exitReq = false;
        {
            std::lock_guard lk(*mtx);
            exitReq = state->exitRequested;
        }
        if (exitReq) break;

        // Shutdown hotkey: END key closes the cheat completely
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            std::lock_guard lk(*mtx);
            state->exitRequested = true;
            break;
        }

        // Drain message queue — WM_HOTKEY arrives here
        MSG  msg{};
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            } else if (msg.message == WM_HOTKEY) {
                switch (static_cast<int>(msg.wParam)) {
                case HK_PANEL:
                    if (panelHwnd && IsWindow(panelHwnd)) {
                        PanelCtx* ctx = reinterpret_cast<PanelCtx*>(GetWindowLongPtrA(panelHwnd, GWLP_USERDATA));
                        if (ctx) ctx->isClosing = true;
                        panelHwnd = nullptr;
                    } else {
                        panelHwnd = createPanelHwnd(hInst, state, mtx, ox, oy);
                    }
                    break;
                case HK_TEAMMATES: {
                    std::lock_guard lk(*mtx);
                    state->showTeammates = !state->showTeammates;
                    break;
                }
                case HK_HEALTH: {
                    std::lock_guard lk(*mtx);
                    state->showHealthBars = !state->showHealthBars;
                    break;
                }
                }
            } else {
                DispatchMessageW(&msg);
            }
        }
        if (quit) break;

        // Synchronize dim window visibility with menu window visibility
        if (dimHwnd) {
            bool menuOpen = (panelHwnd && IsWindow(panelHwnd));
            bool isVisible = IsWindowVisible(dimHwnd);
            if (menuOpen && !isVisible) {
                ShowWindow(dimHwnd, SW_SHOWNOACTIVATE);
            } else if (!menuOpen && isVisible) {
                ShowWindow(dimHwnd, SW_HIDE);
            }
        }

        // Re-anchor overlay when CS2 window moves or resizes
        if (gameWnd) {
            RECT r{};
            if (GetWindowRect(gameWnd, &r)
                && (r.left   != lastRect.left  || r.top    != lastRect.top
                 || r.right  != lastRect.right  || r.bottom != lastRect.bottom))
            {
                lastRect = r;
                ow = static_cast<int>(r.right  - r.left);
                oh = static_cast<int>(r.bottom - r.top);
                SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, ow, oh, SWP_NOACTIVATE);
                if (dimHwnd) SetWindowPos(dimHwnd, HWND_TOPMOST, r.left, r.top, ow, oh, SWP_NOACTIVATE);
                
                DeleteObject(hbmp);
                HDC tmp = GetDC(hwnd);
                hbmp = CreateCompatibleBitmap(tmp, ow, oh);
                ReleaseDC(hwnd, tmp);
                SelectObject(hdcMem, hbmp);
                
                for (auto& d : rain) { d.x = (float)(rand() % ow); d.y = (float)(rand() % oh); }
            }
        }

        // Attach / re-attach to cs2.exe with 1500ms throttle (avoids heavy RPM calls)
        if (!proc) {
            static auto lastAttachTry = std::chrono::steady_clock::now();
            auto nowTime = std::chrono::steady_clock::now();
            if (nowTime - lastAttachTry >= std::chrono::milliseconds(1500)) {
                lastAttachTry = nowTime;
                proc = Process::open("cs2.exe");
                if (proc) {
                    auto cb = proc->moduleBase("client.dll");
                    clientBase = cb.value_or(0);
                    if (!clientBase) proc.reset();
                }
                if (!proc) {
                    std::lock_guard lk(*mtx);
                    state->status = "Waiting for cs2.exe...";
                }
            }
        }

        // Fill with colorkey → transparent
        RECT full{ 0, 0, ow, oh };
        FillRect(hdcMem, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        // Read game data and render
        if (proc && clientBase) {
            auto pawnPtr = proc->read<uintptr_t>(clientBase + off::dwLocalPlayerPawn);
            if (!pawnPtr) {
                // Game closed
                proc.reset(); clientBase = 0;
            } else {
                uintptr_t pawn    = *pawnPtr;
                auto vm           = readViewMatrix(*proc, clientBase);
                int currentTargetBone;
                {
                    std::lock_guard lk2(*mtx);
                    currentTargetBone = state->targetBone;
                }
                auto players      = collectPlayers(*proc, clientBase, pawn, currentTargetBone);
                int32_t localTeam = readLocalTeam(*proc, pawn);

                // Read local eye position
                float lx = proc->read<float>(pawn + sch::C_BasePlayerPawn::m_vOldOrigin).value_or(0.f);
                float ly = proc->read<float>(pawn + sch::C_BasePlayerPawn::m_vOldOrigin + 4).value_or(0.f);
                float lz = proc->read<float>(pawn + sch::C_BasePlayerPawn::m_vOldOrigin + 8).value_or(0.f);
                float vx = proc->read<float>(pawn + sch::C_BaseModelEntity::m_vecViewOffset).value_or(0.f);
                float vy = proc->read<float>(pawn + sch::C_BaseModelEntity::m_vecViewOffset + 4).value_or(0.f);
                float vz = proc->read<float>(pawn + sch::C_BaseModelEntity::m_vecViewOffset + 8).value_or(0.f);
                Vec3 localEye = { lx + vx, ly + vy, lz + vz };

                bool doAim, doRcs, doBhop, autoStrafe, visOnly, ignJump;
                int aimKey, targetSel; float aimFov, smooth, rcsVert, rcsHoriz;
                {
                    std::lock_guard lk2(*mtx);
                    doAim  = state->aimbotEnabled;
                    doRcs  = state->rcsEnabled;
                    doBhop = state->bhopEnabled;
                    autoStrafe = state->bhopAutoStrafe;
                    aimKey = state->aimKeybind;
                    aimFov = state->aimbotFOV;
                    smooth = state->aimbotSmoothness;
                    rcsVert = state->rcsVertical;
                    rcsHoriz = state->rcsHorizontal;
                    targetSel = state->targetSelection;
                    visOnly = state->aimVisibleOnly;
                    ignJump = state->aimIgnoreJumping;
                }

                // Bunnyhop Logic with Directional Auto-Strafer
                if (doBhop && (GetAsyncKeyState(VK_SPACE) & 0x8000)) {
                    auto flags = proc->read<uint32_t>(pawn + sch::C_BaseEntity::m_fFlags);
                    if (flags) {
                        bool onGround = (*flags & (1 << 0));
                        if (onGround) {
                            proc->write<int>(clientBase + btn::jump, 65537); // +jump
                        } else {
                            proc->write<int>(clientBase + btn::jump, 256);   // -jump

                            if (autoStrafe) {
                                auto viewAngles = proc->read<Vector2>(clientBase + off::dwViewAngles);
                                if (viewAngles) {
                                    static float lastYaw = 0.0f;
                                    float currentYaw = viewAngles->y;
                                    float diff = currentYaw - lastYaw;
                                    if (diff > 180.0f) diff -= 360.0f;
                                    if (diff < -180.0f) diff += 360.0f;

                                    if (diff > 0.05f) { // Turning left
                                        proc->write<int>(clientBase + btn::left, 65537);
                                        proc->write<int>(clientBase + btn::right, 256);
                                    } else if (diff < -0.05f) { // Turning right
                                        proc->write<int>(clientBase + btn::right, 65537);
                                        proc->write<int>(clientBase + btn::left, 256);
                                    }
                                    lastYaw = currentYaw;
                                }
                            }
                        }
                    }
                } else if (doBhop) {
                    proc->write<int>(clientBase + btn::left, 256);
                    proc->write<int>(clientBase + btn::right, 256);
                }

                // Aimbot and RCS Logic
                static Vector2 oldPunch = {0.f, 0.f};
                bool rcsAppliedByAimbot = false;
                Vector2 currentPunchAngle = {0.f, 0.f};

                if (doRcs) {
                    auto aimPunchSvc = proc->read<uintptr_t>(pawn + sch::C_CSPlayerPawn::m_pAimPunchServices);
                    if (aimPunchSvc && *aimPunchSvc) {
                        auto count = proc->read<uint32_t>(*aimPunchSvc + 0x70);
                        if (count && *count > 0 && *count < 0xFFFF) {
                            auto cachePtr = proc->read<uintptr_t>(*aimPunchSvc + 0x78);
                            if (cachePtr && *cachePtr) {
                                auto punch = proc->read<Vector2>(*cachePtr + (*count - 1) * 12);
                                if (punch) {
                                    currentPunchAngle.x = punch->x * 2.0f * (rcsVert / 100.f);
                                    currentPunchAngle.y = punch->y * 2.0f * (rcsHoriz / 100.f);
                                }
                            }
                        }
                    }
                }

                if (doAim && aimKey != 0 && (GetAsyncKeyState(aimKey) & 0x8000)) {
                    auto viewAngles = proc->read<Vector2>(clientBase + off::dwViewAngles);
                    if (viewAngles) {
                        float bestMetric = 999999.f;
                        Vector2 bestAngle = {0.f, 0.f};
                        bool targetFound = false;

                        for (const auto& p : players) {
                            if (p.team == localTeam) continue;
                            if (visOnly && !p.isSpotted) continue;
                            
                            Vector2 aimAngle = CalcAngle(localEye, p.bonePos);
                            float fov = GetFOV(*viewAngles, aimAngle);
                            
                            if (fov < aimFov) {
                                float metric = fov;
                                if (targetSel == 1) { // Distance
                                    float dx = p.pos.x - localEye.x;
                                    float dy = p.pos.y - localEye.y;
                                    float dz = p.pos.z - localEye.z;
                                    metric = std::sqrt(dx*dx + dy*dy + dz*dz);
                                } else if (targetSel == 2) { // Health
                                    metric = (float)p.health;
                                }

                                if (metric < bestMetric) {
                                    bestMetric = metric;
                                    bestAngle = aimAngle;
                                    targetFound = true;
                                }
                            }
                        }

                        if (targetFound) {
                            if (doRcs) {
                                bestAngle.x -= currentPunchAngle.x;
                                bestAngle.y -= currentPunchAngle.y;
                                
                                if (bestAngle.x > 89.0f) bestAngle.x = 89.0f;
                                if (bestAngle.x < -89.0f) bestAngle.x = -89.0f;
                                while (bestAngle.y > 180.0f) bestAngle.y -= 360.0f;
                                while (bestAngle.y < -180.0f) bestAngle.y += 360.0f;
                                rcsAppliedByAimbot = true;
                            }
                            
                            if (smooth < 1.0f) smooth = 1.0f;
                            Vector2 delta = { bestAngle.x - viewAngles->x, bestAngle.y - viewAngles->y };
                            while (delta.y > 180.f) delta.y -= 360.f;
                            while (delta.y < -180.f) delta.y += 360.f;
                            
                            Vector2 smoothedAngle = {
                                viewAngles->x + delta.x / smooth,
                                viewAngles->y + delta.y / smooth
                            };
                            
                            if (smoothedAngle.x > 89.0f) smoothedAngle.x = 89.0f;
                            if (smoothedAngle.x < -89.0f) smoothedAngle.x = -89.0f;
                            while (smoothedAngle.y > 180.0f) smoothedAngle.y -= 360.0f;
                            while (smoothedAngle.y < -180.0f) smoothedAngle.y += 360.0f;

                            proc->write<Vector2>(clientBase + off::dwViewAngles, smoothedAngle);
                        }
                    }
                }

                // Standalone RCS
                if (doRcs) {
                    auto shotsFired = proc->read<int32_t>(pawn + sch::C_CSPlayerPawn::m_iShotsFired);
                    if (shotsFired && *shotsFired > 0) {
                        if (!rcsAppliedByAimbot) {
                            auto viewAngles = proc->read<Vector2>(clientBase + off::dwViewAngles);
                            if (viewAngles) {
                                Vector2 delta = {
                                    currentPunchAngle.x - oldPunch.x,
                                    currentPunchAngle.y - oldPunch.y
                                };
                                
                                Vector2 newAngle = {
                                    viewAngles->x - delta.x,
                                    viewAngles->y - delta.y
                                };
                                
                                if (newAngle.x > 89.0f) newAngle.x = 89.0f;
                                if (newAngle.x < -89.0f) newAngle.x = -89.0f;
                                while (newAngle.y > 180.0f) newAngle.y -= 360.0f;
                                while (newAngle.y < -180.0f) newAngle.y += 360.0f;
                                
                                proc->write<Vector2>(clientBase + off::dwViewAngles, newAngle);
                            }
                        }
                        oldPunch = currentPunchAngle;
                    } else {
                        oldPunch = {0.f, 0.f};
                    }
                } else {
                    oldPunch = {0.f, 0.f};
                }

                // Update launcher status at ~2 Hz
                auto now = std::chrono::steady_clock::now();
                if (now - lastStatusTick >= std::chrono::milliseconds(500)) {
                    lastStatusTick = now;
                    std::lock_guard lk(*mtx);
                    state->players   = players;
                    state->localTeam = localTeam;
                    if (vm) state->viewMatrix = *vm;
                    state->status = "Attached  \xe2\x80\x94  "
                                  + std::to_string(players.size()) + " players visible";
                }

                // Read advanced customization settings
                bool showNames, showArmor, showSkeleton; int boxThick, boxStyle;
                COLORREF enemyC, teamC, healthC;
                {
                    std::lock_guard lk2(*mtx);
                    showNames    = state->showNames;
                    showArmor    = state->showArmor;
                    showSkeleton = state->showSkeleton;
                    boxThick     = state->boxThickness;
                    boxStyle     = state->boxStyle;
                    enemyC       = kEnemyColors [state->enemyPreset];
                    teamC        = kTeamColors  [state->teamPreset];
                    healthC      = kHealthColors[state->healthPreset];
                }

                bool vmZero = !vm;
                if (vm) for (float v : *vm) { if (v != 0.f) { vmZero = false; break; } }

                if (!vmZero) {
                    drawEsp(hdcMem, players, *vm,
                            static_cast<float>(ow), static_cast<float>(oh),
                            state->showTeammates, state->showHealthBars, showNames, showArmor, showSkeleton,
                            localTeam, boxThick, boxStyle, enemyC, teamC, healthC, hFont);

                    bool drawFov; float fovRad;
                    {
                        std::lock_guard lk2(*mtx);
                        drawFov = state->showFOVCircle;
                        fovRad = state->aimbotFOV;
                    }
                    if (drawFov) {
                        float cx = ow / 2.f;
                        float cy = oh / 2.f;
                        float radius = tan(fovRad * 3.14159f / 360.f) / tan(90.f * 3.14159f / 360.f) * cx;
                        
                        HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100)); // Subtle gray circle
                        HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem, pen));
                        HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdcMem, GetStockObject(NULL_BRUSH)));
                        
                        Ellipse(hdcMem, static_cast<int>(cx - radius), static_cast<int>(cy - radius), 
                                        static_cast<int>(cx + radius), static_cast<int>(cy + radius));
                        
                        SelectObject(hdcMem, oldPen);
                        SelectObject(hdcMem, oldBr);
                        DeleteObject(pen);
                    }
                }
            }
        }

        // Draw Full-screen Rain if menu is active
        if (panelHwnd && IsWindow(panelHwnd)) {
            for (auto& drop : rain) {
                drop.y += drop.speed;
                if (drop.y > oh) {
                    drop.y = -drop.length; drop.x = (float)(rand() % (ow > 0 ? ow : 2000));
                }
                if (drop.x >= 0 && drop.x <= ow) {
                    HPEN pen = CreatePen(PS_SOLID, 1, drop.color);
                    HPEN oldPen = (HPEN)SelectObject(hdcMem, pen);
                    MoveToEx(hdcMem, (int)drop.x, (int)drop.y, nullptr);
                    LineTo(hdcMem, (int)drop.x, (int)(drop.y + drop.length));
                    SelectObject(hdcMem, oldPen); DeleteObject(pen);
                }
            }
        }

        HDC hdcWin2 = GetDC(hwnd);
        BitBlt(hdcWin2, 0, 0, ow, oh, hdcMem, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, hdcWin2);

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 fps
    }

    UnregisterHotKey(hwnd, HK_PANEL);
    UnregisterHotKey(hwnd, HK_TEAMMATES);
    UnregisterHotKey(hwnd, HK_HEALTH);
    if (panelHwnd && IsWindow(panelHwnd)) DestroyWindow(panelHwnd);
    if (dimHwnd && IsWindow(dimHwnd)) DestroyWindow(dimHwnd);
    SelectObject(hdcMem, prevFont);
    DeleteObject(hFont);
    DeleteObject(hbmp);
    DeleteDC(hdcMem);
    DestroyWindow(hwnd);
    UnregisterClassW(L"FsEspOverlay", hInst);
    UnregisterClassW(L"FsDimOverlay", hInst);
}
