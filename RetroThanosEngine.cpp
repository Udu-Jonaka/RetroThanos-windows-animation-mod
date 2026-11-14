#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <math.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <mutex>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 2
#endif

#define SUBCLASS_ID 0x5448414Eu // "THAN"

// Shared configuration across processes via Named Shared Memory
struct SharedConfig {
    HHOOK hHook;
    BOOL  enabled;
    int   minDurationMs;
    int   closeDurationMs;
    int   particleSize;
};

static const WCHAR kSharedMemName[] = L"Local\\RetroThanos_Shared_Config_v1";
static HANDLE g_hSharedMap = nullptr;
static SharedConfig* g_pConfig = nullptr;

static HINSTANCE g_hInstance = nullptr;
static std::mutex g_AnimMutex;

// Property identifiers for window tagging
static constexpr PCWSTR kPropClosed       = L"rt_closed";
static constexpr PCWSTR kPropCloseBypass  = L"rt_close_bypass";
static constexpr PCWSTR kPropMinimizing   = L"rt_minimizing";
static constexpr PCWSTR kPropGhostOverlay = L"rt_ghost_overlay";

template <typename T>
static inline T Clamp(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// Ensure connection to shared settings
static void EnsureSharedConfig() {
    if (g_pConfig) return;
    g_hSharedMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemName);
    if (!g_hSharedMap) {
        g_hSharedMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedConfig), kSharedMemName);
        if (g_hSharedMap) {
            g_pConfig = (SharedConfig*)MapViewOfFile(g_hSharedMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedConfig));
            if (g_pConfig) {
                g_pConfig->hHook = nullptr;
                g_pConfig->enabled = TRUE;
                g_pConfig->minDurationMs = 750;
                g_pConfig->closeDurationMs = 700;
                g_pConfig->particleSize = 4;
            }
        }
    } else {
        g_pConfig = (SharedConfig*)MapViewOfFile(g_hSharedMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedConfig));
    }
}

static inline int GetMinDuration()   { EnsureSharedConfig(); return g_pConfig ? g_pConfig->minDurationMs : 750; }
static inline int GetCloseDuration() { EnsureSharedConfig(); return g_pConfig ? g_pConfig->closeDurationMs : 700; }
static inline int GetParticleSize()  { EnsureSharedConfig(); return g_pConfig ? g_pConfig->particleSize : 4; }
static inline BOOL IsActive()        { EnsureSharedConfig(); return g_pConfig ? g_pConfig->enabled : TRUE; }

// DIB Creation helper (32-bit top-down)
static HBITMAP CreateDib32(HDC dc, int width, int height, void** bits) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void UpdateDwmTransitions(HWND hWnd, BOOL enable) {
    BOOL disable = !enable;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
}

static void SetWindowCloak(HWND hWnd, BOOL cloak) {
    DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
}

static bool ShouldAnimate(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !IsActive()) return false;
    if (GetPropW(hWnd, kPropGhostOverlay)) return false;

    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);

    if (style & WS_CHILD) return false;
    if (!(style & WS_VISIBLE)) return false;
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    WCHAR className[64]{};
    if (GetClassNameW(hWnd, className, ARRAYSIZE(className))) {
        if (wcscmp(className, L"Progman") == 0 ||
            wcscmp(className, L"WorkerW") == 0 ||
            wcscmp(className, L"Shell_TrayWnd") == 0 ||
            wcscmp(className, L"Shell_SecondaryTrayWnd") == 0 ||
            wcsstr(className, L"Tooltip") != nullptr) {
            return false;
        }
    }
    return true;
}

struct ThanosParticle {
    float windX;
    float windY;
    float curveX;
    float curveY;
    float startTime;
    uint32_t nextActive;
};

struct WindowAnimData {
    HWND    hRealWnd{};
    HBITMAP hBitmap{};
    RECT    rect{};
    int     w{};
    int     h{};
    int     durationMs{};
    bool    isClosing{};
    UINT    closeMsg{};
    WPARAM  closeWParam{};
};

