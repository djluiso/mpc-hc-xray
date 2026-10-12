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

#include "XRayFaceEngine.h"
#include "onnxruntime/include/onnxruntime_c_api.h"
#include "stb/stb_image.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

namespace XRay
{
    namespace
    {
        constexpr int kAlignedSize = 112;

        // Reference landmark positions of the 112x112 ArcFace/SFace crop
        constexpr float kAlignTemplate[10] = {
            38.2946f, 51.6963f,
            73.5318f, 51.5014f,
            56.0252f, 71.7366f,
            41.5493f, 92.3655f,
            70.7299f, 92.2041f
        };

        std::wstring Utf8ToWide(const char* s)
        {
            if (!s || !*s) {
                return {};
            }
            int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
            std::wstring w(n > 0 ? n - 1 : 0, L'\0');
            if (n > 1) {
                MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
            }
            return w;
        }

        inline float SampleBilinear(const ImageBGR& img, float x, float y, int c)
        {
            if (x < 0 || y < 0 || x > img.width - 1 || y > img.height - 1) {
                return 0.0f;
            }
            int x0 = (int)x, y0 = (int)y;
            int x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
            float fx = x - x0, fy = y - y0;
            const uint8_t* d = img.data.data();
            const size_t stride = size_t(img.width) * 3;
            float p00 = d[y0 * stride + x0 * 3 + c], p01 = d[y0 * stride + x1 * 3 + c];
            float p10 = d[y1 * stride + x0 * 3 + c], p11 = d[y1 * stride + x1 * 3 + c];
            return (p00 * (1 - fx) + p01 * fx) * (1 - fy) + (p10 * (1 - fx) + p11 * fx) * fy;
        }

        // Average of the source pixels covered by a destination pixel, used when shrinking a lot
        inline float SampleBox(const ImageBGR& img, float x0, float y0, float x1, float y1, int c)
        {
            int ix0 = std::max(0, (int)x0), iy0 = std::max(0, (int)y0);
            int ix1 = std::min(img.width, std::max(ix0 + 1, (int)std::ceil(x1)));
            int iy1 = std::min(img.height, std::max(iy0 + 1, (int)std::ceil(y1)));
            const uint8_t* d = img.data.data();
            const size_t stride = size_t(img.width) * 3;
            uint32_t sum = 0, cnt = 0;
            for (int y = iy0; y < iy1; y++) {
                const uint8_t* row = d + y * stride + c;
                for (int x = ix0; x < ix1; x++) {
                    sum += row[x * 3];
                    cnt++;
                }
            }
            return cnt ? float(sum) / cnt : 0.0f;
        }

        float IoU(const FaceDetection& a, const FaceDetection& b)
        {
            float ix = std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
            float iy = std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
            float inter = ix * iy;
            float uni = a.w * a.h + b.w * b.h - inter;
            return uni > 0 ? inter / uni : 0.0f;
        }

        std::vector<FaceDetection> NonMaxSuppression(std::vector<FaceDetection> faces, float iouThreshold)
        {
            std::sort(faces.begin(), faces.end(), [](const FaceDetection & a, const FaceDetection & b) {
                return a.score > b.score;
            });
            std::vector<FaceDetection> kept;
            for (const auto& f : faces) {
                bool overlap = std::any_of(kept.begin(), kept.end(), [&](const FaceDetection & k) {
                    return IoU(f, k) > iouThreshold;
                });
                if (!overlap) {
                    kept.push_back(f);
                }
            }
            return kept;
        }
    }

    bool ImageFromDIB(const uint8_t* dib, size_t size, ImageBGR& out)
    {
        if (!dib || size < sizeof(BITMAPINFOHEADER)) {
            return false;
        }
        const BITMAPINFOHEADER* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
        const int bpp = bih->biBitCount;
        if ((bpp != 24 && bpp != 32) || bih->biWidth <= 0 || bih->biHeight == 0) {
            return false;
        }
        const int w = bih->biWidth;
        const int h = std::abs(bih->biHeight);
        size_t offset = bih->biSize + bih->biClrUsed * sizeof(RGBQUAD);
        if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) {
            offset += 3 * sizeof(DWORD);
        }
        const size_t packed = size_t(w) * (bpp / 8);
        if (size < offset + packed * h) {
            return false;
        }
        size_t stride = (size - offset) / h;
        const size_t aligned = (packed + 3) & ~size_t(3);
        stride = (stride >= aligned) ? aligned : packed;

