/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "XRayOverlay.h"
#include "XRayThread.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <windowsx.h>
#include <shlwapi.h>
#include <objidl.h>
namespace Gdiplus
{
    using std::min;
    using std::max;
}
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

namespace gp = Gdiplus;

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace XRay
{
    namespace
    {
        constexpr wchar_t kWindowClass[] = L"MPC-HC.XRay";
        constexpr UINT WM_XRAY_SERVICE = WM_APP + 0x5A1;
        constexpr UINT WM_XRAY_DETAILS = WM_APP + 0x5A2;
        constexpr UINT_PTR kSyncTimer = 1;
        constexpr UINT_PTR kAnimTimer = 2;

        // the module (exe or plugin DLL) this code is part of: owns the window class
        HINSTANCE ModuleInstance()
        {
            return reinterpret_cast<HINSTANCE>(&__ImageBase);
        }

        // Prime Video X-Ray like palette
        const gp::Color kPanel(236, 13, 20, 27);
        const gp::Color kText(255, 255, 255, 255);
        const gp::Color kSubText(255, 143, 160, 172);
        const gp::Color kDimText(255, 110, 126, 138);
        const gp::Color kAccent(255, 26, 152, 255);
        const gp::Color kHover(26, 255, 255, 255);
        const gp::Color kSeparator(36, 255, 255, 255);
        const gp::Color kPlaceholder(255, 36, 48, 58);
        const gp::Color kSilhouette(255, 78, 94, 106);

        void AddRoundRect(gp::GraphicsPath& p, float x, float y, float w, float h, float r)
        {
            r = std::min(r, std::min(w, h) / 2);
            if (r <= 0.5f) {
                p.AddRectangle(gp::RectF(x, y, w, h));
                return;
            }
            const float d = r * 2;
            p.AddArc(x, y, d, d, 180, 90);
            p.AddArc(x + w - d, y, d, d, 270, 90);
            p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            p.AddArc(x, y + h - d, d, d, 90, 90);
            p.CloseFigure();
        }

        void FillRoundRect(gp::Graphics& g, const gp::Color& c, float x, float y, float w, float h, float r)
        {
            gp::GraphicsPath path;
            AddRoundRect(path, x, y, w, h, r);
            gp::SolidBrush b(c);
            g.FillPath(&b, &path);
        }

        struct Fonts {
            gp::FontFamily regular{ L"Segoe UI" };
            gp::FontFamily semibold{ L"Segoe UI Semibold" };
            std::unique_ptr<gp::Font> title, tab, name, bigName, body, note, caption, button;

            explicit Fonts(float s)
            {
                const gp::FontFamily& sb = semibold.IsAvailable() ? semibold : regular;
                const int sbStyle = semibold.IsAvailable() ? gp::FontStyleRegular : gp::FontStyleBold;
                title = std::make_unique<gp::Font>(&regular, 24 * s, gp::FontStyleBold, gp::UnitPixel);
                tab = std::make_unique<gp::Font>(&sb, 15 * s, sbStyle, gp::UnitPixel);
                name = std::make_unique<gp::Font>(&sb, 16 * s, sbStyle, gp::UnitPixel);
                bigName = std::make_unique<gp::Font>(&regular, 25 * s, gp::FontStyleBold, gp::UnitPixel);
                body = std::make_unique<gp::Font>(&regular, 14 * s, gp::FontStyleRegular, gp::UnitPixel);
                note = std::make_unique<gp::Font>(&regular, 13 * s, gp::FontStyleRegular, gp::UnitPixel);
                caption = std::make_unique<gp::Font>(&sb, 11.5f * s, sbStyle, gp::UnitPixel);
                button = std::make_unique<gp::Font>(&sb, 14 * s, sbStyle, gp::UnitPixel);
            }
        };
        Fonts* g_fonts = nullptr; // valid while painting

        // Draws text in a box, at most maxLines lines with an ellipsis, returns the used height
        float Text(gp::Graphics& g, const std::wstring& text, const gp::Font* font, const gp::Color& color, float x, float y, float w, int maxLines = 1,
                   gp::StringAlignment align = gp::StringAlignmentNear)
        {
            if (text.empty() || w <= 1) {
                return 0;
            }
            gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
            fmt.SetAlignment(align);
            fmt.SetTrimming(maxLines == 1 ? gp::StringTrimmingEllipsisCharacter : gp::StringTrimmingEllipsisWord);
            fmt.SetFormatFlags((maxLines == 1 ? gp::StringFormatFlagsNoWrap : 0) | gp::StringFormatFlagsLineLimit | gp::StringFormatFlagsMeasureTrailingSpaces);
            const float lineHeight = font->GetHeight(&g);
            gp::RectF box(x, y, w, lineHeight * maxLines + 1);
            gp::RectF used;
            g.MeasureString(text.c_str(), (INT)text.size(), font, box, &fmt, &used);
            gp::SolidBrush brush(color);
            g.DrawString(text.c_str(), (INT)text.size(), font, box, &fmt, &brush);
            return std::max(used.Height, lineHeight);
        }

        float TextWidth(gp::Graphics& g, const std::wstring& text, const gp::Font* font)
        {
            gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
            fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsMeasureTrailingSpaces);
            gp::RectF used;
            g.MeasureString(text.c_str(), (INT)text.size(), font, gp::PointF(0, 0), &fmt, &used);
            return used.Width;
        }

        struct Date {
            int y = 0, m = 0, d = 0;
            bool valid() const { return y > 0; }
        };

        Date ParseDate(const std::wstring& s)
        {
            Date dt;
            if (s.size() >= 4 && iswdigit(s[0])) {
                if (swscanf_s(s.c_str(), L"%d-%d-%d", &dt.y, &dt.m, &dt.d) < 1) {
                    dt = {};
                }
                if (dt.m < 1 || dt.m > 12 || dt.d < 1 || dt.d > 31) {
                    dt.m = dt.d = 0;
                }
            }
            return dt;
        }

        Date Today()
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            return { st.wYear, st.wMonth, st.wDay };
        }

        int Age(const Date& from, const Date& to)
        {
            int age = to.y - from.y;
            if (from.m && to.m && (to.m < from.m || (to.m == from.m && to.d < from.d))) {
                age--;
            }
            return age;
        }

        std::wstring FormatDate(const Date& dt, bool shortMonth = false)
        {
            if (!dt.valid()) {
                return {};
            }
            if (dt.m && dt.d) {
                SYSTEMTIME st{};
                st.wYear = (WORD)dt.y;
                st.wMonth = (WORD)dt.m;
                st.wDay = (WORD)dt.d;
                wchar_t buf[128];
                if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, shortMonth ? L"MMM d, yyyy" : L"MMMM d, yyyy", buf, _countof(buf), nullptr)) {
                    return buf;
                }
            }
            return std::to_wstring(dt.y);
        }

        // "Born Jan 28, 1981 (age 45)" / "1922 – 2015 (died at 93)"
        std::wstring LifeLine(const Person& p)
        {
            const Date born = ParseDate(p.birthday), died = ParseDate(p.deathday);
            if (!born.valid()) {
                return {};
            }
            if (died.valid()) {
                return std::to_wstring(born.y) + L" \u2013 " + std::to_wstring(died.y) + L"  (died at " + std::to_wstring(Age(born, died)) + L")";
            }
            return L"Born " + FormatDate(born, true) + L"  (age " + std::to_wstring(Age(born, Today())) + L")";
        }

        std::wstring Lower(std::wstring s)
        {
            std::transform(s.begin(), s.end(), s.begin(), towlower);
            return s;
        }
    }

    struct Overlay::DetailsState {
        std::mutex lock;
        std::vector<std::tuple<int, int, Person>> done; // generation, cast index, person
        std::set<int> pending;                          // UI thread only
    };

    // -------------------------------------------------------------------------------------------------------------
    // Life cycle

    Overlay::Overlay(OverlayHost& host, const std::wstring& cacheDir, const std::wstring& appDir)
        : m_host(host)
        , m_service(std::make_shared<Service>(cacheDir, appDir))
        , m_details(std::make_shared<DetailsState>())
    {
        gp::GdiplusStartupInput input;
        gp::GdiplusStartup(&m_gdiplusToken, &input, nullptr);

        WNDCLASSEXW wc{ sizeof(wc) };
        HINSTANCE inst = ModuleInstance();
        if (!GetClassInfoExW(inst, kWindowClass, &wc)) {
            wc = { sizeof(wc) };
            wc.lpfnWndProc = WndProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kWindowClass;
            RegisterClassExW(&wc);
        }
        // message only window: receives the service notifications and the timers, it never changes
        m_msgWnd = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this);
    }

    Overlay::~Overlay()
    {
        m_service->Cancel();
        DestroyWindows();
        if (m_msgWnd) {
            DestroyWindow(m_msgWnd);
        }
        // a reloaded plugin must not find the class pointing to the window procedure of the unloaded DLL
        UnregisterClassW(kWindowClass, ModuleInstance());
        m_images.clear();
        if (m_gdiplusToken) {
            gp::GdiplusShutdown(m_gdiplusToken);
        }
    }

    bool Overlay::EnsureWindow(Layered& w)
    {
        if (w.hwnd && IsWindow(w.hwnd)) {
            return true;
        }
        w = Layered();
        w.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"X-Ray", WS_POPUP,
                                 0, 0, 1, 1, m_owner, nullptr, ModuleInstance(), this);
        return w.hwnd != nullptr;
    }

    void Overlay::DestroyWindows()
    {
        for (Layered* w : { &m_button, &m_panel }) {
            if (w->hwnd) {
                DestroyWindow(w->hwnd);
            }
            *w = Layered();
        }
    }

    // -------------------------------------------------------------------------------------------------------------
    // Public API

    void Overlay::SetPaused(bool paused)
    {
        m_paused = paused;
        if (!paused && m_panelOpen) {
            m_panelOpen = false;
            m_detail = -1;
        }
        UpdatePosition();
    }

    void Overlay::Reset()
    {
        m_service->Cancel();
        m_busy = false;
        m_needConsent = false;
        m_result.reset();
        m_analyzedPos = -1;
        m_generation++;
        m_detail = -1;
        m_scroll = 0;
        m_tab = Tab::Scene;
        m_panelOpen = false;
        m_paused = false;
        m_images.clear();
        {
            std::lock_guard<std::mutex> lock(m_details->lock);
            m_details->done.clear();
        }
        m_details->pending.clear();
        UpdatePosition();
    }

    void Overlay::OpenPanel()
    {
        if (m_panelOpen) {
            return;
        }
        m_panelOpen = true;
        m_detail = -1;
        m_scroll = 0;
        m_tab = Tab::Scene;
        const int64_t pos = m_host.GetPositionMs();
        const bool stale = !m_result || m_result->status != Result::Status::Ok || std::llabs(pos - m_analyzedPos) > 400;
        if (stale && !m_busy) {
            const int consent = m_host.GetEngineDownloadConsent();
            if (!m_service->IsEngineInstalled() && consent < 0) {
                m_needConsent = true;
            } else {
                StartAnalysis(consent > 0);
            }
        }
        m_layoutDirty = true;
        UpdatePosition();
    }

    void Overlay::ClosePanel()
    {
        if (!m_panelOpen) {
            return;
        }
        m_panelOpen = false;
        m_detail = -1;
        m_layoutDirty = true;
        UpdatePosition();
    }

    void Overlay::UpdatePosition()
    {
        HWND owner = m_host.GetOwnerWindow();
        if (owner != m_owner) {
            DestroyWindows();
            m_owner = owner;
        }
        RECT r{};
        const bool visible = m_owner && IsWindowVisible(m_owner) && !IsIconic(m_owner) && m_host.GetVideoRect(r)
                             && r.right - r.left >= 240 && r.bottom - r.top >= 160;
        const float scale = std::max(1u, m_host.GetDpi()) / 96.0f;
        if (!EqualRect(&r, &m_video) || scale != m_scale) {
            m_video = r;
            m_scale = scale;
            m_layoutDirty = true;
        }

        const bool showButton = visible && m_paused && !m_panelOpen;
        const bool showPanel = visible && m_panelOpen;
        if (showButton) {
            const bool wasVisible = m_button.hwnd && IsWindowVisible(m_button.hwnd);
            if (EnsureWindow(m_button) && (m_layoutDirty || !wasVisible)) {
                RenderButton();
            }
        } else if (m_button.hwnd) {
            ShowWindow(m_button.hwnd, SW_HIDE);
        }
        if (showPanel) {
            const bool wasVisible = m_panel.hwnd && IsWindowVisible(m_panel.hwnd);
            if (EnsureWindow(m_panel) && (m_layoutDirty || !wasVisible)) {
                RenderPanel();
            }
        } else if (m_panel.hwnd) {
            ShowWindow(m_panel.hwnd, SW_HIDE);
        }
        m_layoutDirty = false;
        UpdateTimer();
    }

    bool Overlay::PreTranslateMessage(const MSG* msg)
    {
        if (!m_panelOpen || !m_panel.hwnd || !IsWindowVisible(m_panel.hwnd)) {
            return false;
        }
        if (msg->message == WM_MOUSEWHEEL) {
            POINT pt{ GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
            RECT rc;
            GetWindowRect(m_panel.hwnd, &rc);
            if (PtInRect(&rc, pt)) {
                Scroll(-GET_WHEEL_DELTA_WPARAM(msg->wParam) / (float)WHEEL_DELTA * S(64));
                return true;
            }
        } else if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) {
            if (m_detail >= 0) {
                OnAction(Action::Back, 0);
            } else {
                ClosePanel();
            }
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------------------------------------------
    // Analysis

    void Overlay::StartAnalysis(bool allowDownload)
    {
        m_needConsent = false;
        ImageBGR frame;
        std::wstring error;
        Request req;
        req.positionMs = m_host.GetPositionMs();
        m_analyzedPos = req.positionMs;
        if (!m_host.CaptureFrame(frame, error) || frame.empty()) {
            // the service still resolves the movie and shows the whole cast
            frame = ImageBGR();
        }
        req.mediaName = m_host.GetMediaName();
        req.mediaPath = m_host.GetMediaPath();
        req.frame = std::move(frame);
        req.tmdbApiKey = m_host.GetTmdbApiKey();
        req.allowEngineDownload = allowDownload;
        m_busy = true;
        m_progress = L"Starting\u2026";
        m_service->Start(std::move(req), m_msgWnd, WM_XRAY_SERVICE);
        m_layoutDirty = true;
        UpdateTimer();
    }

    void Overlay::OnServiceNotify(WPARAM wp)
    {
        if (wp == Service::kNotifyDone || wp == Service::kNotifyPartial) {
            const bool done = wp == Service::kNotifyDone;
            auto r = done ? m_service->TakeResult() : m_service->TakePartialResult();
            if (!r || r->status == Result::Status::Cancelled) {
                return;
            }
            m_busy = !done;
            const bool sameMovie = m_result && m_result->movie && r->movie && m_result->movie->key == r->movie->key;
            if (sameMovie) {
                // keep the details already loaded for the "All Cast" entries
                for (auto& p : r->movie->cast) {
                    for (const auto& old : m_result->movie->cast) {
                        if (old.id == p.id && old.detailsLoaded && !p.detailsLoaded) {
                            p = old;
                        }
                    }
                }
            } else {
                m_detail = -1;
            }
            m_result = r;
            m_generation++;
            m_details->pending.clear();
            if (!sameMovie) {
                m_scroll = 0;
            }
            // nothing recognized because face recognition is off: the cast list is the useful view
            if (r->status == Result::Status::Ok && !r->recognitionAvailable && m_tab == Tab::Scene && m_host.GetEngineDownloadConsent() == 0) {
                m_tab = Tab::Cast;
            }
        } else {
            m_progress = m_service->GetProgress();
        }
        m_layoutDirty = true;
        if (m_panelOpen) {
            RenderPanel();
        }
        UpdateTimer();
    }

    void Overlay::RequestDetails(int castIndex)
    {
        if (!m_result || !m_result->movie || castIndex < 0 || castIndex >= (int)m_result->movie->cast.size()) {
            return;
        }
        const Person& p = m_result->movie->cast[castIndex];
        if (p.detailsLoaded || m_details->pending.count(castIndex)) {
            return;
        }
        m_details->pending.insert(castIndex);
        RunDetached([svc = m_service, state = m_details, hwnd = m_msgWnd, person = p, providerId = m_result->movie->providerId,
                           key = m_host.GetTmdbApiKey(), gen = m_generation, castIndex]() mutable {
            svc->LoadPersonDetails(person, providerId, key);
            person.detailsLoaded = true; // even on failure, don't retry in a loop
            {
                std::lock_guard<std::mutex> lock(state->lock);
                state->done.emplace_back(gen, castIndex, std::move(person));
            }
            PostMessageW(hwnd, WM_XRAY_DETAILS, 0, 0);
        });
    }

    void Overlay::OnDetailsReady()
    {
        std::vector<std::tuple<int, int, Person>> done;
        {
            std::lock_guard<std::mutex> lock(m_details->lock);
            done.swap(m_details->done);
        }
        bool changed = false;
        for (auto& d : done) {
            if (std::get<0>(d) == m_generation && m_result && m_result->movie && std::get<1>(d) < (int)m_result->movie->cast.size()) {
                m_result->movie->cast[std::get<1>(d)] = std::move(std::get<2>(d));
                m_details->pending.erase(std::get<1>(d));
                changed = true;
            }
        }
        if (changed && m_panelOpen) {
            m_layoutDirty = true;
            RenderPanel();
        }
    }

    // -------------------------------------------------------------------------------------------------------------
    // Input

    void Overlay::Scroll(float delta)
    {
        const float maxScroll = std::max(0.0f, m_contentHeight - m_viewHeight);
        const float s = std::clamp(m_scroll + delta, 0.0f, maxScroll);
        if (s != m_scroll) {
            m_scroll = s;
            RenderPanel();
        }
    }

    void Overlay::OnAction(Action a, int param)
    {
        switch (a) {
            case Action::OpenButton:
                OpenPanel();
                return;
            case Action::Close:
                ClosePanel();
                return;
            case Action::TabScene:
            case Action::TabCast:
                m_tab = a == Action::TabScene ? Tab::Scene : Tab::Cast;
                m_detail = -1;
                m_scroll = 0;
                break;
            case Action::Person:
                m_listScroll = m_scroll;
                m_detail = param;
                m_scroll = 0;
                RequestDetails(param);
                break;
            case Action::Back:
                m_detail = -1;
                m_scroll = m_listScroll;
                break;
            case Action::Download:
                m_host.SetEngineDownloadConsent(true);
                m_tab = Tab::Scene;
                StartAnalysis(true);
                break;
            case Action::CastOnly:
                m_host.SetEngineDownloadConsent(false);
                m_tab = Tab::Cast;
                StartAnalysis(false);
                break;
            case Action::Retry:
                StartAnalysis(m_host.GetEngineDownloadConsent() > 0);
                break;
            default:
                return;
        }
        m_layoutDirty = true;
        RenderPanel();
    }

    bool Overlay::IsHovered(const Layered& w, Action a, int param) const
    {
        return w.hover >= 0 && w.hover < (int)w.hits.size() && w.hits[w.hover].action == a && w.hits[w.hover].param == param;
    }

    void Overlay::UpdateTimer()
    {
        if (!m_msgWnd) {
            return;
        }
        if (m_paused || m_panelOpen) {
            SetTimer(m_msgWnd, kSyncTimer, 200, nullptr);
        } else {
            KillTimer(m_msgWnd, kSyncTimer);
        }
        if (m_panelOpen && (m_busy || (m_detail >= 0 && m_details->pending.count(m_detail)))) {
            SetTimer(m_msgWnd, kAnimTimer, 40, nullptr);
        } else {
            KillTimer(m_msgWnd, kAnimTimer);
        }
    }

    LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        }
        auto self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (hwnd == self->m_msgWnd || !self->m_msgWnd) {
            switch (msg) {
                case WM_XRAY_SERVICE:
                    self->OnServiceNotify(wp);
                    return 0;
                case WM_XRAY_DETAILS:
                    self->OnDetailsReady();
                    return 0;
                case WM_TIMER:
                    if (wp == kSyncTimer) {
                        self->UpdatePosition();
                        // seeking or frame stepping while paused: analyze the new frame
                        if (self->m_panelOpen && self->m_paused && !self->m_busy && !self->m_needConsent && self->m_analyzedPos >= 0
                                && std::llabs(self->m_host.GetPositionMs() - self->m_analyzedPos) > 400) {
                            self->StartAnalysis(self->m_host.GetEngineDownloadConsent() > 0);
                            self->RenderPanel();
                        }
                    } else if (wp == kAnimTimer) {
                        self->m_spin++;
                        if (self->m_panelOpen) {
                            self->RenderPanel();
                        }
                    }
                    return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        Layered* w = hwnd == self->m_button.hwnd ? &self->m_button : hwnd == self->m_panel.hwnd ? &self->m_panel : nullptr;
        if (!w) {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return self->HandleMessage(*w, msg, wp, lp);
    }

    LRESULT Overlay::HandleMessage(Layered& w, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto hitAt = [&](LPARAM l) {
            const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
            for (int i = (int)w.hits.size() - 1; i >= 0; i--) {
                const Hit& h = w.hits[i];
                if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
                    return i;
                }
            }
            return -1;
        };
        switch (msg) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_MOUSEMOVE: {
                if (!w.tracking) {
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, w.hwnd, 0 };
                    w.tracking = !!TrackMouseEvent(&tme);
                }
                const int hover = hitAt(lp);
                if (hover != w.hover) {
                    w.hover = hover;
                    &w == &m_button ? RenderButton() : RenderPanel();
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                w.tracking = false;
                if (w.hover != -1) {
                    w.hover = -1;
                    &w == &m_button ? RenderButton() : RenderPanel();
                }
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, w.hover >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK: {
                const int hit = hitAt(lp);
                if (hit >= 0) {
                    const Hit h = w.hits[hit];
                    OnAction(h.action, h.param);
                }
                return 0;
            }
            case WM_RBUTTONUP:
                return 0;
            case WM_MOUSEWHEEL:
                if (&w == &m_panel) {
                    Scroll(-GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA * S(64));
                }
                return 0;
        }
        return DefWindowProcW(w.hwnd, msg, wp, lp);
    }

    // -------------------------------------------------------------------------------------------------------------
    // Painting

    gp::Bitmap* Overlay::GetImage(const std::wstring& path)
    {
        if (path.empty()) {
            return nullptr;
        }
        auto it = m_images.find(path);
        if (it != m_images.end()) {
            return it->second.get();
        }
        std::unique_ptr<gp::Bitmap> copy;
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string data = ss.str();
        if (!data.empty()) {
            if (IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), (UINT)data.size())) {
                std::unique_ptr<gp::Bitmap> decoded(gp::Bitmap::FromStream(stream));
                if (decoded && decoded->GetLastStatus() == gp::Ok && decoded->GetWidth() > 0) {
                    // copy to a bitmap that doesn't depend on the stream, downscaled for fast drawing
                    const float k = std::min(1.0f, 400.0f / std::max(decoded->GetWidth(), decoded->GetHeight()));
                    const INT w = std::max<INT>(1, INT(decoded->GetWidth() * k)), h = std::max<INT>(1, INT(decoded->GetHeight() * k));
                    copy = std::make_unique<gp::Bitmap>(w, h, PixelFormat32bppPARGB);
                    gp::Graphics g(copy.get());
                    g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
                    g.SetPixelOffsetMode(gp::PixelOffsetModeHighQuality);
                    g.DrawImage(decoded.get(), gp::Rect(0, 0, w, h), 0, 0, (INT)decoded->GetWidth(), (INT)decoded->GetHeight(), gp::UnitPixel);
                }
                stream->Release();
            }
        }
        gp::Bitmap* result = copy.get();
        m_images[path] = std::move(copy);
        return result;
    }

    void Overlay::Present(Layered& w, const std::function<void(gp::Graphics&)>& paint)
    {
        if (!w.hwnd || w.width <= 0 || w.height <= 0) {
            return;
        }
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w.width;
        bi.bmiHeader.biHeight = -w.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            memset(bits, 0, size_t(w.width) * w.height * 4);
            {
                gp::Bitmap canvas(w.width, w.height, w.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
                gp::Graphics g(&canvas);
                g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(gp::PixelOffsetModeHighQuality);
                g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
                g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
                Fonts fonts(m_scale);
                g_fonts = &fonts;
                paint(g);
                g_fonts = nullptr;
            }
            HGDIOBJ old = SelectObject(mem, dib);
            POINT src{ 0, 0 };
            SIZE size{ w.width, w.height };
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(w.hwnd, screen, &w.pos, &size, mem, &src, 0, &bf, ULW_ALPHA);
            SelectObject(mem, old);
            if (!IsWindowVisible(w.hwnd)) {
                SetWindowPos(w.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
        if (dib) {
            DeleteObject(dib);
        }
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }

    void Overlay::RenderButton()
    {
        Layered& w = m_button;
        w.width = (int)S(104);
        w.height = (int)S(40);
        w.pos = { m_video.left + (LONG)S(24), m_video.top + (LONG)S(24) };
        w.hits = { { 0, 0, (float)w.width, (float)w.height, Action::OpenButton, 0 } };
        const bool hover = w.hover == 0;
        Present(w, [&](gp::Graphics & g) {
            const float bw = (float)w.width - 1, bh = (float)w.height - 1;
            FillRoundRect(g, hover ? gp::Color(240, 34, 46, 56) : gp::Color(215, 13, 20, 27), 0, 0, bw, bh, bh / 2);
            gp::GraphicsPath border;
            AddRoundRect(border, 0.5f, 0.5f, bw - 1, bh - 1, bh / 2);
            gp::Pen pen(hover ? gp::Color(200, 255, 255, 255) : gp::Color(90, 255, 255, 255), std::max(1.0f, S(1)));
            g.DrawPath(&pen, &border);
            // small "person" glyph + label
            const float cx = S(24), cy = bh / 2;
            gp::SolidBrush accent(kAccent);
            g.FillEllipse(&accent, cx - S(4), cy - S(9), S(8), S(8));
            gp::GraphicsPath body;
            body.AddArc(cx - S(7.5f), cy + S(0.5f), S(15), S(14), 180, 180);
            body.CloseFigure();
            g.FillPath(&accent, &body);
            Text(g, L"X-Ray", g_fonts->button.get(), kText, S(38), cy - g_fonts->button->GetHeight(&g) / 2, bw - S(42));
        });
    }

    void Overlay::RenderPanel()
    {
        Layered& w = m_panel;
        if (!w.hwnd || !m_panelOpen) {
            return;
        }
        const int videoW = m_video.right - m_video.left, videoH = m_video.bottom - m_video.top;
        const float panelW = std::min((float)videoW, std::clamp(videoW * 0.34f, S(320), S(460)));
        const float fade = std::min(S(40), videoW - panelW);
        m_panelW = panelW;
        w.width = (int)(panelW + fade);
        w.height = videoH;
        w.pos = { m_video.left, m_video.top };
        std::vector<Hit> hits;

        Present(w, [&](gp::Graphics & g) {
            const float h = (float)w.height, pad = S(24);
            gp::SolidBrush panel(kPanel);
            g.FillRectangle(&panel, 0.0f, 0.0f, panelW, h);
            if (fade > 1) {
                gp::LinearGradientBrush edge(gp::PointF(panelW - 1, 0), gp::PointF(panelW + fade, 0), kPanel, gp::Color(0, kPanel.GetR(), kPanel.GetG(), kPanel.GetB()));
                g.FillRectangle(&edge, panelW - 0.5f, 0.0f, fade + 0.5f, h);
            }

            // header
            Text(g, L"X-Ray", g_fonts->title.get(), kText, pad, S(20), panelW - pad * 2 - S(40));
            const float cs = S(32), cx = panelW - pad - cs + S(6), cy = S(20);
            hits.push_back({ cx, cy, cs, cs, Action::Close, 0 });
            if (IsHovered(w, Action::Close, 0)) {
                gp::SolidBrush hb(kHover);
                g.FillEllipse(&hb, cx, cy, cs, cs);
            }
            gp::Pen xpen(kText, S(2));
            xpen.SetStartCap(gp::LineCapRound);
            xpen.SetEndCap(gp::LineCapRound);
            const float m = S(11);
            g.DrawLine(&xpen, cx + m, cy + m, cx + cs - m, cy + cs - m);
            g.DrawLine(&xpen, cx + cs - m, cy + m, cx + m, cy + cs - m);

            // tabs
            const float tabY = S(68);
            float tx = pad;
            for (int i = 0; i < 2; i++) {
                const Tab tab = i == 0 ? Tab::Scene : Tab::Cast;
                const Action a = i == 0 ? Action::TabScene : Action::TabCast;
                const std::wstring label = i == 0 ? L"In Scene" : L"All Cast";
                const float tw = TextWidth(g, label, g_fonts->tab.get());
                const bool selected = m_tab == tab;
                Text(g, label, g_fonts->tab.get(), selected || IsHovered(w, a, 0) ? kText : kSubText, tx, tabY, tw + S(4));
                if (selected) {
                    FillRoundRect(g, kAccent, tx, tabY + S(27), tw, S(3), S(1.5f));
                }
                hits.push_back({ tx - S(6), tabY - S(8), tw + S(12), S(40), a, 0 });
                tx += tw + S(30);
            }
            gp::SolidBrush sep(kSeparator);
            const float contentTop = tabY + S(31);
            g.FillRectangle(&sep, 0.0f, contentTop - S(1), panelW, std::max(1.0f, S(1)));

            // footer
            const float footerH = S(34);
            g.FillRectangle(&sep, 0.0f, h - footerH, panelW, std::max(1.0f, S(1)));
            std::wstring footer;
            if (m_busy && m_result) {
                footer = m_progress;
            } else if (m_result && m_result->movie) {
                footer = m_result->movie->provider == "tmdb"
                         ? L"Cast data and photos: TMDB. This product uses the TMDB API but is not endorsed or certified by TMDB."
                         : L"Cast data: Wikidata \u00b7 Photos: Wikimedia Commons";
            }
            Text(g, footer, g_fonts->caption.get(), kDimText, pad, h - footerH + S(10), panelW - pad * 2);

            // scrollable content
            m_viewHeight = h - footerH - contentTop;
            g.SetClip(gp::RectF(0, contentTop, panelW, m_viewHeight));
            std::vector<Hit> contentHits;
            float y = contentTop - m_scroll;
            const float start = y;
            if (m_detail >= 0 && m_result && m_result->movie && m_detail < (int)m_result->movie->cast.size()) {
                y = PaintDetails(g, y, contentHits);
            } else if (m_tab == Tab::Scene) {
                y = PaintSceneTab(g, y, contentHits);
            } else {
                y = PaintCastTab(g, y, contentHits);
            }
            m_contentHeight = y - start + S(12);
            g.ResetClip();
            // only the visible part of the content is clickable
            for (auto& hit : contentHits) {
                const float top = std::max(hit.y, contentTop), bottom = std::min(hit.y + hit.h, h - footerH);
                if (bottom > top) {
                    hits.push_back({ hit.x, top, hit.w, bottom - top, hit.action, hit.param });
                }
            }

            // scroll indicator
            if (m_contentHeight > m_viewHeight + 1) {
                const float trackH = m_viewHeight - S(8);
                const float thumbH = std::max(S(24), trackH * m_viewHeight / m_contentHeight);
                const float thumbY = contentTop + S(4) + (trackH - thumbH) * (m_scroll / (m_contentHeight - m_viewHeight));
                FillRoundRect(g, gp::Color(70, 255, 255, 255), panelW - S(6), thumbY, S(3), thumbH, S(1.5f));
            }
        });
        // keep the scroll position valid when the content shrinks
        const float maxScroll = std::max(0.0f, m_contentHeight - m_viewHeight);
        if (m_scroll > maxScroll) {
            m_scroll = maxScroll;
        }
        w.hits = std::move(hits);
        if (w.hover >= (int)w.hits.size()) {
            w.hover = -1;
        }
    }

    void Overlay::PaintPhoto(gp::Graphics& g, const std::wstring& path, float x, float y, float w, float h, float radius)
    {
        gp::GraphicsPath clip;
        AddRoundRect(clip, x, y, w, h, radius);
        gp::Bitmap* bmp = GetImage(path);
        if (!bmp) {
            gp::SolidBrush bg(kPlaceholder);
            g.FillPath(&bg, &clip);
            gp::Region old;
            g.GetClip(&old);
            g.SetClip(&clip, gp::CombineModeIntersect);
            gp::SolidBrush fg(kSilhouette);
            const float head = w * 0.38f;
            g.FillEllipse(&fg, x + (w - head) / 2, y + h * 0.22f, head, head);
            g.FillEllipse(&fg, x + w * 0.12f, y + h * 0.22f + head * 1.08f, w * 0.76f, h * 0.7f);
            g.SetClip(&old);
            return;
        }
        // "cover" crop, biased to the top where the face usually is
        const float bw = (float)bmp->GetWidth(), bh = (float)bmp->GetHeight();
        float sx = 0, sy = 0, sw = bw, sh = bh;
        if (bw / bh > w / h) {
            sw = bh * w / h;
            sx = (bw - sw) / 2;
        } else {
            sh = bw * h / w;
            sy = (bh - sh) * 0.2f;
        }
        gp::Region old;
        g.GetClip(&old);
        g.SetClip(&clip, gp::CombineModeIntersect);
        g.DrawImage(bmp, gp::RectF(x, y, w, h), sx, sy, sw, sh, gp::UnitPixel);
        g.SetClip(&old);
    }

    void Overlay::PaintPill(gp::Graphics& g, float x, float y, float w, float h, const wchar_t* text, bool primary, Action a, int param, std::vector<Hit>& hits)
    {
        const bool hover = IsHovered(m_panel, a, param);
        if (primary) {
            FillRoundRect(g, hover ? gp::Color(255, 64, 172, 255) : kAccent, x, y, w, h, h / 2);
        } else {
            FillRoundRect(g, hover ? gp::Color(60, 255, 255, 255) : gp::Color(34, 255, 255, 255), x, y, w, h, h / 2);
        }
        const float th = g_fonts->button->GetHeight(&g);
        Text(g, text, g_fonts->button.get(), kText, x + S(8), y + (h - th) / 2, w - S(16), 1, gp::StringAlignmentCenter);
        hits.push_back({ x, y, w, h, a, param });
    }

    float Overlay::PaintMessage(gp::Graphics& g, float y, const std::wstring& title, const std::wstring& text)
    {
        const float pad = S(24), w = m_panelW - pad * 2;
        y += S(20);
        if (!title.empty()) {
            y += Text(g, title, g_fonts->name.get(), kText, pad, y, w, 2) + S(6);
        }
        if (!text.empty()) {
            y += Text(g, text, g_fonts->note.get(), kSubText, pad, y, w, 12) + S(4);
        }
        return y;
    }

    float Overlay::PaintCard(gp::Graphics& g, float y, int castIndex, std::vector<Hit>& hits)
    {
        const Person& p = m_result->movie->cast[castIndex];
        const float panelW = m_panelW, pad = S(24);
        const float cardH = S(100), photoW = S(60), photoH = S(80);
        if (IsHovered(m_panel, Action::Person, castIndex)) {
            FillRoundRect(g, kHover, S(10), y + S(2), panelW - S(20), cardH - S(4), S(8));
        }
        PaintPhoto(g, p.imagePath, pad, y + S(10), photoW, photoH, S(6));
        const float tx = pad + photoW + S(16), tw = panelW - tx - pad - S(14);
        float ty = y + S(16);
        ty += Text(g, p.name, g_fonts->name.get(), kText, tx, ty, tw) + S(3);
        if (!p.character.empty()) {
            ty += Text(g, L"as " + p.character, g_fonts->note.get(), kSubText, tx, ty, tw, 2) + S(3);
        }
        Text(g, LifeLine(p), g_fonts->note.get(), kDimText, tx, ty, tw);
        // chevron
        gp::Pen chev(kSubText, S(1.8f));
        chev.SetStartCap(gp::LineCapRound);
        chev.SetEndCap(gp::LineCapRound);
        const float chx = panelW - pad - S(4), chy = y + cardH / 2;
        g.DrawLine(&chev, chx - S(4), chy - S(6), chx + S(2), chy);
        g.DrawLine(&chev, chx + S(2), chy, chx - S(4), chy + S(6));
        hits.push_back({ S(10), y, panelW - S(20), cardH, Action::Person, castIndex });
        return y + cardH;
    }

    float Overlay::PaintSceneTab(gp::Graphics& g, float y, std::vector<Hit>& hits)
    {
        const float pad = S(24), panelW = m_panelW;
        if (m_needConsent) {
            const int mb = m_service->GetEngineDownloadSizeMB();
            y = PaintMessage(g, y, L"Recognize the actors on screen",
                             L"X-Ray finds the faces in the paused scene and matches them against the cast photos, "
                             L"entirely on this computer with free open-source AI models (OpenCV YuNet and SFace on ONNX Runtime).\n\n"
                             L"This needs a one-time download of about " + std::to_wstring(mb) + L" MB.");
            y += S(14);
            const float bw = (panelW - pad * 2 - S(10)) / 2;
            PaintPill(g, pad, y, bw, S(38), L"Download", true, Action::Download, 0, hits);
            PaintPill(g, pad + bw + S(10), y, bw, S(38), L"Cast list only", false, Action::CastOnly, 0, hits);
            return y + S(38);
        }
        auto paintSpinner = [&](float top) {
            const float sz = S(26), sx = pad, sy = top + S(24);
            gp::Pen track(gp::Color(40, 255, 255, 255), S(3));
            g.DrawEllipse(&track, sx, sy, sz, sz);
            gp::Pen arc(kAccent, S(3));
            arc.SetStartCap(gp::LineCapRound);
            arc.SetEndCap(gp::LineCapRound);
            g.DrawArc(&arc, sx, sy, sz, sz, (float)(m_spin * 24 % 360), 100);
            Text(g, m_progress, g_fonts->body.get(), kSubText, sx + sz + S(14), sy + (sz - g_fonts->body->GetHeight(&g)) / 2, panelW - sx - sz - S(14) - pad, 2);
            return sy + sz;
        };
        if (m_busy && (!m_result || m_result->status != Result::Status::Ok || !m_result->movie)) {
            return paintSpinner(y);
        }
        if (!m_result) {
            return y;
        }
        if (m_result->status != Result::Status::Ok || !m_result->movie) {
            y = PaintMessage(g, y, m_result->status == Result::Status::MovieNotFound ? L"Title not recognized" : L"X-Ray isn't available",
                             m_result->message + (m_result->status == Result::Status::MovieNotFound
                                                  ? L"\n\nX-Ray identifies the movie from its file name, e.g. \u201cThe.Matrix.1999.1080p.mkv\u201d or \u201cBreaking Bad S01E01.mkv\u201d."
                                                  : L""));
            y += S(14);
            PaintPill(g, pad, y, S(130), S(36), L"Try again", false, Action::Retry, 0, hits);
            return y + S(36);
        }

        const Movie& movie = *m_result->movie;
        y += S(16);
        std::wstring title = movie.title;
        if (movie.year > 0) {
            title += L" \u00b7 " + std::to_wstring(movie.year);
        }
        if (movie.isEpisode) {
            wchar_t ep[32];
            swprintf_s(ep, L" \u00b7 S%02dE%02d", movie.season, movie.episode);
            title += ep;
        }
        y += Text(g, title, g_fonts->note.get(), kSubText, pad, y, panelW - pad * 2, 2) + S(4);

        if (!m_result->recognitionAvailable) {
            y = PaintMessage(g, y, L"Face recognition is off", m_result->message.empty() ? L"Download the free face recognition models to see who is on screen." : m_result->message);
            y += S(14);
            const std::wstring label = L"Enable (" + std::to_wstring(m_service->GetEngineDownloadSizeMB()) + L" MB)";
            PaintPill(g, pad, y, S(150), S(36), label.c_str(), true, Action::Download, 0, hits);
            PaintPill(g, pad + S(160), y, S(130), S(36), L"Browse cast", false, Action::TabCast, 0, hits);
            return y + S(36);
        }
        if (m_result->scene.empty()) {
            if (m_busy) {
                // partial result: the rest of the scene is still being analyzed
                return paintSpinner(y);
            }
            if (m_result->facesDetected < 0 && !m_result->sceneAnalyzed) {
                y = PaintMessage(g, y, L"Can't see the video", m_result->message + L" You can still browse the whole cast.");
                y += S(14);
                PaintPill(g, pad, y, S(130), S(36), L"Browse cast", false, Action::TabCast, 0, hits);
                return y + S(36);
            }
            std::wstring text;
            if (m_result->sceneAnalyzed) {
                text = L"No one from the cast was recognized in the last "
                       + std::to_wstring(std::max<int64_t>(1, (m_result->sceneEndMs - m_result->sceneStartMs + 500) / 1000))
                       + L" seconds of the scene (" + std::to_wstring(m_result->framesAnalyzed) + L" frames analyzed).";
            } else {
                text = m_result->facesDetected <= 0
                       ? L"No faces were found in this frame."
                       : std::to_wstring(m_result->facesDetected) + (m_result->facesDetected == 1 ? L" face was" : L" faces were")
                       + L" found, but no one matched the cast photos closely enough.";
            }
            if (!m_result->sceneNote.empty()) {
                text += L"\n\n" + m_result->sceneNote;
            }
            y = PaintMessage(g, y, L"No cast members recognized", text + L" Try pausing on a clear, front-facing close-up.");
            y += S(14);
            PaintPill(g, pad, y, S(130), S(36), L"Browse cast", false, Action::TabCast, 0, hits);
            return y + S(36);
        }
        for (int section = 0; section < 2; section++) {
            const bool onScreen = section == 0;
            bool header = false;
            for (const auto& m : m_result->scene) {
                if (m.onScreen != onScreen || m.castIndex < 0 || m.castIndex >= (int)movie.cast.size()) {
                    continue;
                }
                if (!header) {
                    y += S(8);
                    Text(g, onScreen ? L"ON SCREEN NOW" : L"ALSO IN THIS SCENE", g_fonts->caption.get(), kDimText, pad, y, panelW - pad * 2);
                    y += S(22);
                    header = true;
                }
                y = PaintCard(g, y, m.castIndex, hits);
            }
        }
        if (m_busy) {
            y = paintSpinner(y - S(8));
        } else if (!m_result->sceneNote.empty()) {
            y += S(10);
            y += Text(g, m_result->sceneNote, g_fonts->note.get(), kDimText, pad, y, panelW - pad * 2, 3);
        }
        return y;
    }

    float Overlay::PaintCastTab(gp::Graphics& g, float y, std::vector<Hit>& hits)
    {
        const float pad = S(24), panelW = m_panelW;
        if (!m_result || !m_result->movie) {
            if (m_needConsent || m_busy) {
                return PaintSceneTab(g, y, hits);
            }
            return PaintMessage(g, y, L"No cast information", m_result ? m_result->message : L"");
        }
        const Movie& movie = *m_result->movie;
        y += S(16);
        Text(g, std::to_wstring(movie.cast.size()) + L" CAST MEMBERS", g_fonts->caption.get(), kDimText, pad, y, panelW - pad * 2);
        y += S(22);
        for (int i = 0; i < (int)movie.cast.size(); i++) {
            y = PaintCard(g, y, i, hits);
        }
        return y;
    }

    float Overlay::PaintDetails(gp::Graphics& g, float y, std::vector<Hit>& hits)
    {
        const Movie& movie = *m_result->movie;
        const Person& p = movie.cast[m_detail];
        const float pad = S(24), panelW = m_panelW, w = panelW - pad * 2;

        // back
        y += S(12);
        const bool hb = IsHovered(m_panel, Action::Back, 0);
        if (hb) {
            FillRoundRect(g, kHover, pad - S(10), y, S(86), S(32), S(16));
        }
        gp::Pen chev(kText, S(2));
        chev.SetStartCap(gp::LineCapRound);
        chev.SetEndCap(gp::LineCapRound);
        const float cy = y + S(16);
        g.DrawLine(&chev, pad + S(5), cy - S(6), pad - S(1), cy);
        g.DrawLine(&chev, pad - S(1), cy, pad + S(5), cy + S(6));
        Text(g, L"Back", g_fonts->button.get(), kText, pad + S(14), cy - g_fonts->button->GetHeight(&g) / 2, S(60));
        hits.push_back({ pad - S(10), y, S(86), S(32), Action::Back, 0 });
        y += S(46);

        const float photoW = std::min(S(170), w), photoH = photoW * 4 / 3;
        PaintPhoto(g, p.imagePath, pad, y, photoW, photoH, S(8));
        y += photoH + S(18);
        y += Text(g, p.name, g_fonts->bigName.get(), kText, pad, y, w, 2) + S(2);
        if (!p.character.empty()) {
            y += Text(g, L"as " + p.character, g_fonts->body.get(), kSubText, pad, y, w, 2);
        }
        y += S(14);

        if (!p.detailsLoaded && m_details->pending.count(m_detail)) {
            const float sz = S(18);
            gp::Pen arc(kAccent, S(2.5f));
            arc.SetStartCap(gp::LineCapRound);
            arc.SetEndCap(gp::LineCapRound);
            g.DrawArc(&arc, pad, y, sz, sz, (float)(m_spin * 24 % 360), 100);
            Text(g, L"Loading details\u2026", g_fonts->note.get(), kSubText, pad + sz + S(10), y, w - sz - S(10));
            y += sz + S(12);
        }

        auto row = [&](const wchar_t* label, const std::wstring & value) {
            if (value.empty()) {
                return;
            }
            Text(g, label, g_fonts->caption.get(), kDimText, pad, y, w);
            y += S(18);
            y += Text(g, value, g_fonts->body.get(), kText, pad, y, w, 3) + S(12);
        };
        const Date born = ParseDate(p.birthday), died = ParseDate(p.deathday);
        std::wstring bornText = FormatDate(born);
        if (!p.birthplace.empty()) {
            bornText += (bornText.empty() ? L"" : L" in ") + p.birthplace;
        }
        row(L"BORN", bornText);
        if (died.valid()) {
            row(L"DIED", FormatDate(died) + (born.valid() ? L"  (aged " + std::to_wstring(Age(born, died)) + L")" : L""));
        } else if (born.valid()) {
            row(L"AGE", std::to_wstring(Age(born, Today())) + L" years old");
        }

        const std::wstring current = Lower(movie.title);
        std::vector<const KnownFor*> known;
        for (const auto& k : p.knownFor) {
            if (Lower(k.title) != current) {
                known.push_back(&k);
            }
        }
        if (!known.empty()) {
            y += S(4);
            Text(g, L"ALSO KNOWN FOR", g_fonts->caption.get(), kDimText, pad, y, w);
            y += S(22);
            for (const KnownFor* k : known) {
                const float yearW = k->year > 0 ? S(44) : 0;
                const float th = Text(g, k->title, g_fonts->body.get(), kText, pad, y, w - yearW - S(8), 2);
                if (k->year > 0) {
                    Text(g, std::to_wstring(k->year), g_fonts->note.get(), kSubText, pad + w - yearW, y + S(1), yearW, 1, gp::StringAlignmentFar);
                }
                y += th + S(8);
                gp::SolidBrush sep(gp::Color(18, 255, 255, 255));
                g.FillRectangle(&sep, pad, y - S(4), w, std::max(1.0f, S(1)));
                y += S(4);
            }
        }
        return y;
    }
}