class WindowAnimWorker {
public:
    WindowAnimData* data;
    int W{0}, H{0};
    int origLeft{0}, origTop{0};
    int boundLeft{0}, boundTop{0};
    int boundW{0}, boundH{0};
    int blockSizeSetting{4};

    HDC     hScreenDC{nullptr};
    HDC     hCanvasDC{nullptr};
    HBITMAP hCanvas{nullptr};
    HBITMAP hOldCanvas{nullptr};
    BYTE*   pBits{nullptr};
    size_t  canvasStride{0};

    HDC     hSrcDC{nullptr};
    HBITMAP hOldSrc{nullptr};
    HBITMAP hSrcDib{nullptr};
    HDC     hSrcDibDC{nullptr};
    HBITMAP hOldSrcDib{nullptr};
    BYTE*   srcBits{nullptr};
    size_t  srcStride{0};

    HWND    hGhost{nullptr};

    std::vector<ThanosParticle> thanosParticles;
    uint32_t firstActiveParticle{0};
    static constexpr uint32_t kNoActiveParticle = 0xFFFFFFFFu;

    explicit WindowAnimWorker(WindowAnimData* d) : data(d) {
        W = d->w;
        H = d->h;
        origLeft = d->rect.left;
        origTop  = d->rect.top;
        blockSizeSetting = Clamp(GetParticleSize(), 2, 16);

        HMONITOR hMon = MonitorFromWindow(d->hRealWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);

        if (data->isClosing) {
            int padLeft   = 50;
            int padTop    = H / 2;
            int padRight  = (int)(W * 1.5f);
            int padBottom = (int)(H * 1.5f);
            boundLeft   = std::max((int)mi.rcMonitor.left, origLeft - padLeft);
            boundTop    = std::max((int)mi.rcMonitor.top, origTop - padTop);
            int boundRight  = std::min((int)mi.rcMonitor.right, origLeft + W + padRight);
            int boundBottom = std::min((int)mi.rcMonitor.bottom, origTop + H + padBottom);
            boundW = boundRight - boundLeft;
            boundH = boundBottom - boundTop;
        } else {
            int pad = 8;
            boundLeft   = std::max((int)mi.rcMonitor.left, origLeft - pad);
            boundTop    = std::max((int)mi.rcMonitor.top, origTop - pad);
            int boundRight  = std::min((int)mi.rcMonitor.right, origLeft + W + pad);
            int boundBottom = std::min((int)mi.rcMonitor.bottom, origTop + H + pad);
            boundW = boundRight - boundLeft;
            boundH = boundBottom - boundTop;
        }

        canvasStride = (size_t)boundW * 4;
        srcStride    = (size_t)W * 4;
    }

    ~WindowAnimWorker() {
        if (hGhost) {
            DestroyWindow(hGhost);
            hGhost = nullptr;
        }
        if (hCanvasDC) {
            if (hOldCanvas) SelectObject(hCanvasDC, hOldCanvas);
            DeleteDC(hCanvasDC);
        }
        if (hCanvas) DeleteObject(hCanvas);

        if (hSrcDibDC) {
            if (hOldSrcDib) SelectObject(hSrcDibDC, hOldSrcDib);
            DeleteDC(hSrcDibDC);
        }
        if (hSrcDib) DeleteObject(hSrcDib);

        if (hSrcDC) {
            if (hOldSrc) SelectObject(hSrcDC, hOldSrc);
            DeleteDC(hSrcDC);
        }
        if (hScreenDC) ReleaseDC(nullptr, hScreenDC);
    }

