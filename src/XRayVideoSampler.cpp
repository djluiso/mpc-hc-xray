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

#include "XRayVideoSampler.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

#pragma warning(push, 0)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}
#pragma warning(pop)

namespace XRay
{
    namespace
    {
        // The DLLs come from the same FFmpeg tree as these headers; the major versions are checked at load time
        // because the code below accesses structure fields directly.
        struct Api {
            decltype(&::avutil_version) avutil_version = nullptr;
            decltype(&::av_frame_alloc) av_frame_alloc = nullptr;
            decltype(&::av_frame_free) av_frame_free = nullptr;
            decltype(&::av_frame_unref) av_frame_unref = nullptr;
            decltype(&::av_rescale_q) av_rescale_q = nullptr;

            decltype(&::avcodec_version) avcodec_version = nullptr;
            decltype(&::avcodec_find_decoder) avcodec_find_decoder = nullptr;
            decltype(&::avcodec_alloc_context3) avcodec_alloc_context3 = nullptr;
            decltype(&::avcodec_free_context) avcodec_free_context = nullptr;
            decltype(&::avcodec_parameters_to_context) avcodec_parameters_to_context = nullptr;
            decltype(&::avcodec_open2) avcodec_open2 = nullptr;
            decltype(&::avcodec_send_packet) avcodec_send_packet = nullptr;
            decltype(&::avcodec_receive_frame) avcodec_receive_frame = nullptr;
            decltype(&::avcodec_flush_buffers) avcodec_flush_buffers = nullptr;
            decltype(&::av_packet_alloc) av_packet_alloc = nullptr;
            decltype(&::av_packet_free) av_packet_free = nullptr;
            decltype(&::av_packet_unref) av_packet_unref = nullptr;

            decltype(&::avformat_version) avformat_version = nullptr;
            decltype(&::avformat_alloc_context) avformat_alloc_context = nullptr;
            decltype(&::avformat_open_input) avformat_open_input = nullptr;
            decltype(&::avformat_find_stream_info) avformat_find_stream_info = nullptr;
            decltype(&::avformat_close_input) avformat_close_input = nullptr;
            decltype(&::avformat_seek_file) avformat_seek_file = nullptr;
            decltype(&::av_seek_frame) av_seek_frame = nullptr;
            decltype(&::av_read_frame) av_read_frame = nullptr;

            decltype(&::swscale_version) swscale_version = nullptr;
            decltype(&::sws_getCachedContext) sws_getCachedContext = nullptr;
            decltype(&::sws_scale) sws_scale = nullptr;
            decltype(&::sws_freeContext) sws_freeContext = nullptr;
        };

