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

// Offline face detection/recognition for the X-Ray feature.
// Uses two open models from the OpenCV Model Zoo executed by ONNX Runtime:
//  - YuNet (face_detection_yunet_2023mar.onnx, MIT) for detection + 5 landmarks
//  - SFace (face_recognition_sface_2021dec.onnx, Apache 2.0) for 128-d identity embeddings
// onnxruntime.dll is loaded dynamically, nothing is linked at build time.
// This file deliberately has no MFC dependency.

#include <windows.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtMemoryInfo;
struct OrtStatus;

namespace XRay
{
    struct ImageBGR {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> data; // top-down, tightly packed 8-bit BGR

        bool empty() const {
            return width <= 0 || height <= 0 || data.size() < size_t(width) * height * 3;
        }
    };

    // Converts a packed DIB (BITMAPINFOHEADER followed by pixels, 24 or 32 bpp) as returned by the renderers
    bool ImageFromDIB(const uint8_t* dib, size_t size, ImageBGR& out);
    // Decodes JPEG/PNG/BMP/GIF data
    bool ImageFromEncoded(const uint8_t* bytes, size_t size, ImageBGR& out);

    struct FaceDetection {
        float x = 0, y = 0, w = 0, h = 0;
        float score = 0;
        // right eye, left eye, nose tip, right mouth corner, left mouth corner (x, y pairs)
        std::array<float, 10> landmarks{};
    };

    constexpr size_t kEmbeddingSize = 128;
    using FaceEmbedding = std::array<float, kEmbeddingSize>;

    // Embeddings are L2 normalized, so this is a plain dot product
    float CosineSimilarity(const FaceEmbedding& a, const FaceEmbedding& b);

    class FaceEngine
    {
    public:
        FaceEngine() = default;
        ~FaceEngine();
        FaceEngine(const FaceEngine&) = delete;
        FaceEngine& operator=(const FaceEngine&) = delete;

        bool Load(const std::wstring& runtimeDll, const std::wstring& detectorModel, const std::wstring& recognizerModel, std::wstring& error);
        bool IsLoaded() const { return m_detector && m_recognizer; }
        void Unload();

        // Detects faces anywhere in the image. Results are in image coordinates, sorted by score.
        std::vector<FaceDetection> Detect(const ImageBGR& img, float scoreThreshold = 0.6f, int minFaceSize = 24);
        // Aligns the face using its landmarks and computes its identity embedding
        bool Embed(const ImageBGR& img, const FaceDetection& face, FaceEmbedding& out);

    private:
        bool RunDetector(const ImageBGR& img, int rx, int ry, int rw, int rh, float scoreThreshold, std::vector<FaceDetection>& out);
        bool CheckStatus(OrtStatus* status);

        std::mutex m_lock;
        HMODULE m_hRuntime = nullptr;
        const OrtApi* m_api = nullptr;
        OrtEnv* m_env = nullptr;
        OrtSessionOptions* m_options = nullptr;
        OrtSession* m_detector = nullptr;
        OrtSession* m_recognizer = nullptr;
        OrtMemoryInfo* m_memInfo = nullptr;
        int m_detW = 640;
        int m_detH = 640;
        std::string m_detInputName;
        std::string m_recInputName;
        std::string m_recOutputName;
        std::wstring m_lastError;
    };
}