    bool Initialize() {
        hScreenDC = GetDC(nullptr);
        if (!hScreenDC) return false;

        DWORD ghostExStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
        hGhost = CreateWindowExW(
            ghostExStyle, L"STATIC", nullptr, WS_POPUP,
            boundLeft, boundTop, boundW, boundH,
            nullptr, nullptr, nullptr, nullptr
        );
        if (!hGhost) return false;
        SetPropW(hGhost, kPropGhostOverlay, (HANDLE)1);

        hCanvas = CreateDib32(hScreenDC, boundW, boundH, (void**)&pBits);
        if (!hCanvas || !pBits) return false;
        hCanvasDC = CreateCompatibleDC(hScreenDC);
        hOldCanvas = (HBITMAP)SelectObject(hCanvasDC, hCanvas);

        hSrcDC = CreateCompatibleDC(hScreenDC);
        hOldSrc = (HBITMAP)SelectObject(hSrcDC, data->hBitmap);

        hSrcDib = CreateDib32(hScreenDC, W, H, (void**)&srcBits);
        if (!hSrcDib || !srcBits) return false;
        hSrcDibDC = CreateCompatibleDC(hScreenDC);
        hOldSrcDib = (HBITMAP)SelectObject(hSrcDibDC, hSrcDib);

        BitBlt(hSrcDibDC, 0, 0, W, H, hSrcDC, 0, 0, SRCCOPY);

        if (data->isClosing) {
            InitThanosParticles();
        }
        return true;
    }

    void InitThanosParticles() {
        const int columns = (W + blockSizeSetting - 1) / blockSizeSetting;
        const int rows    = (H + blockSizeSetting - 1) / blockSizeSetting;
        const size_t particleCount = (size_t)columns * (size_t)rows;

        thanosParticles.reserve(particleCount);
        firstActiveParticle = 0;

        const float maxDist = (float)(W + H);
        uint32_t particleIndex = 0;

        for (int srcY = 0; srcY < H; srcY += blockSizeSetting) {
            for (int srcX = 0; srcX < W; srcX += blockSizeSetting) {
                uint32_t hash = ((uint32_t)srcX * 73856093u) ^ ((uint32_t)srcY * 19349663u);
                const uint32_t nextActive = (particleIndex + 1 < particleCount) ? (particleIndex + 1) : kNoActiveParticle;

                float distFromBottomRight = (float)((W - srcX) + (H - srcY));
                float baseStartTime = (distFromBottomRight / maxDist) * 0.6f;
                float waveNoise = (((hash % 100) / 100.0f) - 0.5f) * 0.15f;
                float startTime = Clamp(baseStartTime + waveNoise, 0.0f, 0.65f);

                float noiseX = (((hash >> 4) % 200) / 100.0f) - 1.0f;
                float noiseY = (((hash >> 8) % 200) / 100.0f) - 1.0f;

                float baseWindX = W * 0.5f;
                float baseWindY = H * 0.2f;
                float swirl = ((srcY / (float)H) - 0.5f) * 2.0f;
                baseWindY += swirl * (H * 0.3f);

                float windX  = baseWindX + noiseX * (W * 0.2f);
                float windY  = baseWindY + noiseY * (H * 0.3f);
                float curveX = noiseY * (W * 0.4f);
                float curveY = -noiseX * (H * 0.4f);

                thanosParticles.push_back({windX, windY, curveX, curveY, startTime, nextActive});
                ++particleIndex;
            }
        }
    }

    void ClearCanvas() {
        memset(pBits, 0, (size_t)boundW * (size_t)boundH * 4);
    }

    inline void DrawBlock(int srcX, int srcY, int dstX, int dstY, float alpha) {
        const int bw = blockSizeSetting;
        const int bh = blockSizeSetting;
        const int y0 = dstY < 0 ? -dstY : 0;
        const int y1 = std::min(bh, std::min(H - srcY, boundH - dstY));
        const int x0 = dstX < 0 ? -dstX : 0;
        const int x1 = std::min(bw, std::min(W - srcX, boundW - dstX));
        if (x0 >= x1 || y0 >= y1) return;

        const size_t bytes = (size_t)(x1 - x0) * sizeof(uint32_t);
        for (int by = y0; by < y1; ++by) {
            const BYTE* src = srcBits + (size_t)(srcY + by) * srcStride + (size_t)(srcX + x0) * 4;
            BYTE* dst       = pBits   + (size_t)(dstY + by) * canvasStride + (size_t)(dstX + x0) * 4;
            if (alpha >= 0.99f) {
                memcpy(dst, src, bytes);
            } else {
                for (int bx = 0; bx < (x1 - x0); ++bx) {
                    const BYTE* s = src + bx * 4;
                    BYTE* d       = dst + bx * 4;
                    d[0] = (BYTE)(s[0] * alpha);
                    d[1] = (BYTE)(s[1] * alpha);
                    d[2] = (BYTE)(s[2] * alpha);
                    d[3] = (BYTE)(s[3] * alpha);
                }
            }
        }
    }