        HMODULE LoadLav(const std::wstring& dir, const wchar_t* prefix)
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + prefix + L"*.dll").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) {
                return nullptr;
            }
            FindClose(h);
            // altered search path: the dependencies are resolved from the same folder
            return LoadLibraryExW((dir + fd.cFileName).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }

        template <typename T>
        bool Resolve(HMODULE module, const char* name, T& fn)
        {
            fn = reinterpret_cast<T>(GetProcAddress(module, name));
            return fn != nullptr;
        }

        const Api* LoadApi(const std::wstring& libDir, std::wstring& error)
        {
            static std::mutex lock;
            static std::unique_ptr<Api> api;
            static std::wstring loadError;
            static bool tried = false;

            std::lock_guard<std::mutex> guard(lock);
            if (tried) {
                error = loadError;
                return api.get();
            }
            tried = true;

            std::wstring dir = libDir;
            if (!dir.empty() && dir.back() != L'\\') {
                dir += L'\\';
            }
            // the modules stay loaded for the lifetime of the process (LAV Filters uses them as well)
            HMODULE util = LoadLav(dir, L"avutil-lav-");
            HMODULE codec = util ? LoadLav(dir, L"avcodec-lav-") : nullptr;
            HMODULE format = codec ? LoadLav(dir, L"avformat-lav-") : nullptr;
            HMODULE scale = format ? LoadLav(dir, L"swscale-lav-") : nullptr;
            if (!scale) {
                loadError = L"The LAV Filters FFmpeg libraries weren't found in " + dir;
                error = loadError;
                return nullptr;
            }

            auto a = std::make_unique<Api>();
            bool ok = true;
#define XRAY_RESOLVE(module, name) ok = Resolve(module, #name, a->name) && ok
            XRAY_RESOLVE(util, avutil_version);
            XRAY_RESOLVE(util, av_frame_alloc);
            XRAY_RESOLVE(util, av_frame_free);
            XRAY_RESOLVE(util, av_frame_unref);
            XRAY_RESOLVE(util, av_rescale_q);
            XRAY_RESOLVE(codec, avcodec_version);
            XRAY_RESOLVE(codec, avcodec_find_decoder);
            XRAY_RESOLVE(codec, avcodec_alloc_context3);
            XRAY_RESOLVE(codec, avcodec_free_context);
            XRAY_RESOLVE(codec, avcodec_parameters_to_context);
            XRAY_RESOLVE(codec, avcodec_open2);
            XRAY_RESOLVE(codec, avcodec_send_packet);
            XRAY_RESOLVE(codec, avcodec_receive_frame);
            XRAY_RESOLVE(codec, avcodec_flush_buffers);
            XRAY_RESOLVE(codec, av_packet_alloc);
            XRAY_RESOLVE(codec, av_packet_free);
            XRAY_RESOLVE(codec, av_packet_unref);
            XRAY_RESOLVE(format, avformat_version);
            XRAY_RESOLVE(format, avformat_alloc_context);
            XRAY_RESOLVE(format, avformat_open_input);
            XRAY_RESOLVE(format, avformat_find_stream_info);
            XRAY_RESOLVE(format, avformat_close_input);
            XRAY_RESOLVE(format, avformat_seek_file);
            XRAY_RESOLVE(format, av_seek_frame);
            XRAY_RESOLVE(format, av_read_frame);
            XRAY_RESOLVE(scale, swscale_version);
            XRAY_RESOLVE(scale, sws_getCachedContext);
            XRAY_RESOLVE(scale, sws_scale);
            XRAY_RESOLVE(scale, sws_freeContext);
#undef XRAY_RESOLVE
            if (!ok) {
                loadError = L"The LAV Filters FFmpeg libraries are missing required functions";
            } else if ((a->avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR || (a->avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR
                       || (a->avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR || (a->swscale_version() >> 16) != LIBSWSCALE_VERSION_MAJOR) {
                loadError = L"The LAV Filters FFmpeg libraries have an unexpected version";
                ok = false;
            }
            if (!ok) {
                error = loadError;
                return nullptr;
            }
            api = std::move(a);
            return api.get();
        }

        std::string ToUtf8(const std::wstring& s)
        {
            if (s.empty()) {
                return {};
            }
            const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
            std::string out(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
            return out;
        }
    }

    struct VideoSampler::Impl {
        std::wstring libDir;
        const Api* api = nullptr;
        AVFormatContext* fmt = nullptr;
        AVCodecContext* dec = nullptr;
        SwsContext* sws = nullptr;
        int stream = -1;
        int64_t startOffsetMs = 0;
        const std::atomic<bool>* cancel = nullptr;

        static int Interrupt(void* opaque)
        {
            const auto* self = static_cast<Impl*>(opaque);
            return self->cancel && *self->cancel ? 1 : 0;
        }

        bool Convert(const AVFrame* frame, int maxWidth, ImageBGR& out)
        {
            const int w = frame->width, h = frame->height;
            if (w <= 0 || h <= 0) {
                return false;
            }
            double sar = 1.0;
            if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
                sar = (double)frame->sample_aspect_ratio.num / frame->sample_aspect_ratio.den;
            }
            int dw = std::max(2, (int)std::lround(w * sar));
            int dh = h;
            if (maxWidth > 0 && dw > maxWidth) {
                dh = std::max(2, (int)std::lround((double)dh * maxWidth / dw));
                dw = maxWidth;
            }
            dw &= ~1;
            dh &= ~1;
            sws = api->sws_getCachedContext(sws, w, h, (AVPixelFormat)frame->format, dw, dh, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) {
                return false;
            }
            out.width = dw;
            out.height = dh;
            out.data.resize(size_t(dw) * dh * 3);
            uint8_t* dst[4] = { out.data.data(), nullptr, nullptr, nullptr };
            int dstStride[4] = { dw * 3, 0, 0, 0 };
            return api->sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dstStride) > 0;
        }
    };

    VideoSampler::VideoSampler(const std::wstring& libDir)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->libDir = libDir;
    }

    VideoSampler::~VideoSampler()
    {
        Close();
    }

    bool VideoSampler::IsOpen() const
    {
        return m_impl->dec != nullptr;
    }

    void VideoSampler::Close()
    {
        Impl& d = *m_impl;
        if (!d.api) {
            return;
        }
        if (d.sws) {
            d.api->sws_freeContext(d.sws);
            d.sws = nullptr;
        }
        if (d.dec) {
            d.api->avcodec_free_context(&d.dec);
        }
        if (d.fmt) {
            d.api->avformat_close_input(&d.fmt);
        }
        d.stream = -1;
    }

    bool VideoSampler::Open(const std::wstring& path, std::wstring& error)
    {
        Close();
        Impl& d = *m_impl;
        d.api = LoadApi(d.libDir, error);
        if (!d.api) {
            return false;
        }
        const Api& a = *d.api;

        d.fmt = a.avformat_alloc_context();
        if (!d.fmt) {
            error = L"Out of memory";
            return false;
        }
        d.fmt->interrupt_callback.callback = &Impl::Interrupt;
        d.fmt->interrupt_callback.opaque = &d;
        if (a.avformat_open_input(&d.fmt, ToUtf8(path).c_str(), nullptr, nullptr) < 0) {
            d.fmt = nullptr; // freed on failure
            error = L"The video file couldn't be opened for scene analysis";
            return false;
        }
        if (a.avformat_find_stream_info(d.fmt, nullptr) < 0) {
            error = L"The video stream couldn't be read";
            Close();
            return false;
        }

        // the main video stream: skip cover art, prefer the default track, then the largest picture
        int best = -1;
        int64_t bestRank = -1;
        for (unsigned i = 0; i < d.fmt->nb_streams; i++) {
            const AVStream* st = d.fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || (st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                continue;
            }
            const int64_t rank = int64_t(st->codecpar->width) * st->codecpar->height + ((st->disposition & AV_DISPOSITION_DEFAULT) ? (1LL << 40) : 0);
            if (rank > bestRank) {
                bestRank = rank;
                best = (int)i;
            }
        }
        if (best < 0) {
            error = L"No video stream";
            Close();
            return false;
        }
        const AVStream* st = d.fmt->streams[best];
        const AVCodec* codec = a.avcodec_find_decoder(st->codecpar->codec_id);
        d.dec = codec ? a.avcodec_alloc_context3(codec) : nullptr;
        if (!d.dec || a.avcodec_parameters_to_context(d.dec, st->codecpar) < 0) {
            error = L"Unsupported video codec";
            Close();
            return false;
        }
        d.dec->pkt_timebase = st->time_base;
        d.dec->thread_count = (int)std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        d.dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        // only a couple of frames per second are looked at, frames nobody references can be dropped
        d.dec->skip_frame = AVDISCARD_NONREF;
        if (a.avcodec_open2(d.dec, codec, nullptr) < 0) {
            error = L"The video decoder couldn't be opened";
            Close();
            return false;
        }
        d.stream = best;
        // LAV Splitter reports positions relative to the start of the file
        d.startOffsetMs = d.fmt->start_time != AV_NOPTS_VALUE ? d.fmt->start_time / 1000 : 0;
        return true;
    }

    int64_t VideoSampler::GetDurationMs() const
    {
        const Impl& d = *m_impl;
        return d.fmt && d.fmt->duration != AV_NOPTS_VALUE ? d.fmt->duration / 1000 : -1;
    }

    bool VideoSampler::Sample(int64_t startMs, int64_t endMs, int64_t stepMs, int maxWidth,
                              const std::function<bool(int64_t, ImageBGR&)>& onFrame, const std::atomic<bool>& cancel)
    {
        Impl& d = *m_impl;
        if (!IsOpen() || endMs < startMs) {
            return false;
        }
        const Api& a = *d.api;
        d.cancel = &cancel;
        startMs = std::max<int64_t>(0, startMs);
        stepMs = std::max<int64_t>(40, stepMs);

        const int64_t seekUs = (startMs + d.startOffsetMs) * 1000;
        if (a.avformat_seek_file(d.fmt, -1, INT64_MIN, seekUs, seekUs, 0) < 0) {
            a.av_seek_frame(d.fmt, -1, seekUs, AVSEEK_FLAG_BACKWARD);
        }
        a.avcodec_flush_buffers(d.dec);

        AVPacket* pkt = a.av_packet_alloc();
        AVFrame* frame = a.av_frame_alloc();
        if (!pkt || !frame) {
            a.av_packet_free(&pkt);
            a.av_frame_free(&frame);
            d.cancel = nullptr;
            return false;
        }
        const AVRational tb = d.fmt->streams[d.stream]->time_base;
        const AVRational msBase{ 1, 1000 };
        int64_t next = startMs;
        int64_t lastMs = INT64_MIN;
        int delivered = 0;
        bool stop = false;

        // returns false when sampling is finished
        auto drain = [&]() {
            for (;;) {
                const int r = a.avcodec_receive_frame(d.dec, frame);
                if (r < 0) {
                    return r == AVERROR(EAGAIN);
                }
                int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                int64_t ms = ts != AV_NOPTS_VALUE ? a.av_rescale_q(ts, tb, msBase) - d.startOffsetMs
                             : (lastMs == INT64_MIN ? startMs : lastMs + 40);
                lastMs = ms;
                bool more = ms <= endMs && !cancel;
                if (more && ms >= next) {
                    ImageBGR img;
                    if (d.Convert(frame, maxWidth, img)) {
                        delivered++;
                        more = onFrame(ms, img);
                    }
                    while (next <= ms) {
                        next += stepMs;
                    }
                }
                a.av_frame_unref(frame);
                if (!more) {
                    return false;
                }
            }
        };

        while (!stop && !cancel) {
            const int r = a.av_read_frame(d.fmt, pkt);
            if (r < 0) {
                // end of file: flush the frames still buffered in the decoder
                a.avcodec_send_packet(d.dec, nullptr);
                drain();
                break;
            }
            if (pkt->stream_index == d.stream && a.avcodec_send_packet(d.dec, pkt) >= 0) {
                stop = !drain();
            }
            a.av_packet_unref(pkt);
        }
        a.av_packet_free(&pkt);
        a.av_frame_free(&frame);
        d.cancel = nullptr;
        return delivered > 0;
    }
}
