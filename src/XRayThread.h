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

#include <windows.h>
#include <functional>

namespace XRay
{
    namespace detail
    {
        inline DWORD WINAPI DetachedThreadProc(void* param)
        {
            auto* fn = static_cast<std::function<void()>*>(param);
            try {
                (*fn)();
            } catch (...) {
            }
            delete fn;
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&DetachedThreadProc), &self);
            // releases the reference taken by RunDetached: the module (when it is a plugin DLL) is unloaded only
            // once its last background thread has left its code
            FreeLibraryAndExitThread(self, 0);
        }
    }

    // Runs fn on a new background thread that keeps the module containing this code loaded until it ends
    inline void RunDetached(std::function<void()> fn)
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&detail::DetachedThreadProc), &self);
        auto* param = new std::function<void()>(std::move(fn));
        HANDLE h = CreateThread(nullptr, 0, detail::DetachedThreadProc, param, 0, nullptr);
        if (h) {
            CloseHandle(h);
            return;
        }
        // out of threads: run it synchronously rather than never
        (*param)();
        delete param;
        if (self) {
            FreeLibrary(self);
        }
    }
}