    void RenderThanos(float progress) {
        const int columns = (W + blockSizeSetting - 1) / blockSizeSetting;
        const int dstOffsetX = origLeft - boundLeft;
        const int dstOffsetY = origTop - boundTop;

        constexpr float kLifeSpan = 0.4f;
        size_t rowStart = 0;
        int srcY = 0;
        uint32_t previousParticle = kNoActiveParticle;
        uint32_t particleIndex = firstActiveParticle;

        while (particleIndex != kNoActiveParticle && particleIndex < thanosParticles.size()) {
            ThanosParticle& particle = thanosParticles[particleIndex];
            const uint32_t nextParticle = particle.nextActive;
            const float localProgress = (progress - particle.startTime) / kLifeSpan;

            if (localProgress >= 1.0f) {
                if (previousParticle == kNoActiveParticle) firstActiveParticle = nextParticle;
                else thanosParticles[previousParticle].nextActive = nextParticle;
                particleIndex = nextParticle;
                continue;
            }

            previousParticle = particleIndex;
            while ((size_t)particleIndex >= rowStart + (size_t)columns) {
                rowStart += (size_t)columns;
                srcY += blockSizeSetting;
            }
            const int srcX = (int)((size_t)particleIndex - rowStart) * blockSizeSetting;
            int dstBaseX = dstOffsetX + srcX;
            int dstBaseY = dstOffsetY + srcY;
            float currentAlpha = 1.0f;

            if (localProgress > 0.0f) {
                const float travel = localProgress;
                const float travelSq = travel * travel;
                dstBaseX += (int)(particle.windX * travel + particle.curveX * travelSq);
                dstBaseY += (int)(particle.windY * travel + particle.curveY * travelSq);
                currentAlpha = 1.0f - localProgress;
            }

            DrawBlock(srcX, srcY, dstBaseX, dstBaseY, currentAlpha);
            particleIndex = nextParticle;
        }
    }

    void BlitVerticalSquash(int dstBaseX, int dstBaseY, int dstH, float alpha, float bright) {
        if (dstH < 1 || alpha <= 0.0f) return;
        bright = Clamp(bright, 0.0f, 1.5f);
        const float invH = 1.0f / (float)dstH;

        for (int dy = 0; dy < dstH; ++dy) {
            const int outY = dstBaseY + dy;
            if (outY < 0 || outY >= boundH) continue;
            const float srcYF = ((float)dy + 0.5f) * invH * (float)H - 0.5f;
            int y0 = (int)floorf(srcYF);
            float fy = srcYF - (float)y0;
            int y1 = y0 + 1;
            if (y0 < 0) { y0 = 0; fy = 0.0f; }
            if (y1 >= H) y1 = H - 1;
            if (y0 >= H) y0 = H - 1;

            const BYTE* row0 = srcBits + (size_t)y0 * srcStride;
            const BYTE* row1 = srcBits + (size_t)y1 * srcStride;
            BYTE* dstRow     = pBits   + (size_t)outY * canvasStride;
            const float w0 = 1.0f - fy;
            const float w1 = fy;

            for (int x = 0; x < W; ++x) {
                const int outX = dstBaseX + x;
                if (outX < 0 || outX >= boundW) continue;
                const BYTE* s0 = row0 + (size_t)x * 4;
                const BYTE* s1 = row1 + (size_t)x * 4;
                BYTE* d        = dstRow + (size_t)outX * 4;

                float b = ((float)s0[0] * w0 + (float)s1[0] * w1) * bright;
                float g = ((float)s0[1] * w0 + (float)s1[1] * w1) * bright;
                float r = ((float)s0[2] * w0 + (float)s1[2] * w1) * bright;
                float a = ((float)s0[3] * w0 + (float)s1[3] * w1) * alpha;

                if (bright > 1.0f) {
                    const float lift = (bright - 1.0f) * 0.65f;
                    b += (255.0f - b) * lift;
                    g += (255.0f - g) * lift;
                    r += (220.0f - r) * lift;
                }
                d[0] = (BYTE)(b > 255.0f ? 255.0f : b);
                d[1] = (BYTE)(g > 255.0f ? 255.0f : g);
                d[2] = (BYTE)(r > 255.0f ? 255.0f : r);
                d[3] = (BYTE)(a > 255.0f ? 255.0f : a);
            }
        }
    }

