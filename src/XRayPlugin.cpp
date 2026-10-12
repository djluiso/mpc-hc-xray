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

// X-Ray as an MpcPluginHost plugin: glue between the plugin SDK and the X-Ray overlay

#include "../sdk/MpcPlugin.h"
#include "XRayOverlay.h"
#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <memory>

namespace
{
    constexpr wchar_t kLegacySettingsKey[] = L"Software\\MPC-HC\\MPC-HC\\Settings"; // X-Ray built into the player

    std::wstring HostString(const MpcHostApi* api, int(__cdecl* fn)(void*, wchar_t*, int))
    {
        const int n = fn(api->host, nullptr, 0);
        if (n <= 0) {
            return std::wstring();
        }
        std::wstring s(size_t(n) + 1, L'\0');
        fn(api->host, &s[0], n + 1);
        s.resize(n);
        return s;
    }

    std::wstring WithSlash(std::wstring d)
    {
        if (!d.empty() && d.back() != L'\\') {
            d += L'\\';
        }
        return d;
    }

    bool HasFFmpeg(const std::wstring& dir)
    {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"avcodec-lav-*.dll").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) {
            return false;
        }
        FindClose(f);
        return true;
    }

    bool IsAudioFile(const std::wstring& path)
    {
        const size_t dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos) {
            return false;
        }
        std::wstring ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
        for (const wchar_t* e : { L"mp3", L"flac", L"m4a", L"aac", L"ogg", L"oga", L"opus", L"wav", L"wma", L"ape", L"wv", L"mka",
                                  L"ac3", L"dts", L"mpc", L"aiff", L"aif", L"alac", L"tta", L"m4b", L"mid", L"midi" }) {
            if (ext == e) {
                return true;
            }
        }
        return false;
    }

    class XRayPlugin : public XRay::OverlayHost
    {
    public:
        explicit XRayPlugin(const MpcHostApi* api)
            : m_api(api)
        {
            m_escapeMessage = RegisterWindowMessageW(L"MpcPluginHost.XRay.Escape");
            m_plugin.cbSize = sizeof(MpcPlugin);
            m_plugin.apiVersion = MPC_PLUGIN_API_VERSION;
            m_plugin.id = L"XRay";
            m_plugin.name = L"X-Ray";
            m_plugin.version = L"1.0";
            m_plugin.description = L"Prime Video like X-Ray: who is on screen when the video is paused";
            m_plugin.instance = this;
            m_plugin.Destroy = [](void* p) {
                delete static_cast<XRayPlugin*>(p);
            };
            m_plugin.OnEvent = [](void* p, int event, intptr_t param) {
                static_cast<XRayPlugin*>(p)->OnEvent(event, param);
            };
            m_plugin.PreTranslateMessage = [](void* p, const MSG * msg) -> BOOL {
                return static_cast<XRayPlugin*>(p)->PreTranslateMessage(msg);
            };
        }

        ~XRayPlugin() override
        {
            SetEscapeHook(false);
            m_overlay.reset();
        }

        const MpcPlugin* Plugin() const { return &m_plugin; }

        // OverlayHost
        HWND GetOwnerWindow() override
        {
            return m_api->GetOverlayOwner(m_api->host);
        }

        bool GetVideoRect(RECT& rc) override
        {
            if (IsAudioFile(GetMediaPath())) {
                return false;
            }
            return !!m_api->GetVideoRect(m_api->host, &rc);
        }

        bool CaptureFrame(XRay::ImageBGR& frame, std::wstring& error) override
        {
            // with a local file, X-Ray decodes the paused frame itself at full quality
            if (!GetMediaPath().empty() && !m_videoLibDirs.empty()) {
                for (const auto& dir : m_videoLibDirs) {
                    if (HasFFmpeg(dir)) {
                        return false;
                    }
                }
            }
            struct Sink {
                XRay::ImageBGR* frame;
                bool ok;
            } sink{ &frame, false };
            wchar_t err[512] = L"";
            const BOOL captured = m_api->CaptureFrame(m_api->host, [](void* ctx, const BITMAPINFOHEADER * dib, size_t size) {
                auto* s = static_cast<Sink*>(ctx);
                s->ok = XRay::ImageFromDIB(reinterpret_cast<const uint8_t*>(dib), size, *s->frame);
            }, &sink, err, _countof(err));
            if (!captured || !sink.ok) {
                error = *err ? err : L"Unable to capture the current frame";
                return false;
            }
            return true;
        }

        std::wstring GetMediaName() override
        {
            return HostString(m_api, m_api->GetMediaTitle);
        }

        std::wstring GetMediaPath() override
        {
            return HostString(m_api, m_api->GetMediaPath);
        }

        int64_t GetPositionMs() override
        {
            return m_api->GetPositionMs(m_api->host);
        }

        std::string GetTmdbApiKey() override
        {
            std::wstring key = GetSetting(L"TMDbApiKey", L"XRayTMDbApiKey");
            if (key.empty()) {
                wchar_t env[1024];
                const DWORD len = GetEnvironmentVariableW(L"TMDB_API_KEY", env, _countof(env));
                if (len > 0 && len < _countof(env)) {
                    key = env;
                }
            }
            key.erase(0, key.find_first_not_of(L" \t"));
            key.erase(key.find_last_not_of(L" \t") + 1);
            if (key.empty()) {
                return std::string();
            }
            const int n = WideCharToMultiByte(CP_UTF8, 0, key.c_str(), (int)key.size(), nullptr, 0, nullptr, nullptr);
            std::string utf8(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, key.c_str(), (int)key.size(), &utf8[0], n, nullptr, nullptr);
            return utf8;
        }

        int GetEngineDownloadConsent() override
        {
            const std::wstring v = GetSetting(L"EngineConsent", L"XRayEngineConsent");
            return v.empty() ? -1 : _wtoi(v.c_str());
        }

        void SetEngineDownloadConsent(bool accepted) override
        {
            m_api->SetSetting(m_api->host, L"EngineConsent", accepted ? L"1" : L"0");
        }

        UINT GetDpi() override
        {
            return m_api->GetDpi(m_api->host);
        }

    private:
        // Plugin setting, falling back to the value saved by the X-Ray version that was built into the player
        std::wstring GetSetting(const wchar_t* key, const wchar_t* legacyName)
        {
            const wchar_t* kUnset = L"\x01";
            wchar_t buf[2048];
            m_api->GetSetting(m_api->host, key, kUnset, buf, _countof(buf));
            if (wcscmp(buf, kUnset) != 0) {
                return buf;
            }
            DWORD dw = 0, size = sizeof(dw);
            if (RegGetValueW(HKEY_CURRENT_USER, kLegacySettingsKey, legacyName, RRF_RT_REG_DWORD, nullptr, &dw, &size) == ERROR_SUCCESS) {
                return std::to_wstring((int)dw);
            }
            size = sizeof(buf);
            if (RegGetValueW(HKEY_CURRENT_USER, kLegacySettingsKey, legacyName, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
                return buf;
            }
            return std::wstring();
        }

        void EnsureOverlay()
        {
            if (m_overlay) {
                return;
            }
            const std::wstring dataDir = WithSlash(HostString(m_api, m_api->GetDataDir));
            const std::wstring pluginDir = WithSlash(HostString(m_api, m_api->GetPluginDir));
            const std::wstring programDir = WithSlash(HostString(m_api, m_api->GetProgramDir));
            // FFmpeg for scene analysis: shipped with the plugin, else the player's LAV Filters
            m_videoLibDirs.clear();
            m_videoLibDirs.push_back(pluginDir + L"ffmpeg\\");
            if (!programDir.empty()) {
#ifdef _WIN64
                m_videoLibDirs.push_back(programDir + L"LAVFilters64\\");
#else
                m_videoLibDirs.push_back(programDir + L"LAVFilters\\");
#endif
            }
            m_overlay = std::make_unique<XRay::Overlay>(*this, dataDir + L"XRay", pluginDir);
            m_overlay->SetVideoLibraryDirs(m_videoLibDirs);
        }

        void OnEvent(int event, intptr_t param)
        {
            switch (event) {
                case MPC_EVENT_PLAYSTATE_CHANGED: {
                    const bool paused = param == MPC_PLAYSTATE_PAUSED;
                    if (!m_overlay && !paused) {
                        return;
                    }
                    EnsureOverlay();
                    m_overlay->SetPaused(paused);
                    SetEscapeHook(paused);
                    break;
                }
                case MPC_EVENT_MEDIA_OPENED:
                case MPC_EVENT_MEDIA_CLOSED:
                case MPC_EVENT_PLAYER_DISCONNECTED:
                    if (m_overlay) {
                        m_overlay->Reset();
                    }
                    SetEscapeHook(false);
                    break;
                case MPC_EVENT_LAYOUT_CHANGED:
                    if (m_overlay) {
                        m_overlay->UpdatePosition();
                    }
                    break;
            }
        }

        BOOL PreTranslateMessage(const MSG* msg)
        {
            if (!m_overlay) {
                return FALSE;
            }
            if (msg->hwnd == nullptr && msg->message == m_escapeMessage) {
                MSG esc{};
                esc.message = WM_KEYDOWN;
                esc.wParam = VK_ESCAPE;
                m_overlay->PreTranslateMessage(&esc);
                return TRUE;
            }
            return m_overlay->PreTranslateMessage(msg);
        }

        // Escape closes the panel like in the player: the keyboard focus is in the player (another process),
        // so a low level keyboard hook is used, only while paused
        void SetEscapeHook(bool on)
        {
            if (on && !m_hook) {
                s_instance = this;
                m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, ModuleHandle(), 0);
            } else if (!on && m_hook) {
                UnhookWindowsHookEx(m_hook);
                m_hook = nullptr;
                s_instance = nullptr;
            }
        }

        static HINSTANCE ModuleHandle()
        {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&KeyboardProc), &m);
            return m;
        }

        static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp)
        {
            XRayPlugin* self = s_instance;
            if (code == HC_ACTION && self && self->m_overlay && self->m_overlay->IsPanelOpen() && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
                const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
                if (k->vkCode == VK_ESCAPE) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
                    if (pid && (pid == self->m_api->GetPlayerProcessId(self->m_api->host) || pid == GetCurrentProcessId())) {
                        PostThreadMessageW(GetCurrentThreadId(), self->m_escapeMessage, 0, 0);
                        return 1; // the player must not see it (it would leave fullscreen)
                    }
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        const MpcHostApi* m_api;
        MpcPlugin m_plugin{};
        std::unique_ptr<XRay::Overlay> m_overlay;
        std::vector<std::wstring> m_videoLibDirs;
        HHOOK m_hook = nullptr;
        UINT m_escapeMessage = 0;
        static XRayPlugin* s_instance;
    };

    XRayPlugin* XRayPlugin::s_instance = nullptr;
}

extern "C" __declspec(dllexport) const MpcPlugin* __cdecl MpcPluginCreate(const MpcHostApi* host)
{
    if (!host || host->apiVersion < 1 || host->cbSize < offsetof(MpcHostApi, GetPlayerProcessId) + sizeof(void*) * 2) {
        return nullptr;
    }
    auto* plugin = new XRayPlugin(host);
    return plugin->Plugin();
}