        out.width = w;
        out.height = h;
        out.data.resize(size_t(w) * h * 3);
        const bool topDown = bih->biHeight < 0;
        for (int y = 0; y < h; y++) {
            const uint8_t* src = dib + offset + stride * (topDown ? y : (h - 1 - y));
            uint8_t* dst = out.data.data() + size_t(y) * w * 3;
            if (bpp == 24) {
                memcpy(dst, src, size_t(w) * 3);
            } else {
                for (int x = 0; x < w; x++, src += 4, dst += 3) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }
        }
        return true;
    }

    bool ImageFromEncoded(const uint8_t* bytes, size_t size, ImageBGR& out)
    {
        if (!bytes || !size || size > INT_MAX) {
            return false;
        }
        int w = 0, h = 0, n = 0;
        stbi_uc* rgb = stbi_load_from_memory(bytes, (int)size, &w, &h, &n, 3);
        if (!rgb) {
            return false;
        }
        out.width = w;
        out.height = h;
        out.data.resize(size_t(w) * h * 3);
        for (size_t i = 0, cnt = size_t(w) * h; i < cnt; i++) {
            out.data[i * 3 + 0] = rgb[i * 3 + 2];
            out.data[i * 3 + 1] = rgb[i * 3 + 1];
            out.data[i * 3 + 2] = rgb[i * 3 + 0];
        }
        stbi_image_free(rgb);
        return true;
    }

    float CosineSimilarity(const FaceEmbedding& a, const FaceEmbedding& b)
    {
        float dot = 0;
        for (size_t i = 0; i < kEmbeddingSize; i++) {
            dot += a[i] * b[i];
        }
        return dot;
    }

    FaceEngine::~FaceEngine()
    {
        Unload();
    }

    bool FaceEngine::CheckStatus(OrtStatus* status)
    {
        if (!status) {
            return true;
        }
        m_lastError = Utf8ToWide(m_api->GetErrorMessage(status));
        m_api->ReleaseStatus(status);
        return false;
    }

    void FaceEngine::Unload()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_api) {
            if (m_detector) {
                m_api->ReleaseSession(m_detector);
            }
            if (m_recognizer) {
                m_api->ReleaseSession(m_recognizer);
            }
            if (m_memInfo) {
                m_api->ReleaseMemoryInfo(m_memInfo);
            }
            if (m_options) {
                m_api->ReleaseSessionOptions(m_options);
            }
            if (m_env) {
                m_api->ReleaseEnv(m_env);
            }
        }
        m_detector = m_recognizer = nullptr;
        m_memInfo = nullptr;
        m_options = nullptr;
        m_env = nullptr;
        m_api = nullptr;
        // onnxruntime.dll is intentionally kept loaded: unloading it while its thread pools wind down is unsafe
    }

    bool FaceEngine::Load(const std::wstring& runtimeDll, const std::wstring& detectorModel, const std::wstring& recognizerModel, std::wstring& error)
    {
        Unload();
        std::lock_guard<std::mutex> lock(m_lock);

        if (!m_hRuntime) {
            // LOAD_WITH_ALTERED_SEARCH_PATH lets the runtime find its own dependencies next to it
            m_hRuntime = LoadLibraryExW(runtimeDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!m_hRuntime) {
                error = L"Unable to load " + runtimeDll;
                return false;
            }
        }
        using GetApiBaseFn = const OrtApiBase * (ORT_API_CALL*)();
        auto getApiBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(m_hRuntime, "OrtGetApiBase"));
        const OrtApiBase* base = getApiBase ? getApiBase() : nullptr;
        if (!base) {
            error = L"onnxruntime.dll does not export OrtGetApiBase";
            return false;
        }
        // Only long-standing API entries are used, so older runtimes are accepted as well
        for (uint32_t v = ORT_API_VERSION; v >= 11 && !m_api; v--) {
            m_api = base->GetApi(v);
        }
        if (!m_api) {
            error = L"The installed onnxruntime.dll is too old";
            return false;
        }

        auto fail = [&]() {
            error = m_lastError;
            return false;
        };

        if (!CheckStatus(m_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "mpc-hc-xray", &m_env))) {
            return fail();
        }
        m_api->DisableTelemetryEvents(m_env);
        if (!CheckStatus(m_api->CreateSessionOptions(&m_options))
                || !CheckStatus(m_api->SetSessionGraphOptimizationLevel(m_options, ORT_ENABLE_ALL))
                || !CheckStatus(m_api->SetIntraOpNumThreads(m_options, (int)std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u)))) {
            return fail();
        }
        if (!CheckStatus(m_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_memInfo))) {
            return fail();
        }
        if (!CheckStatus(m_api->CreateSession(m_env, detectorModel.c_str(), m_options, &m_detector))) {
            m_lastError = L"Face detector: " + m_lastError;
            return fail();
        }
        if (!CheckStatus(m_api->CreateSession(m_env, recognizerModel.c_str(), m_options, &m_recognizer))) {
            m_lastError = L"Face recognizer: " + m_lastError;
            return fail();
        }

        OrtAllocator* allocator = nullptr;
        if (!CheckStatus(m_api->GetAllocatorWithDefaultOptions(&allocator))) {
            return fail();
        }
        auto getName = [&](OrtSession * s, bool input, std::string & name) {
            char* p = nullptr;
            OrtStatus* st = input ? m_api->SessionGetInputName(s, 0, allocator, &p) : m_api->SessionGetOutputName(s, 0, allocator, &p);
            if (!CheckStatus(st)) {
                return false;
            }
            name = p;
            m_api->AllocatorFree(allocator, p);
            return true;
        };
        if (!getName(m_detector, true, m_detInputName) || !getName(m_recognizer, true, m_recInputName) || !getName(m_recognizer, false, m_recOutputName)) {
            return fail();
        }

        // The published YuNet model has a fixed 640x640 input, but honor whatever the model declares
        OrtTypeInfo* typeInfo = nullptr;
        if (CheckStatus(m_api->SessionGetInputTypeInfo(m_detector, 0, &typeInfo))) {
            const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
            size_t dims = 0;
            if (CheckStatus(m_api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo)) && tensorInfo
                    && CheckStatus(m_api->GetDimensionsCount(tensorInfo, &dims)) && dims == 4) {
                int64_t shape[4] = {};
                if (CheckStatus(m_api->GetDimensions(tensorInfo, shape, 4))) {
                    if (shape[2] > 0 && shape[3] > 0) {
                        m_detH = (int)shape[2];
                        m_detW = (int)shape[3];
                    }
                }
            }
            m_api->ReleaseTypeInfo(typeInfo);
        }
        if (m_detW % 32 || m_detH % 32) {
            error = L"Unexpected face detector input size";
            return false;
        }
        return true;
    }

    bool FaceEngine::RunDetector(const ImageBGR& img, int rx, int ry, int rw, int rh, float scoreThreshold, std::vector<FaceDetection>& out)
    {
        const float scale = std::min(float(m_detW) / rw, float(m_detH) / rh);
        const int nw = std::max(1, std::min(m_detW, (int)std::lround(rw * scale)));
        const int nh = std::max(1, std::min(m_detH, (int)std::lround(rh * scale)));
        const float inv = 1.0f / scale;
        const bool shrinkALot = inv > 1.5f;

        // NCHW, BGR, raw 0..255 values; padded with black on the right/bottom
        std::vector<float> input(size_t(3) * m_detW * m_detH, 0.0f);
        const size_t plane = size_t(m_detW) * m_detH;
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                for (int c = 0; c < 3; c++) {
                    float v;
                    if (shrinkALot) {
                        v = SampleBox(img, rx + x * inv, ry + y * inv, rx + (x + 1) * inv, ry + (y + 1) * inv, c);
                    } else {
                        v = SampleBilinear(img, std::min(rx + (x + 0.5f) * inv - 0.5f, float(rx + rw - 1)),
                                           std::min(ry + (y + 0.5f) * inv - 0.5f, float(ry + rh - 1)), c);
                    }
                    input[c * plane + size_t(y) * m_detW + x] = v;
                }
            }
        }

        const int64_t shape[4] = { 1, 3, m_detH, m_detW };
        OrtValue* inputValue = nullptr;
        if (!CheckStatus(m_api->CreateTensorWithDataAsOrtValue(m_memInfo, input.data(), input.size() * sizeof(float), shape, 4,
                                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputValue))) {
            return false;
        }

        static const char* outputNames[12] = {
            "cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32",
            "bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32"
        };
        OrtValue* outputs[12] = {};
        const char* inputNames[1] = { m_detInputName.c_str() };
        bool ok = CheckStatus(m_api->Run(m_detector, nullptr, inputNames, &inputValue, 1, outputNames, 12, outputs));
        m_api->ReleaseValue(inputValue);

        if (ok) {
            const int strides[3] = { 8, 16, 32 };
            for (int i = 0; i < 3 && ok; i++) {
                float* cls = nullptr, *obj = nullptr, *bbox = nullptr, *kps = nullptr;
                ok = CheckStatus(m_api->GetTensorMutableData(outputs[i], (void**)&cls))
                     && CheckStatus(m_api->GetTensorMutableData(outputs[i + 3], (void**)&obj))
                     && CheckStatus(m_api->GetTensorMutableData(outputs[i + 6], (void**)&bbox))
                     && CheckStatus(m_api->GetTensorMutableData(outputs[i + 9], (void**)&kps));
                if (!ok) {
                    break;
                }
                const int stride = strides[i];
                const int cols = m_detW / stride, rows = m_detH / stride;
                for (int r = 0; r < rows; r++) {
                    for (int c = 0; c < cols; c++) {
                        const size_t idx = size_t(r) * cols + c;
                        const float clsScore = std::clamp(cls[idx], 0.0f, 1.0f);
                        const float objScore = std::clamp(obj[idx], 0.0f, 1.0f);
                        const float score = std::sqrt(clsScore * objScore);
                        if (score < scoreThreshold) {
                            continue;
                        }
                        const float cx = (c + bbox[idx * 4 + 0]) * stride;
                        const float cy = (r + bbox[idx * 4 + 1]) * stride;
                        const float w = std::exp(bbox[idx * 4 + 2]) * stride;
                        const float h = std::exp(bbox[idx * 4 + 3]) * stride;
                        // ignore detections centered in the padding area
                        if (cx >= nw || cy >= nh) {
                            continue;
                        }
                        FaceDetection f;
                        f.x = rx + (cx - w / 2) * inv;
                        f.y = ry + (cy - h / 2) * inv;
                        f.w = w * inv;
                        f.h = h * inv;
                        f.score = score;
                        for (int n = 0; n < 5; n++) {
                            f.landmarks[n * 2] = rx + (kps[idx * 10 + n * 2] + c) * stride * inv;
                            f.landmarks[n * 2 + 1] = ry + (kps[idx * 10 + n * 2 + 1] + r) * stride * inv;
                        }
                        out.push_back(f);
                    }
                }
            }
        }
        for (auto* v : outputs) {
            if (v) {
                m_api->ReleaseValue(v);
            }
        }
        return ok;
    }

    std::vector<FaceDetection> FaceEngine::Detect(const ImageBGR& img, float scoreThreshold, int minFaceSize)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        std::vector<FaceDetection> all;
        if (!IsLoaded() || img.empty()) {
            return all;
        }

        RunDetector(img, 0, 0, img.width, img.height, scoreThreshold, all);
        // Movie frames are much larger than the detector input, small faces get lost when the whole
        // frame is shrunk, so also scan two overlapping halves at a higher resolution
        if (std::max(img.width, img.height) > m_detW * 3 / 2) {
            if (img.width >= img.height) {
                const int tw = img.width * 3 / 5;
                RunDetector(img, 0, 0, tw, img.height, scoreThreshold, all);
                RunDetector(img, img.width - tw, 0, tw, img.height, scoreThreshold, all);
            } else {
                const int th = img.height * 3 / 5;
                RunDetector(img, 0, 0, img.width, th, scoreThreshold, all);
                RunDetector(img, 0, img.height - th, img.width, th, scoreThreshold, all);
            }
        }

        all.erase(std::remove_if(all.begin(), all.end(), [&](const FaceDetection & f) {
            return f.w < minFaceSize || f.h < minFaceSize;
        }), all.end());
        return NonMaxSuppression(std::move(all), 0.3f);
    }

    bool FaceEngine::Embed(const ImageBGR& img, const FaceDetection& face, FaceEmbedding& out)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!IsLoaded() || img.empty()) {
            return false;
        }

        // Least squares similarity transform (rotation + uniform scale + translation) from the
        // detected landmarks to the reference template
        float msx = 0, msy = 0, mdx = 0, mdy = 0;
        for (int i = 0; i < 5; i++) {
            msx += face.landmarks[i * 2];
            msy += face.landmarks[i * 2 + 1];
            mdx += kAlignTemplate[i * 2];
            mdy += kAlignTemplate[i * 2 + 1];
        }
        msx /= 5;
        msy /= 5;
        mdx /= 5;
        mdy /= 5;
        float num_a = 0, num_b = 0, den = 0;
        for (int i = 0; i < 5; i++) {
            const float sx = face.landmarks[i * 2] - msx, sy = face.landmarks[i * 2 + 1] - msy;
            const float dx = kAlignTemplate[i * 2] - mdx, dy = kAlignTemplate[i * 2 + 1] - mdy;
            num_a += sx * dx + sy * dy;
            num_b += sx * dy - sy * dx;
            den += sx * sx + sy * sy;
        }
        if (den <= 1e-6f) {
            return false;
        }
        const float a = num_a / den, b = num_b / den;
        const float tx = mdx - (a * msx - b * msy);
        const float ty = mdy - (b * msx + a * msy);
        const float det = a * a + b * b;
        if (det <= 1e-12f) {
            return false;
        }

        // NCHW, RGB, raw 0..255 values
        std::vector<float> input(size_t(3) * kAlignedSize * kAlignedSize);
        const size_t plane = size_t(kAlignedSize) * kAlignedSize;
        for (int v = 0; v < kAlignedSize; v++) {
            for (int u = 0; u < kAlignedSize; u++) {
                const float px = u - tx, py = v - ty;
                const float sx = (a * px + b * py) / det;
                const float sy = (-b * px + a * py) / det;
                const size_t o = size_t(v) * kAlignedSize + u;
                input[0 * plane + o] = SampleBilinear(img, sx, sy, 2);
                input[1 * plane + o] = SampleBilinear(img, sx, sy, 1);
                input[2 * plane + o] = SampleBilinear(img, sx, sy, 0);
            }
        }

        const int64_t shape[4] = { 1, 3, kAlignedSize, kAlignedSize };
        OrtValue* inputValue = nullptr;
        if (!CheckStatus(m_api->CreateTensorWithDataAsOrtValue(m_memInfo, input.data(), input.size() * sizeof(float), shape, 4,
                                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputValue))) {
            return false;
        }
        const char* inputNames[1] = { m_recInputName.c_str() };
        const char* outputNames[1] = { m_recOutputName.c_str() };
        OrtValue* output = nullptr;
        bool ok = CheckStatus(m_api->Run(m_recognizer, nullptr, inputNames, &inputValue, 1, outputNames, 1, &output));
        m_api->ReleaseValue(inputValue);
        if (ok) {
            float* feat = nullptr;
            ok = CheckStatus(m_api->GetTensorMutableData(output, (void**)&feat)) && feat;
            if (ok) {
                double norm = 0;
                for (size_t i = 0; i < kEmbeddingSize; i++) {
                    norm += double(feat[i]) * feat[i];
                }
                norm = std::sqrt(norm);
                ok = norm > 1e-6;
                for (size_t i = 0; ok && i < kEmbeddingSize; i++) {
                    out[i] = float(feat[i] / norm);
                }
            }
        }
        if (output) {
            m_api->ReleaseValue(output);
        }
        return ok;
    }
}