    void DrawSharpCrtBeam(float cx, float cy, float halfLen, float intensity) {
        if (intensity <= 0.02f || halfLen < 0.25f) return;
        intensity = Clamp(intensity, 0.0f, 1.0f);

        const float tip = 1.75f;
        const int x0 = (int)floorf(cx - halfLen - tip);
        const int x1 = (int)ceilf(cx + halfLen + tip);
        const int y0 = (int)floorf(cy - 2.0f);
        const int y1 = (int)ceilf(cy + 2.0f);

        for (int y = y0; y <= y1; ++y) {
            if (y < 0 || y >= boundH) continue;
            const float ady = fabsf((float)y + 0.5f - cy);
            float gy = 0.0f;
            if (ady < 0.55f) gy = 1.0f;
            else if (ady < 1.55f) gy = 1.0f - (ady - 0.55f);
            if (gy <= 0.0f) continue;

            BYTE* dstRow = pBits + (size_t)y * canvasStride;
            for (int x = x0; x <= x1; ++x) {
                if (x < 0 || x >= boundW) continue;
                const float adx = fabsf((float)x + 0.5f - cx);
                float gx = 1.0f;
                if (adx > halfLen) {
                    const float t = (adx - halfLen) / tip;
                    if (t >= 1.0f) continue;
                    gx = 1.0f - t;
                }
                const float a = gy * gx * intensity;
                if (a < 0.02f) continue;

                const float hot = gy;
                const float b = (210.0f + 45.0f * hot) * a;
                const float g = (235.0f + 20.0f * hot) * a;
                const float r = (255.0f) * a;
                const float aa = 255.0f * a;

                BYTE* d = dstRow + (size_t)x * 4;
                d[0] = (BYTE)(b > 255.0f ? 255.0f : b);
                d[1] = (BYTE)(g > 255.0f ? 255.0f : g);
                d[2] = (BYTE)(r > 255.0f ? 255.0f : r);
                d[3] = (BYTE)(aa > 255.0f ? 255.0f : aa);
            }
        }
    }

    void DrawSharpCrtDot(float cx, float cy, float intensity) {
        if (intensity <= 0.02f) return;
        intensity = Clamp(intensity, 0.0f, 1.0f);

        const int x0 = (int)floorf(cx - 1.5f);
        const int x1 = (int)ceilf(cx + 1.5f);
        const int y0 = (int)floorf(cy - 1.5f);
        const int y1 = (int)ceilf(cy + 1.5f);

        for (int y = y0; y <= y1; ++y) {
            if (y < 0 || y >= boundH) continue;
            BYTE* dstRow = pBits + (size_t)y * canvasStride;
            for (int x = x0; x <= x1; ++x) {
                if (x < 0 || x >= boundW) continue;
                const float dx = (float)x + 0.5f - cx;
                const float dy = (float)y + 0.5f - cy;
                const float d2 = dx * dx + dy * dy;
                float w = 0.0f;
                if (d2 <= 0.35f) w = 1.0f;
                else if (d2 <= 1.6f) w = 1.0f - (d2 - 0.35f) / 1.25f;
                if (w <= 0.0f) continue;

                const float a = w * intensity;
                BYTE* d = dstRow + (size_t)x * 4;
                d[0] = (BYTE)(220.0f * a);
                d[1] = (BYTE)(245.0f * a);
                d[2] = (BYTE)(255.0f * a);
                d[3] = (BYTE)(255.0f * a);
            }
        }
    }

