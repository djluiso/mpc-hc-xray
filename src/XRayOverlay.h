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

#pragma once

// X-Ray overlay UI: an "X-Ray" button shown over the paused video and a Prime Video like side panel
// listing the actors in the scene and the whole cast. Plain Win32 + GDI+ (layered popup windows),
// the player is abstracted by OverlayHost so that this file has no MFC dependency.

#include "XRayService.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Gdiplus
{
    class Bitmap;
    class Graphics;
    class RectF;
}

namespace XRay
{
    class OverlayHost
    {
    public:
        virtual ~OverlayHost() = default;

        // Window that owns the overlay (stays below it), usually the main frame or the fullscreen window
        virtual HWND GetOwnerWindow() = 0;
        // Screen rectangle of the video area, false if the overlay can't be shown (minimized, D3D exclusive...)
        virtual bool GetVideoRect(RECT& screenRect) = 0;
        virtual bool CaptureFrame(ImageBGR& frame, std::wstring& error) = 0;
        // Full path or title of the playing media, used to identify the movie
        virtual std::wstring GetMediaName() = 0;
        // local file being played, empty for streams and discs (no scene analysis then)
        virtual std::wstring GetMediaPath() = 0;
        virtual int64_t GetPositionMs() = 0;
        // Optional TMDb v3 API key / v4 read access token, empty to use Wikidata only
        virtual std::string GetTmdbApiKey() = 0;
        // -1: never asked, 0: declined, 1: accepted
        virtual int GetEngineDownloadConsent() = 0;
        virtual void SetEngineDownloadConsent(bool accepted) = 0;
        virtual UINT GetDpi() = 0;
    };

    class Overlay
    {
    public:
        Overlay(OverlayHost& host, const std::wstring& cacheDir, const std::wstring& appDir);
        ~Overlay();
        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        // Shows the X-Ray button while paused, closes everything when playback resumes
        void SetPaused(bool paused);
        // Media closed or changed: hide and forget the current results
        void Reset();
        void OpenPanel();
        void ClosePanel();
        bool IsPanelOpen() const { return m_panelOpen; }
        // Re-syncs the windows with the video area (call on move/resize/fullscreen changes)
        void UpdatePosition();
        // Lets the overlay consume mouse wheel / Escape messages destined to the player, true if consumed
        bool PreTranslateMessage(const MSG* msg);
        // See Service::SetVideoLibraryDirs
        void SetVideoLibraryDirs(std::vector<std::wstring> dirs) { m_service->SetVideoLibraryDirs(std::move(dirs)); }

    private:
        enum class Action { None, Close, TabScene, TabCast, Person, Back, Download, CastOnly, Retry, OpenButton };
        enum class Tab { Scene, Cast };
        struct Hit {
            float x, y, w, h;
            Action action;
            int param;
        };
        struct Layered {
            HWND hwnd = nullptr;
            int width = 0, height = 0;
            POINT pos{};
            std::vector<Hit> hits;
            int hover = -1;
            bool tracking = false;
        };
        struct DetailsState;

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        LRESULT HandleMessage(Layered& w, UINT msg, WPARAM wp, LPARAM lp);
        bool EnsureWindow(Layered& w);
        void DestroyWindows();
        void Present(Layered& w, const std::function<void(Gdiplus::Graphics&)>& paint);
        void RenderButton();
        void RenderPanel();
        float PaintSceneTab(Gdiplus::Graphics& g, float y, std::vector<Hit>& hits);
        float PaintCastTab(Gdiplus::Graphics& g, float y, std::vector<Hit>& hits);
        float PaintDetails(Gdiplus::Graphics& g, float y, std::vector<Hit>& hits);
        float PaintCard(Gdiplus::Graphics& g, float y, int castIndex, std::vector<Hit>& hits);
        float PaintMessage(Gdiplus::Graphics& g, float y, const std::wstring& title, const std::wstring& text);
        void PaintPhoto(Gdiplus::Graphics& g, const std::wstring& path, float x, float y, float w, float h, float radius);
        void PaintPill(Gdiplus::Graphics& g, float x, float y, float w, float h, const wchar_t* text, bool primary, Action a, int param, std::vector<Hit>& hits);
        void OnAction(Action a, int param);
        void StartAnalysis(bool allowDownload);
        void OnServiceNotify(WPARAM wp);
        void RequestDetails(int castIndex);
        void OnDetailsReady();
        void Scroll(float delta);
        void UpdateTimer();
        bool IsHovered(const Layered& w, Action a, int param) const;
        float S(float v) const { return v * m_scale; }
        Gdiplus::Bitmap* GetImage(const std::wstring& path);

        OverlayHost& m_host;
        std::shared_ptr<Service> m_service;
        ULONG_PTR m_gdiplusToken = 0;
        HWND m_owner = nullptr;
        HWND m_msgWnd = nullptr;
        Layered m_button, m_panel;
        bool m_paused = false;
        bool m_panelOpen = false;
        bool m_layoutDirty = true;
        float m_scale = 1.0f;
        float m_panelW = 0;
        RECT m_video{};

        // analysis state
        bool m_busy = false;
        bool m_needConsent = false;
        std::wstring m_progress;
        std::shared_ptr<Result> m_result;
        int64_t m_analyzedPos = -1;
        int m_generation = 0;
        Tab m_tab = Tab::Scene;
        int m_detail = -1;
        float m_scroll = 0, m_listScroll = 0, m_contentHeight = 0, m_viewHeight = 0;
        int m_spin = 0;
        std::shared_ptr<DetailsState> m_details;
        std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> m_images;
    };
}
