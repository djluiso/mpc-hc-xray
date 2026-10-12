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

// Decodes frames of a local video file independently of the playback graph, so that X-Ray can look at the
// whole scene around the paused position (several shots / camera angles) instead of a single frame.
// Uses the FFmpeg DLLs shipped with the internal LAV Filters, loaded dynamically; nothing is linked at build
// time and the feature silently degrades to single frame analysis when they are missing.
// This file deliberately has no MFC dependency.

#include "XRayFaceEngine.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace XRay
{
    class VideoSampler
    {
    public:
        // libDir: folder containing avformat-lav-*.dll, avcodec-lav-*.dll, avutil-lav-*.dll and swscale-lav-*.dll
        explicit VideoSampler(const std::wstring& libDir);
        ~VideoSampler();
        VideoSampler(const VideoSampler&) = delete;
        VideoSampler& operator=(const VideoSampler&) = delete;

        bool Open(const std::wstring& path, std::wstring& error);
        void Close();
        bool IsOpen() const;
        int64_t GetDurationMs() const;

        // Decodes [startMs, endMs] (player timeline) and calls onFrame about every stepMs with a BGR frame whose
        // width is at most maxWidth (display aspect ratio applied). onFrame returns false to stop early.
        bool Sample(int64_t startMs, int64_t endMs, int64_t stepMs, int maxWidth,
                    const std::function<bool(int64_t timeMs, ImageBGR& frame)>& onFrame, const std::atomic<bool>& cancel);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