    void RenderCrtOff(float progress) {
        constexpr float squashEnd = 0.42f;
        constexpr float zipEnd    = 0.78f;
        const float cx = (float)(origLeft - boundLeft) + (float)W * 0.5f;
        const float cy = (float)(origTop - boundTop) + (float)H * 0.5f;

        if (progress < squashEnd) {
            const float u = progress / squashEnd;
            const float punch = u * u * u;
            const float minH = 2.0f;
            const float dstHf = (float)H * (1.0f - punch) + minH * punch;
            const int dstH = std::max(2, (int)(dstHf + 0.5f));
            const int dstY = (int)(cy - (float)dstH * 0.5f);
            const int dstX = origLeft - boundLeft;
            const float bright = 1.0f + punch * 0.85f;

            float contentAlpha = 1.0f;
            float beamAlpha = 0.0f;
            if ((float)dstH < 10.0f) {
                const float t = ((float)dstH - minH) / (10.0f - minH);
                contentAlpha = Clamp(t, 0.0f, 1.0f);
                beamAlpha = 1.0f - contentAlpha;
            }
            if (contentAlpha > 0.02f) BlitVerticalSquash(dstX, dstY, dstH, contentAlpha, bright);
            if (beamAlpha > 0.02f) DrawSharpCrtBeam(cx, cy, (float)W * 0.5f, beamAlpha);
            return;
        }

        if (progress < zipEnd) {
            const float u = (progress - squashEnd) / (zipEnd - squashEnd);
            const float ease = u * u;
            const float halfLen = (float)W * 0.5f * (1.0f - ease) + 1.0f * ease;
            DrawSharpCrtBeam(cx, cy, halfLen, 1.0f);
            return;
        }

        const float u = (progress - zipEnd) / std::max(0.01f, 1.0f - zipEnd);
        const float decay = 1.0f - u;
        if (decay > 0.02f) DrawSharpCrtDot(cx, cy, decay * decay);
    }

    void Present() {
        POINT ptSrc{0, 0};
        POINT ptDst{boundLeft, boundTop};
        SIZE sizeWnd{boundW, boundH};

        BLENDFUNCTION blend{};
        blend.BlendOp             = AC_SRC_OVER;
        blend.BlendFlags          = 0;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat         = AC_SRC_ALPHA;

        UpdateLayeredWindow(hGhost, hScreenDC, &ptDst, &sizeWnd, hCanvasDC, &ptSrc, 0, &blend, ULW_ALPHA);
        ShowWindow(hGhost, SW_SHOWNOACTIVATE);
    }

    void Run() {
        LARGE_INTEGER freq{}, startTick{}, nowTick{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&startTick);

        const double durationSec = (double)data->durationMs / 1000.0;

        for (;;) {
            QueryPerformanceCounter(&nowTick);
            double elapsed = (double)(nowTick.QuadPart - startTick.QuadPart) / (double)freq.QuadPart;
            float progress = (float)(elapsed / durationSec);
            if (progress > 1.0f) progress = 1.0f;

            ClearCanvas();
            if (data->isClosing) {
                RenderThanos(progress);
            } else {
                RenderCrtOff(progress);
            }

            Present();
            if (progress >= 1.0f) break;

            DwmFlush();
        }
    }
};

