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

/*
 * MPC-HC plugin SDK
 *
 * Plugins run inside MpcPluginHost.exe, a small companion process that works with an unmodified MPC-HC:
 * it follows the player through its built-in web interface (state, position, file, snapshot, commands) and
 * its windows (position of the video area), and lets plugins draw their own popup windows over the video.
 *
 * A plugin is a DLL placed in the "Plugins" folder next to MpcPluginHost.exe, either as
 *     Plugins\<Name>.dll            or
 *     Plugins\<Name>\<Name>.dll     (the folder may contain the plugin's own dependencies and data)
 * that exports MpcPluginCreate (see below). Its bitness must match MpcPluginHost.exe.
 *
 * Plugins are loaded and unloaded while the host runs: copying, replacing or deleting the DLL is picked up
 * within a couple of seconds. The host loads a private copy of the DLL, so the original file is never
 * locked and can be overwritten at any time. The plugin's folder is added to the DLL search path.
 *
 * The interface is plain C, so a plugin can be built with any compiler and C runtime.
 *
 * Threading: every function of MpcHostApi and MpcPlugin is called on, and must be called from, the host's
 * UI thread, which runs a regular message loop (windows, timers and hooks of plugins work as usual).
 * Plugins may use their own threads internally.
 *
 * Unloading: after MpcPlugin::Destroy nothing may run code of the plugin DLL anymore: destroy all windows
 * and timers, unregister window classes and remove hooks. Threads must be joined, or hold a reference to
 * the DLL (GetModuleHandleEx) that they release when ending (FreeLibraryAndExitThread).
 */

#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPC_PLUGIN_API_VERSION 1

/* MpcHostApi::GetPlayState */
enum MpcPlayState {
    MPC_PLAYSTATE_NONE = 0,     /* nothing loaded */
    MPC_PLAYSTATE_PLAYING = 1,
    MPC_PLAYSTATE_PAUSED = 2,
    MPC_PLAYSTATE_STOPPED = 3,
};

/* MpcPlugin::OnEvent */
enum MpcPluginEvent {
    MPC_EVENT_PLAYSTATE_CHANGED = 1,    /* param: MpcPlayState */
    MPC_EVENT_MEDIA_OPENED = 2,
    MPC_EVENT_MEDIA_CLOSED = 3,
    MPC_EVENT_LAYOUT_CHANGED = 4,       /* player window or video area moved, resized, shown or hidden */
    MPC_EVENT_PLAYER_CONNECTED = 5,     /* the host found a player and reads its state */
    MPC_EVENT_PLAYER_DISCONNECTED = 6,  /* the player was closed (MEDIA_CLOSED is sent first) */
};

/* Receives a copy of the current video frame, valid during the call only.
   dib: BITMAPINFOHEADER followed by the pixels (bottom-up unless biHeight < 0). */
typedef void (__cdecl* MpcFrameSink)(void* context, const BITMAPINFOHEADER* dib, size_t size);

typedef struct MpcHostApi {
    uint32_t cbSize;            /* sizeof(MpcHostApi) of the host, check it before using newer members */
    uint32_t apiVersion;        /* MPC_PLUGIN_API_VERSION of the host */
    void* host;                 /* pass as first argument of every function below */

    /* String getters copy at most cch characters including the terminator into buf (which may be NULL
       when cch is 0) and return the full length without the terminator. */

    /* Main window of the player, NULL when no player is running */
    HWND(__cdecl* GetMainWindow)(void* host);
    /* Top level window that overlay popups should be owned by (main window or the fullscreen window) */
    HWND(__cdecl* GetOverlayOwner)(void* host);
    /* Screen rectangle of the video. FALSE when no video is visible or nothing can be drawn over it
       (audio only, minimized, exclusive D3D fullscreen). */
    BOOL(__cdecl* GetVideoRect)(void* host, RECT* screenRect);
    int(__cdecl* GetPlayState)(void* host);
    int64_t(__cdecl* GetPositionMs)(void* host);
    int64_t(__cdecl* GetDurationMs)(void* host);
    /* Title of the playing media, typically the full path or the playlist title */
    int(__cdecl* GetMediaTitle)(void* host, wchar_t* buf, int cch);
    /* Path of the playing local file, empty for streams, discs and devices */
    int(__cdecl* GetMediaPath)(void* host, wchar_t* buf, int cch);
    /* Copies the current video frame to sink (from the player's web interface, requires its "Enable
       preview" option). FALSE and an error message on failure. Blocks for up to a couple of seconds. */
    BOOL(__cdecl* CaptureFrame)(void* host, MpcFrameSink sink, void* context, wchar_t* error, int errorCch);

    /* Per plugin settings, stored in the host's settings file */
    int(__cdecl* GetSetting)(void* host, const wchar_t* key, const wchar_t* defaultValue, wchar_t* buf, int cch);
    BOOL(__cdecl* SetSetting)(void* host, const wchar_t* key, const wchar_t* value);

    /* Folders, with a trailing backslash */
    int(__cdecl* GetProgramDir)(void* host, wchar_t* buf, int cch);     /* folder of the player executable (empty if unknown) */
    int(__cdecl* GetPluginDir)(void* host, wchar_t* buf, int cch);      /* folder of the original plugin DLL */
    /* Writable data folder: %LOCALAPPDATA%\MPC-HC, or the host's folder in portable mode.
       Plugins should use a sub folder named after their id. */
    int(__cdecl* GetDataDir)(void* host, wchar_t* buf, int cch);

    UINT(__cdecl* GetDpi)(void* host);
    void(__cdecl* Log)(void* host, const wchar_t* message);

    /* Process id of the player, 0 when no player is running */
    DWORD(__cdecl* GetPlayerProcessId)(void* host);
    /* Executes a player command (ID_* / wm_command of the web interface, e.g. 887 = play, 888 = pause) */
    BOOL(__cdecl* SendCommand)(void* host, int command);
} MpcHostApi;

typedef struct MpcPlugin {
    uint32_t cbSize;            /* sizeof(MpcPlugin) */
    uint32_t apiVersion;        /* MPC_PLUGIN_API_VERSION the plugin was built with */
    const wchar_t* id;          /* short unique identifier, e.g. L"XRay" (settings section, data folder) */
    const wchar_t* name;
    const wchar_t* version;
    const wchar_t* description;
    void* instance;             /* passed back to the functions below */

    void(__cdecl* Destroy)(void* instance);
    /* MpcPluginEvent. Right after creation the host sends the current state
       (PLAYER_CONNECTED, MEDIA_OPENED and PLAYSTATE_CHANGED as applicable). */
    void(__cdecl* OnEvent)(void* instance, int event, intptr_t param);
    /* Messages of the host's UI thread (the plugin's own windows and thread messages) before they are
       dispatched. Return TRUE to swallow the message. May be NULL. */
    BOOL(__cdecl* PreTranslateMessage)(void* instance, const MSG* msg);
} MpcPlugin;

/* Exported by the plugin. The returned object must stay valid until Destroy is called.
   Return NULL to refuse loading (e.g. incompatible apiVersion). host stays valid until Destroy. */
typedef const MpcPlugin* (__cdecl* MpcPluginCreateFn)(const MpcHostApi* host);
#define MPC_PLUGIN_CREATE_NAME "MpcPluginCreate"

#ifdef __cplusplus
}
#endif