static DWORD WINAPI AnimationThreadProc(LPVOID lpParam) {
    auto* data = reinterpret_cast<WindowAnimData*>(lpParam);
    if (!data) return 0;

    HWND hWnd                = data->hRealWnd;
    const bool isClosing     = data->isClosing;
    const UINT closeMsg      = data->closeMsg;
    const WPARAM closeWParam = data->closeWParam;

    {
        WindowAnimWorker worker(data);
        if (worker.Initialize()) {
            worker.Run();
        }
    }

    if (data->hBitmap) {
        DeleteObject(data->hBitmap);
    }
    delete data;

    if (IsWindow(hWnd)) {
        if (isClosing) {
            SetPropW(hWnd, kPropCloseBypass, (HANDLE)1);
            SetWindowCloak(hWnd, FALSE);
            UpdateDwmTransitions(hWnd, TRUE);
            if (closeMsg == WM_SYSCOMMAND) {
                PostMessageW(hWnd, WM_SYSCOMMAND, closeWParam ? closeWParam : SC_CLOSE, 0);
            } else {
                PostMessageW(hWnd, WM_CLOSE, 0, 0);
            }
        } else {
            SetWindowCloak(hWnd, FALSE);
            UpdateDwmTransitions(hWnd, TRUE);
            RemovePropW(hWnd, kPropMinimizing);
        }
    }

    return 0;
}

static bool TriggerAnimation(HWND hWnd, bool isClosing, UINT closeMsg = 0, WPARAM closeWParam = 0) {
    if (!ShouldAnimate(hWnd)) return false;

    std::lock_guard<std::mutex> lock(g_AnimMutex);

    RECT frameRect{};
    if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect)))) {
        GetWindowRect(hWnd, &frameRect);
    }

    int w = frameRect.right - frameRect.left;
    int h = frameRect.bottom - frameRect.top;
    if (w <= 0 || h <= 0) return false;

    HDC hScreenDC = GetDC(nullptr);
    void* pBits = nullptr;
    HBITMAP hSnap = CreateDib32(hScreenDC, w, h, &pBits);
    if (!hSnap) {
        ReleaseDC(nullptr, hScreenDC);
        return false;
    }

    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hOld = (HBITMAP)SelectObject(hMemDC, hSnap);

    BOOL captured = BitBlt(hMemDC, 0, 0, w, h, hScreenDC, frameRect.left, frameRect.top, SRCCOPY);
    if (!captured) {
        captured = PrintWindow(hWnd, hMemDC, PW_RENDERFULLCONTENT);
    }

    SelectObject(hMemDC, hOld);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);

    if (!captured) {
        DeleteObject(hSnap);
        return false;
    }

    UpdateDwmTransitions(hWnd, FALSE);
    SetWindowCloak(hWnd, TRUE);

    int duration = isClosing ? GetCloseDuration() : GetMinDuration();

    auto* data = new WindowAnimData{
        hWnd, hSnap, frameRect, w, h, duration, isClosing, closeMsg, closeWParam
    };

    if (!isClosing) {
        SetPropW(hWnd, kPropMinimizing, (HANDLE)1);
        ShowWindow(hWnd, SW_MINIMIZE);
    }

    HANDLE hThread = CreateThread(nullptr, 0, AnimationThreadProc, data, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
        return true;
    }

    SetWindowCloak(hWnd, FALSE);
    UpdateDwmTransitions(hWnd, TRUE);
    DeleteObject(hSnap);
    delete data;
    return false;
}

// Subclass Procedure: Intercepts messages per-window
static LRESULT CALLBACK WindowSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    if (uMsg == WM_NCDESTROY) {
        RemovePropW(hWnd, kPropClosed);
        RemovePropW(hWnd, kPropCloseBypass);
        RemovePropW(hWnd, kPropMinimizing);
        RemoveWindowSubclass(hWnd, WindowSubclassProc, SUBCLASS_ID);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    const bool isClose = (uMsg == WM_CLOSE) || (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE);
    if (isClose) {
        if (GetPropW(hWnd, kPropCloseBypass)) {
            RemovePropW(hWnd, kPropCloseBypass);
            RemovePropW(hWnd, kPropClosed);
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        if (!GetPropW(hWnd, kPropClosed)) {
            SetPropW(hWnd, kPropClosed, (HANDLE)1);
            UINT repostMsg = (uMsg == WM_CLOSE) ? WM_CLOSE : WM_SYSCOMMAND;
            WPARAM repostWParam = (uMsg == WM_SYSCOMMAND) ? wParam : 0;
            if (TriggerAnimation(hWnd, /*isClosing=*/true, repostMsg, repostWParam)) {
                return 0; // Intercepted for Thanos Snap
            }
            RemovePropW(hWnd, kPropClosed);
        }
    }

    if (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_MINIMIZE) {
        if (!GetPropW(hWnd, kPropMinimizing)) {
            if (TriggerAnimation(hWnd, /*isClosing=*/false)) {
                return 0; // Intercepted for Retro TV
            }
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Attach subclass to window
static void SubclassIfEligible(HWND hWnd) {
    if (ShouldAnimate(hWnd)) {
        SetWindowSubclass(hWnd, WindowSubclassProc, SUBCLASS_ID, 0);
    }
}

// CBT Global Hook Callback
extern "C" __declspec(dllexport)
LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && IsActive()) {
        HWND hWnd = (HWND)wParam;
        if (nCode == HCBT_CREATEWND || nCode == HCBT_ACTIVATE) {
            SubclassIfEligible(hWnd);
        }
        else if (nCode == HCBT_MINMAX) {
            if (lParam == SW_MINIMIZE) {
                if (!GetPropW(hWnd, kPropMinimizing) && ShouldAnimate(hWnd)) {
                    TriggerAnimation(hWnd, /*isClosing=*/false);
                    return 1; // Cancel native minimize; our worker executes it after CRT
                }
            }
        }
    }

    EnsureSharedConfig();
    HHOOK hook = g_pConfig ? g_pConfig->hHook : nullptr;
    return CallNextHookEx(hook, nCode, wParam, lParam);
}

static BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam) {
    SubclassIfEligible(hWnd);
    return TRUE;
}

// Exported control APIs for the launcher
extern "C" __declspec(dllexport) BOOL InstallHook() {
    EnsureSharedConfig();
    if (!g_pConfig) return FALSE;

    if (!g_pConfig->hHook) {
        g_pConfig->hHook = SetWindowsHookExW(WH_CBT, CBTProc, g_hInstance, 0);
        if (!g_pConfig->hHook) return FALSE;
    }

    // Subclass all existing open windows
    EnumWindows(EnumWindowsProc, 0);
    return TRUE;
}

extern "C" __declspec(dllexport) void UninstallHook() {
    EnsureSharedConfig();
    if (g_pConfig && g_pConfig->hHook) {
        UnhookWindowsHookEx(g_pConfig->hHook);
        g_pConfig->hHook = nullptr;
    }
}

extern "C" __declspec(dllexport) void SetEnabled(BOOL enabled) {
    EnsureSharedConfig();
    if (g_pConfig) g_pConfig->enabled = enabled;
}

extern "C" __declspec(dllexport) BOOL IsHookEnabled() {
    EnsureSharedConfig();
    return g_pConfig ? g_pConfig->enabled : TRUE;
}

extern "C" __declspec(dllexport) void SetDurations(int minDurationMs, int closeDurationMs) {
    EnsureSharedConfig();
    if (g_pConfig) {
        g_pConfig->minDurationMs = minDurationMs;
        g_pConfig->closeDurationMs = closeDurationMs;
    }
}

extern "C" __declspec(dllexport) void SetParticleSize(int sizePx) {
    EnsureSharedConfig();
    if (g_pConfig) {
        g_pConfig->particleSize = sizePx;
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInstance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        EnsureSharedConfig();
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_pConfig) {
            UnmapViewOfFile(g_pConfig);
            g_pConfig = nullptr;
        }
        if (g_hSharedMap) {
            CloseHandle(g_hSharedMap);
            g_hSharedMap = nullptr;
        }
    }
    return TRUE;
}

