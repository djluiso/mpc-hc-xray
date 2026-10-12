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

#include "XRayService.h"
#include "XRayThread.h"
#include <wininet.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include "rapidjson/include/rapidjson/document.h"
#include "rapidjson/include/rapidjson/stringbuffer.h"
#include "rapidjson/include/rapidjson/writer.h"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace XRay
{
    namespace
    {
        constexpr wchar_t kUserAgent[] = L"MPC-HC X-Ray/1.0 (https://github.com/clsid2/mpc-hc)";
        constexpr size_t kMaxCast = 40;
        constexpr size_t kMaxKnownFor = 8;
        constexpr size_t kMaxLearnedPerPerson = 12;
        constexpr int kCacheVersion = 1;

        // Recognition thresholds for SFace cosine similarity, calibrated on group photos of several casts
        // (different people still reach ~0.41 against a single headshot, so the best candidate also has to
        // stand out from the second best). A wrong name is worse than a missing one.
        constexpr float kMatchScore = 0.42f;
        constexpr float kMatchMargin = 0.05f;
        constexpr float kWeakMatchScore = 0.36f;
        constexpr float kWeakMatchMargin = 0.13f;
        constexpr float kLearnScore = 0.50f;

        // Scene analysis: the paused frame alone is often a profile, a back or a blurry face, while the same
        // people are clearly visible a few seconds earlier from another camera angle. Frames around the paused
        // position are sampled, the faces are grouped per person across frames and each group is named once.
        constexpr int64_t kSceneBeforeMs = 20000;
        constexpr int64_t kSceneAfterMs = 6000;
        constexpr int64_t kNearMs = 6000;           // [pos - kNearMs, pos + kSceneAfterMs] is sampled first, densely
        constexpr int64_t kNearStepMs = 500;
        constexpr int64_t kFarStepMs = 800;
        constexpr int kSampleWidth = 1280;
        constexpr ULONGLONG kSceneBudgetMs = 45000;
        constexpr int64_t kOnScreenMs = 1500;       // "on screen now": same shot and at most this far from the pause
        constexpr int64_t kMaxShotGapMs = 1500;     // samples further apart can't be linked as one shot
        constexpr float kCutDistance = 0.62f;       // color histogram distance between samples that means a cut
        constexpr float kSameFaceScore = 0.48f;     // two faces of the same scene above this are the same person
        constexpr float kTrackFaceScore = 0.25f;    // ...or above this when they are at the same place in the same shot
        constexpr float kTrackIoU = 0.30f;
        constexpr float kVoteScore = 0.33f;         // a group whose faces mostly agree on someone needs less
        constexpr float kVoteMargin = 0.04f;
        constexpr float kVoteShare = 0.6f;

        struct EngineFile {
            const wchar_t* name;
            const char* url;
            const char* sha256;
            int sizeMB;
        };

#ifdef _WIN64
        constexpr char kRuntimeZipUrl[] = "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip";
        constexpr char kRuntimeZipSha256[] = "78D447051E48BD2E1E778BBA378BEC4ECE11191C9E538CF7B2C4A4565E8F5581";
        constexpr wchar_t kRuntimeZipEntry[] = L"onnxruntime-win-x64-1.20.1/lib/onnxruntime.dll";
        constexpr wchar_t kRuntimeZipFolder[] = L"onnxruntime-win-x64-1.20.1";
        constexpr char kRuntimeDllSha256[] = "4CB41E89B8BF30578E1DD95E9C40292D61974A4BFCD666409302C4F0C5AA8CE0";
#else
        constexpr char kRuntimeZipUrl[] = "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x86-1.20.1.zip";
        constexpr char kRuntimeZipSha256[] = "E4364D3B4A56847B87141529019F2FAA04699BF732E075A85F112E9C049309CF";
        constexpr wchar_t kRuntimeZipEntry[] = L"onnxruntime-win-x86-1.20.1/lib/onnxruntime.dll";
        constexpr wchar_t kRuntimeZipFolder[] = L"onnxruntime-win-x86-1.20.1";
        constexpr char kRuntimeDllSha256[] = "8D2D4AAB482F11EFBF80B014D9428F90DEF03A4D417A75E8B2487B23AF670D49";
#endif
        constexpr int kRuntimeZipSizeMB = 65;
        constexpr wchar_t kRuntimeDll[] = L"onnxruntime.dll";

        const EngineFile kDetectorModel = {
            L"face_detection_yunet_2023mar.onnx",
            "https://github.com/opencv/opencv_zoo/raw/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet/face_detection_yunet_2023mar.onnx",
            "8F2383E4DD3CFBB4553EA8718107FC0423210DC964F9F4280604804ED2552FA4", 1
        };
        const EngineFile kRecognizerModel = {
            L"face_recognition_sface_2021dec.onnx",
            "https://github.com/opencv/opencv_zoo/raw/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_recognition_sface/face_recognition_sface_2021dec.onnx",
            "0BA9FBFA01B5270C96627C4EF784DA859931E02F04419C829E83484087C34E79", 37
        };

        std::wstring ToWide(const std::string& s)
        {
            if (s.empty()) {
                return {};
            }
            int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
            return w;
        }

        std::string ToUtf8(const std::wstring& w)
        {
            if (w.empty()) {
                return {};
            }
            int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
            return s;
        }

        std::string UrlEncode(const std::string& s)
        {
            static const char hex[] = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c : s) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    out += char(c);
                } else {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
            }
            return out;
        }

        std::wstring ToLower(std::wstring s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
                return (wchar_t)std::towlower(c);
            });
            return s;
        }

        std::wstring Trim(const std::wstring& s, const wchar_t* chars = L" \t-_.,:;")
        {
            size_t b = s.find_first_not_of(chars);
            if (b == std::wstring::npos) {
                return {};
            }
            size_t e = s.find_last_not_of(chars);
            return s.substr(b, e - b + 1);
        }

        std::string SafeName(const std::string& id)
        {
            std::string out;
            for (char c : id) {
                out += (isalnum((unsigned char)c) || c == '-') ? c : '_';
            }
            return out.substr(0, 120);
        }

        bool ReadAll(const std::wstring& path, std::string& data)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                return false;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            data = ss.str();
            return true;
        }

        bool WriteAll(const std::wstring& path, const void* data, size_t size)
        {
            std::error_code ec;
            fs::create_directories(fs::path(path).parent_path(), ec);
            const std::wstring tmp = path + L".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    return false;
                }
                f.write(static_cast<const char*>(data), size);
                if (!f) {
                    return false;
                }
            }
            return !!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        bool WriteAll(const std::wstring& path, const std::string& data)
        {
            return WriteAll(path, data.data(), data.size());
        }

        std::string Sha256File(const std::wstring& path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                return {};
            }
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            std::string result;
            if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
                if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
                    std::vector<char> buf(1 << 20);
                    bool ok = true;
                    while (ok && f) {
                        f.read(buf.data(), buf.size());
                        if (f.gcount() > 0) {
                            ok = BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)buf.data(), (ULONG)f.gcount(), 0));
                        }
                    }
                    UCHAR digest[32];
                    if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
                        static const char hex[] = "0123456789ABCDEF";
                        for (UCHAR b : digest) {
                            result += hex[b >> 4];
                            result += hex[b & 15];
                        }
                    }
                    BCryptDestroyHash(hash);
                }
                BCryptCloseAlgorithmProvider(alg, 0);
            }
            return result;
        }

        bool FileExists(const std::wstring& path)
        {
            DWORD attr = GetFileAttributesW(path.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        }

        // --- JSON helpers ---

        std::string JStr(const rapidjson::Value& v, const char* name)
        {
            if (v.IsObject()) {
                auto it = v.FindMember(name);
                if (it != v.MemberEnd() && it->value.IsString()) {
                    return std::string(it->value.GetString(), it->value.GetStringLength());
                }
            }
            return {};
        }

        std::wstring JWStr(const rapidjson::Value& v, const char* name)
        {
            return ToWide(JStr(v, name));
        }

        double JNum(const rapidjson::Value& v, const char* name, double def = 0)
        {
            if (v.IsObject()) {
                auto it = v.FindMember(name);
                if (it != v.MemberEnd() && it->value.IsNumber()) {
                    return it->value.GetDouble();
                }
            }
            return def;
        }

        bool JBool(const rapidjson::Value& v, const char* name)
        {
            if (v.IsObject()) {
                auto it = v.FindMember(name);
                if (it != v.MemberEnd() && it->value.IsBool()) {
                    return it->value.GetBool();
                }
            }
            return false;
        }

        const rapidjson::Value* JArr(const rapidjson::Value& v, const char* name)
        {
            if (v.IsObject()) {
                auto it = v.FindMember(name);
                if (it != v.MemberEnd() && it->value.IsArray()) {
                    return &it->value;
                }
            }
            return nullptr;
        }

        // Coarse 4x4x4 color histogram, compared between consecutive samples to find the camera cuts
        using Signature = std::array<float, 64>;

        Signature ColorSignature(const ImageBGR& img)
        {
            Signature s{};
            if (img.empty()) {
                return s;
            }
            const int step = std::max(1, std::min(img.width, img.height) / 90);
            float total = 0;
            for (int y = 0; y < img.height; y += step) {
                const uint8_t* row = img.data.data() + size_t(y) * img.width * 3;
                for (int x = 0; x < img.width; x += step) {
                    const uint8_t* p = row + x * 3;
                    s[(p[2] >> 6) * 16 + (p[1] >> 6) * 4 + (p[0] >> 6)] += 1;
                    total += 1;
                }
            }
            for (auto& v : s) {
                v /= std::max(1.0f, total);
            }
            return s;
        }

        float SignatureDistance(const Signature& a, const Signature& b)
        {
            float d = 0;
            for (size_t i = 0; i < a.size(); i++) {
                d += std::fabs(a[i] - b[i]);
            }
            return d;
        }

        float IoU(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
        {
            const float ix = std::max(0.0f, std::min(ax + aw, bx + bw) - std::max(ax, bx));
            const float iy = std::max(0.0f, std::min(ay + ah, by + bh) - std::max(ay, by));
            const float inter = ix * iy;
            const float uni = aw * ah + bw * bh - inter;
            return uni > 0 ? inter / uni : 0;
        }

        // Identifies a given file (the same movie can be cut differently in another release)
        std::string FileIdentity(const std::wstring& pathOrName)
        {
            std::wstring name = pathOrName;
            const size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                name = name.substr(slash + 1);
            }
            uint64_t size = 0;
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(pathOrName.c_str(), GetFileExInfoStandard, &fad)) {
                size = (uint64_t(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            }
            std::string data = ToUtf8(ToLower(name));
            data.append((const char*)&size, sizeof(size));
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : data) {
                h = (h ^ c) * 1099511628211ULL;
            }
            char hex[17];
            sprintf_s(hex, "%016llx", (unsigned long long)h);
            return hex;
        }

        // changes when actors are added to or removed from a cast, the scene timeline must be redone then
        std::string CastIdentity(const std::vector<Person>& cast)
        {
            std::vector<std::string> ids;
            for (const auto& p : cast) {
                ids.push_back(p.id);
            }
            std::sort(ids.begin(), ids.end());
            uint32_t h = 2166136261u;
            for (const auto& id : ids) {
                for (unsigned char c : id + "|") {
                    h = (h ^ c) * 16777619u;
                }
            }
            char hex[9];
            sprintf_s(hex, "%08x", h);
            return hex;
        }

        // Wikidata SPARQL json: results.bindings[i].<var>.value
        std::string SparqlValue(const rapidjson::Value& binding, const char* var)
        {
            if (binding.IsObject()) {
                auto it = binding.FindMember(var);
                if (it != binding.MemberEnd()) {
                    return JStr(it->value, "value");
                }
            }
            return {};
        }

        std::string WikidataId(const std::string& entityUrl)
        {
            size_t p = entityUrl.rfind('/');
            return p == std::string::npos ? entityUrl : entityUrl.substr(p + 1);
        }

        // the label service returns the bare id ("Q123") when an entity has no English label
        bool IsWikidataId(const std::string& s)
        {
            return s.size() > 1 && s[0] == 'Q' && std::all_of(s.begin() + 1, s.end(), [](char c) {
                return c >= '0' && c <= '9';
            });
        }

        int YearOf(const std::string& date)
        {
            return date.size() >= 4 ? atoi(date.substr(0, 4).c_str()) : 0;
        }

        using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

        void WStr(JsonWriter& w, const char* key, const std::wstring& value)
        {
            w.Key(key);
            const std::string s = ToUtf8(value);
            w.String(s.c_str(), (rapidjson::SizeType)s.size());
        }

        void SStr(JsonWriter& w, const char* key, const std::string& value)
        {
            w.Key(key);
            w.String(value.c_str(), (rapidjson::SizeType)value.size());
        }

        void WritePersonFields(JsonWriter& w, const Person& p)
        {
            SStr(w, "id", p.id);
            WStr(w, "name", p.name);
            WStr(w, "character", p.character);
            SStr(w, "image", p.imageUrl);
            WStr(w, "birthday", p.birthday);
            WStr(w, "deathday", p.deathday);
            WStr(w, "birthplace", p.birthplace);
            w.Key("order");
            w.Int(p.order);
        }

        void ReadPersonFields(const rapidjson::Value& v, Person& p)
        {
            p.id = JStr(v, "id");
            p.name = JWStr(v, "name");
            p.character = JWStr(v, "character");
            p.imageUrl = JStr(v, "image");
            p.birthday = JWStr(v, "birthday");
            p.deathday = JWStr(v, "deathday");
            p.birthplace = JWStr(v, "birthplace");
            p.order = (int)JNum(v, "order");
        }

        std::string WikidataImageUrl(const std::string& commonsUrl)
        {
            if (commonsUrl.empty()) {
                return {};
            }
            std::string url = commonsUrl;
            if (url.compare(0, 7, "http://") == 0) {
                url = "https://" + url.substr(7);
            }
            // Special:FilePath redirects to a scaled thumbnail
            return url + (url.find('?') == std::string::npos ? "?width=320" : "&width=320");
        }

        // comparable form of a person name: lower case letters and digits only
        std::wstring NameKey(const std::wstring& name)
        {
            std::wstring out;
            for (wchar_t c : name) {
                if (std::iswalnum(c)) {
                    out += (wchar_t)std::towlower(c);
                }
            }
            return out;
        }

        // adds a role to a "Role A / Role B" list, ignoring duplicates
        void AddCharacter(std::wstring& list, const std::wstring& role)
        {
            const std::wstring r = Trim(role, L" \t");
            if (r.empty()) {
                return;
            }
            const std::wstring key = NameKey(r);
            size_t start = 0;
            while (start <= list.size() && !list.empty()) {
                size_t end = list.find(L" / ", start);
                if (NameKey(list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start)) == key) {
                    return;
                }
                if (end == std::wstring::npos) {
                    break;
                }
                start = end + 3;
            }
            list += (list.empty() ? L"" : L" / ") + r;
        }

        struct WikiCastEntry {
            std::wstring actor;     // displayed name
            std::wstring link;      // linked article of the actor, may be empty
            std::wstring character;
        };

        // replaces the innermost templates until none is left, keeping the text of the few that carry a name
        std::wstring ExpandWikiTemplates(std::wstring s)
        {
            static const std::wregex inner(L"\\{\\{([^{}]*)\\}\\}");
            for (int guard = 0; guard < 20; guard++) {
                std::wsmatch m;
                if (!std::regex_search(s, m, inner)) {
                    break;
                }
                std::vector<std::wstring> parts;
                std::wstringstream ss(m[1].str());
                for (std::wstring part; std::getline(ss, part, L'|');) {
                    parts.push_back(Trim(part, L" \t"));
                }
                const std::wstring name = parts.empty() ? L"" : ToLower(parts[0]);
                std::wstring repl;
                auto positional = [&](size_t i) {
                    // skip named parameters ("lt=...")
                    for (size_t k = 1, n = 0; k < parts.size(); k++) {
                        if (parts[k].find(L'=') == std::wstring::npos && n++ == i - 1) {
                            return parts[k];
                        }
                    }
                    return std::wstring();
                };
                if (name == L"sortname" || name == L"sort name") {
                    repl = positional(1) + L" " + positional(2);
                } else if (name == L"ill" || name == L"interlanguage link" || name == L"nowrap" || name == L"small" || name == L"nobold") {
                    repl = positional(1);
                    if (name == L"ill" || name == L"interlanguage link") {
                        repl = L"[[" + repl + L"]]";
                    }
                }
                s.replace(m.position(0), m.length(0), repl);
            }
            return s;
        }

        // The "Cast" section of an English Wikipedia article: "* [[Pedro Pascal]] as [[Din Djarin]], a bounty hunter"
        std::vector<WikiCastEntry> ParseWikipediaCast(const std::wstring& wikitext)
        {
            static const std::wregex heading(L"^\\s*(=+)\\s*(.*?)\\s*=+\\s*$");
            static const std::wregex refs(L"<ref[^>]*/>|<ref[^>]*>.*?</ref>|<ref[^>]*>.*$|<!--.*?-->");
            static const std::wregex tags(L"<[^>]+>");
            static const std::wregex link(L"\\[\\[([^\\]|]*)(?:\\|([^\\]]*))?\\]\\]");
            std::vector<WikiCastEntry> out;
            std::wistringstream in(wikitext);
            int castLevel = 0;
            for (std::wstring line; std::getline(in, line);) {
                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }
                std::wsmatch m;
                if (std::regex_match(line, m, heading)) {
                    const int level = (int)m[1].length();
                    const std::wstring title = ToLower(m[2].str());
                    if (castLevel && level <= castLevel) {
                        castLevel = 0;
                    }
                    if (!castLevel && ((title.compare(0, 4, L"cast") == 0 && title.compare(0, 7, L"casting") != 0) || title == L"starring"
                                       || title == L"characters" || title.find(L"voice cast") != std::wstring::npos)) {
                        castLevel = level;
                    }
                    continue;
                }
                // "**" lines are notes such as stand-ins and doubles
                if (!castLevel || line.empty() || line[0] != L'*' || (line.size() > 1 && line[1] == L'*')) {
                    continue;
                }
                std::wstring s = std::regex_replace(line, refs, L"");
                s = ExpandWikiTemplates(s);
                // links -> displayed text, remembering where each link landed
                std::vector<std::pair<size_t, std::wstring>> links;
                std::wstring plain;
                auto it = s.cbegin();
                for (std::wsregex_iterator li(s.begin(), s.end(), link), end; li != end; ++li) {
                    const auto& lm = *li;
                    plain.append(it, s.cbegin() + lm.position(0));
                    const std::wstring target = lm[1].str();
                    if (target.compare(0, 5, L"File:") == 0 || target.compare(0, 6, L"Image:") == 0) {
                        it = s.cbegin() + lm.position(0) + lm.length(0);
                        continue;
                    }
                    links.emplace_back(plain.size(), target);
                    plain += lm[2].matched ? lm[2].str() : target;
                    it = s.cbegin() + lm.position(0) + lm.length(0);
                }
                plain.append(it, s.cend());
                plain = std::regex_replace(plain, tags, L"");
                for (const wchar_t* q : { L"'''", L"''" }) {
                    for (size_t p; (p = plain.find(q)) != std::wstring::npos;) {
                        plain.erase(p, wcslen(q));
                    }
                }
                for (size_t p; (p = plain.find(L"&nbsp;")) != std::wstring::npos;) {
                    plain.replace(p, 6, L" ");
                }
                const size_t first = plain.find_first_not_of(L"*#: \t");
                if (first == std::wstring::npos) {
                    continue;
                }
                const size_t as = plain.find(L" as ", first);
                WikiCastEntry e;
                e.actor = Trim(plain.substr(first, as == std::wstring::npos ? std::wstring::npos : as - first), L" \t,:;");
                for (const auto& l : links) {
                    if (l.first >= first && (as == std::wstring::npos || l.first < as)) {
                        e.link = l.second;
                        break;
                    }
                }
                if (as != std::wstring::npos) {
                    std::wstring c = plain.substr(as + 4);
                    // keep the role, drop the description: "Din Djarin, a bounty hunter" / "Grogu (voice)"
                    size_t cut = std::wstring::npos;
                    for (const wchar_t* sep : { L",", L";", L":", L" (", L" \u2013 ", L" \u2014 ", L" - ", L"\u2014", L". " }) {
                        cut = std::min(cut, c.find(sep));
                    }
                    c = Trim(c.substr(0, cut), L" \t.");
                    if (c.compare(0, 4, L"the ") == 0 && c.find(L"voice of ") == 4) {
                        c = c.substr(13);
                    }
                    if (c.size() <= 60) {
                        e.character = c;
                    }
                }
                // a cast line names a person: short, no sentence
                if (e.actor.empty() || e.actor.size() > 40 || std::count(e.actor.begin(), e.actor.end(), L' ') > 4) {
                    continue;
                }
                out.push_back(std::move(e));
            }
            return out;
        }

        std::wstring MovieKey(const MediaQuery& q, const char* provider)
        {
            std::wstring key = ToLower(q.title);
            if (q.year) {
                key += L"_" + std::to_wstring(q.year);
            }
            if (q.IsEpisode()) {
                wchar_t buf[32];
                swprintf_s(buf, L"_s%02de%02d", q.season, q.episode);
                key += buf;
            }
            key += L"_";
            key += ToWide(provider);
            return key;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // File name parsing

    static MediaQuery ParseNameComponent(std::wstring name);

    MediaQuery ParseMediaName(const std::wstring& fileName)
    {
        std::wstring path = fileName;
        size_t qm = path.find(L'?');
        if (qm != std::wstring::npos && qm > 0) {
            path = path.substr(0, qm);
        }
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
            path.pop_back();
        }
        size_t slash = path.find_last_of(L"\\/");
        MediaQuery q = ParseNameComponent(slash != std::wstring::npos ? path.substr(slash + 1) : path);

        // generic names like "movie.mkv" or "title_t00.mkv": use the parent folder, keeping the episode number if any
        static const std::wregex generic(LR"(^(movie|film|video|feature|main|sample|title\s?t?\d*|vts\s\d+\s\d+|\d{5}|)$)", std::regex::icase);
        if (slash != std::wstring::npos && slash > 0 && std::regex_match(q.title, generic)) {
            std::wstring parent = path.substr(0, slash);
            for (int up = 0; up < 3; up++) {
                size_t pslash = parent.find_last_of(L"\\/");
                std::wstring leaf = pslash != std::wstring::npos ? parent.substr(pslash + 1) : parent;
                if (up < 2 && pslash != std::wstring::npos && (_wcsicmp(leaf.c_str(), L"VIDEO_TS") == 0 || _wcsicmp(leaf.c_str(), L"BDMV") == 0
                                                                || _wcsicmp(leaf.c_str(), L"STREAM") == 0)) {
                    parent = parent.substr(0, pslash);
                    continue;
                }
                parent = leaf;
                break;
            }
            if (!parent.empty() && parent.back() != L':') {
                MediaQuery pq = ParseNameComponent(parent + L".dir");
                if (!pq.title.empty()) {
                    if (!q.IsEpisode() && pq.IsEpisode()) {
                        q.season = pq.season;
                        q.episode = pq.episode;
                    }
                    q.title = pq.title;
                    q.year = q.year ? q.year : pq.year;
                }
            }
        }
        return q;
    }

    static MediaQuery ParseNameComponent(std::wstring name)
    {
        MediaQuery q;
        size_t qm = name.find(L'?');
        if (qm != std::wstring::npos && qm > 0) {
            name = name.substr(0, qm);
        }
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && name.size() - dot >= 3 && name.size() - dot <= 6) {
            const std::wstring ext = name.substr(dot + 1);
            if (std::all_of(ext.begin(), ext.end(), [](wchar_t c) { return iswalnum(c) != 0; })
                    && !std::all_of(ext.begin(), ext.end(), [](wchar_t c) { return iswdigit(c) != 0; })) {
                name = name.substr(0, dot);
            }
        }

        // release group tags and other bracketed noise, but keep a bracketed year
        std::wsmatch m;
        static const std::wregex yearInBrackets(LR"([\(\[]((?:19|20)\d{2})[\)\]])");
        if (std::regex_search(name, m, yearInBrackets)) {
            q.year = std::stoi(m[1].str());
            name = m.prefix().str() + L" " + m[1].str() + L" " + m.suffix().str();
        }
        static const std::wregex brackets(LR"(\[[^\]]*\]|\{[^\}]*\}|\([^\)]*\))");
        name = std::regex_replace(name, brackets, L" ");

        // separators
        for (auto& c : name) {
            if (c == L'.' || c == L'_' || c == L'+') {
                c = L' ';
            }
        }

        static const std::wregex episode(LR"(\b[Ss](\d{1,2})\s?[Ee](\d{1,3})\b|\b(\d{1,2})x(\d{2,3})\b)");
        if (std::regex_search(name, m, episode)) {
            if (m[1].matched) {
                q.season = std::stoi(m[1].str());
                q.episode = std::stoi(m[2].str());
            } else {
                q.season = std::stoi(m[3].str());
                q.episode = std::stoi(m[4].str());
            }
            name = m.prefix().str();
        }

        // the last plausible year that is not the very first word ("2001 A Space Odyssey 1968")
        static const std::wregex year(LR"(\b((?:19|20)\d{2})\b)");
        size_t cut = std::wstring::npos;
        for (std::wsregex_iterator it(name.begin(), name.end(), year), end; it != end; ++it) {
            const size_t pos = (size_t)it->position(0);
            if (!Trim(name.substr(0, pos)).empty()) {
                cut = pos;
                q.year = std::stoi((*it)[1].str());
            }
        }
        if (cut != std::wstring::npos) {
            name = name.substr(0, cut);
        }

        // only unambiguous technical tags, words like "web" or "cam" can be part of real titles
        static const std::wregex junk(
            LR"(\b(2160p|1080p|1080i|720p|576p|480p|4k|uhd|bluray|blu-ray|bdrip|brrip|bdremux|remux|webrip|web-dl|webdl|hdrip|dvdrip|dvdscr|hdtv|hdcam|x264|x265|h264|h265|hevc|xvid|divx|aac|ac3|eac3|dts|truehd|atmos|10bit|repack|unrated|remastered|imax)\b.*$)",
            std::regex::icase);
        name = std::regex_replace(name, junk, L"");

        static const std::wregex spaces(LR"(\s+)");
        name = std::regex_replace(name, spaces, L" ");
        q.title = Trim(name);
        return q;
    }

    // ---------------------------------------------------------------------------------------------------------------

    struct Service::Job {
        std::atomic<bool> cancel{ false };
        std::wstring progress;
        std::shared_ptr<Result> partial;
        std::shared_ptr<Result> result;
        bool done = false;
    };

    Service::Service(const std::wstring& cacheDir, const std::wstring& appDir)
        : m_cacheDir(cacheDir)
        , m_appDir(appDir)
    {
        if (!m_cacheDir.empty() && m_cacheDir.back() != L'\\') {
            m_cacheDir += L'\\';
        }
        if (!m_appDir.empty() && m_appDir.back() != L'\\') {
            m_appDir += L'\\';
        }
#ifdef _WIN64
        m_videoLibDirs.push_back(m_appDir + L"LAVFilters64\\");
#else
        m_videoLibDirs.push_back(m_appDir + L"LAVFilters\\");
#endif
        std::error_code ec;
        fs::create_directories(m_cacheDir, ec);

        HINTERNET h = InternetOpenW(kUserAgent, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (h) {
            DWORD timeout = 15000;
            InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
            InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
            InternetSetOptionW(h, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        }
        m_hInternet = h;
    }

    Service::~Service()
    {
        {
            std::unique_lock<std::mutex> lock(m_jobLock);
            if (m_job) {
                m_job->cancel = true;
            }
            // worker threads reference this object, wait for them (they honor the cancel flag quickly)
            m_jobDone.wait(lock, [this] { return m_runningJobs == 0; });
        }
        if (m_hInternet) {
            InternetCloseHandle((HINTERNET)m_hInternet);
        }
    }

    void Service::SetVideoLibraryDirs(std::vector<std::wstring> dirs)
    {
        for (auto& d : dirs) {
            if (!d.empty() && d.back() != L'\\') {
                d += L'\\';
            }
        }
        std::lock_guard<std::mutex> lock(m_processLock);
        m_videoLibDirs = std::move(dirs);
    }

    void Service::Start(Request request, HWND hNotify, UINT msg)
    {
        std::lock_guard<std::mutex> lock(m_jobLock);
        if (m_job) {
            m_job->cancel = true;
        }
        auto job = std::make_shared<Job>();
        m_job = job;
        m_runningJobs++;

        RunDetached([this, job, hNotify, msg, req = std::move(request)]() {
            std::shared_ptr<Result> result;
            {
                std::lock_guard<std::mutex> processLock(m_processLock);
                if (!job->cancel) {
                    auto progress = [&](const std::wstring & text) {
                        {
                            std::lock_guard<std::mutex> l(m_jobLock);
                            job->progress = text;
                        }
                        if (!job->cancel) {
                            PostMessageW(hNotify, msg, kNotifyProgress, 0);
                        }
                    };
                    auto partial = [&](std::shared_ptr<Result> r) {
                        {
                            std::lock_guard<std::mutex> l(m_jobLock);
                            job->partial = std::move(r);
                        }
                        if (!job->cancel) {
                            PostMessageW(hNotify, msg, kNotifyPartial, 0);
                        }
                    };
                    try {
                        result = Process(req, progress, job->cancel, partial);
                    } catch (const std::exception& e) {
                        result = std::make_shared<Result>();
                        result->status = Result::Status::Error;
                        result->message = ToWide(e.what());
                    }
                }
            }
            if (!result || job->cancel) {
                result = std::make_shared<Result>();
                result->status = Result::Status::Cancelled;
            }
            bool notify;
            {
                std::lock_guard<std::mutex> l(m_jobLock);
                job->result = result;
                job->done = true;
                notify = !job->cancel;
                m_runningJobs--;
                // notify while holding the lock: the destructor may be waiting and free us right after
                m_jobDone.notify_all();
                if (notify) {
                    PostMessageW(hNotify, msg, kNotifyDone, 0);
                }
            }
        });
    }

    void Service::Cancel()
    {
        std::lock_guard<std::mutex> lock(m_jobLock);
        if (m_job) {
            m_job->cancel = true;
            m_job.reset();
        }
    }

    bool Service::IsBusy() const
    {
        std::lock_guard<std::mutex> lock(m_jobLock);
        return m_job && !m_job->done;
    }

    std::wstring Service::GetProgress() const
    {
        std::lock_guard<std::mutex> lock(m_jobLock);
        return m_job ? m_job->progress : std::wstring();
    }

    std::shared_ptr<Result> Service::TakePartialResult()
    {
        std::lock_guard<std::mutex> lock(m_jobLock);
        if (m_job && !m_job->done && m_job->partial) {
            return std::move(m_job->partial);
        }
        return nullptr;
    }

    std::shared_ptr<Result> Service::TakeResult()
    {
        std::lock_guard<std::mutex> lock(m_jobLock);
        if (m_job && m_job->done) {
            auto r = m_job->result;
            m_job.reset();
            return r;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // HTTP

    bool Service::HttpGet(const std::string& url, const std::vector<std::wstring>& headers, std::string& body, long* status)
    {
        body.clear();
        if (status) {
            *status = 0;
        }
        if (!m_hInternet) {
            return false;
        }
        std::wstring hdr;
        for (const auto& h : headers) {
            hdr += h + L"\r\n";
        }
        HINTERNET hUrl = InternetOpenUrlW((HINTERNET)m_hInternet, ToWide(url).c_str(), hdr.empty() ? nullptr : hdr.c_str(), (DWORD)hdr.size(),
                                          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_KEEP_CONNECTION, 0);
        if (!hUrl) {
            return false;
        }
        DWORD code = 0, len = sizeof(code);
        HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
        if (status) {
            *status = (long)code;
        }
        char buf[16384];
        DWORD read = 0;
        while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0) {
            body.append(buf, read);
            if (body.size() > 64 * 1024 * 1024) {
                break;
            }
        }
        InternetCloseHandle(hUrl);
        return code >= 200 && code < 300;
    }

    bool Service::HttpDownloadFile(const std::string& url, const std::wstring& path, const std::function<bool(uint64_t, uint64_t)>& progress)
    {
        if (!m_hInternet) {
            return false;
        }
        HINTERNET hUrl = InternetOpenUrlW((HINTERNET)m_hInternet, ToWide(url).c_str(), nullptr, 0,
                                          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI, 0);
        if (!hUrl) {
            return false;
        }
        DWORD code = 0, len = sizeof(code);
        HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
        uint64_t total = 0;
        wchar_t lenBuf[32] = {};
        len = sizeof(lenBuf);
        if (HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH, lenBuf, &len, nullptr)) {
            total = _wcstoui64(lenBuf, nullptr, 10);
        }
        bool ok = code == 200;
        if (ok) {
            std::error_code ec;
            fs::create_directories(fs::path(path).parent_path(), ec);
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            ok = !!f;
            std::vector<char> buf(256 * 1024);
            uint64_t done = 0;
            DWORD read = 0;
            while (ok && InternetReadFile(hUrl, buf.data(), (DWORD)buf.size(), &read)) {
                if (read == 0) {
                    break;
                }
                f.write(buf.data(), read);
                done += read;
                ok = !!f && (!progress || progress(done, total));
            }
            ok = ok && (!total || done == total);
        }
        InternetCloseHandle(hUrl);
        if (!ok) {
            DeleteFileW(path.c_str());
        }
        return ok;
    }

    std::vector<std::wstring> Service::TmdbHeaders(const std::string& key) const
    {
        std::vector<std::wstring> h = { L"Accept: application/json" };
        if (key.size() > 40) {
            h.push_back(L"Authorization: Bearer " + ToWide(key));
        }
        return h;
    }

    std::string Service::TmdbUrl(const std::string& path, const std::string& key, const std::string& query) const
    {
        std::string url = "https://api.themoviedb.org/3/" + path + "?language=en-US";
        if (key.size() <= 40) {
            url += "&api_key=" + UrlEncode(key);
        }
        if (!query.empty()) {
            url += "&" + query;
        }
        return url;
    }

    std::wstring Service::PathFor(const wchar_t* folder, const std::string& id, const wchar_t* ext) const
    {
        return m_cacheDir + folder + L"\\" + ToWide(SafeName(id)) + ext;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Face engine installation

    Service::EnginePaths Service::FindEngine() const
    {
        EnginePaths p;
        const std::wstring engineDir = m_cacheDir + L"engine\\";
        for (const auto& dir : { m_appDir, m_appDir + L"XRay\\", engineDir }) {
            if (p.runtime.empty() && FileExists(dir + kRuntimeDll)) {
                p.runtime = dir + kRuntimeDll;
            }
            if (p.detector.empty() && FileExists(dir + kDetectorModel.name)) {
                p.detector = dir + kDetectorModel.name;
            }
            if (p.recognizer.empty() && FileExists(dir + kRecognizerModel.name)) {
                p.recognizer = dir + kRecognizerModel.name;
            }
        }
        return p;
    }

    bool Service::IsEngineInstalled() const
    {
        EnginePaths p = FindEngine();
        return !p.runtime.empty() && !p.detector.empty() && !p.recognizer.empty();
    }

    int Service::GetEngineDownloadSizeMB() const
    {
        EnginePaths p = FindEngine();
        return (p.runtime.empty() ? kRuntimeZipSizeMB : 0) + (p.detector.empty() ? kDetectorModel.sizeMB : 0) + (p.recognizer.empty() ? kRecognizerModel.sizeMB : 0);
    }

    bool Service::InstallEngine(const std::function<void(const std::wstring&)>& progress, const std::atomic<bool>& cancel, std::wstring& error)
    {
        const std::wstring engineDir = m_cacheDir + L"engine\\";
        std::error_code ec;
        fs::create_directories(engineDir, ec);
        EnginePaths have = FindEngine();

        auto download = [&](const std::string & url, const std::wstring & path, const std::string & sha256, const std::wstring & what) {
            if (FileExists(path) && Sha256File(path) == sha256) {
                return true;
            }
            const std::wstring tmp = path + L".part";
            int lastPct = -1;
            bool ok = HttpDownloadFile(url, tmp, [&](uint64_t done, uint64_t total) {
                int pct = total ? int(done * 100 / total) : 0;
                if (pct != lastPct) {
                    lastPct = pct;
                    progress(L"Downloading " + what + L"\u2026 " + std::to_wstring(pct) + L"%");
                }
                return !cancel;
            });
            if (!ok) {
                error = cancel ? L"Cancelled" : L"Download failed: " + what;
                return false;
            }
            if (Sha256File(tmp) != sha256) {
                DeleteFileW(tmp.c_str());
                error = L"Integrity check failed for " + what;
                return false;
            }
            DeleteFileW(path.c_str());
            if (!MoveFileW(tmp.c_str(), path.c_str())) {
                error = L"Unable to write " + path;
                return false;
            }
            return true;
        };

        if (have.detector.empty() && !download(kDetectorModel.url, engineDir + kDetectorModel.name, kDetectorModel.sha256, L"face detection model")) {
            return false;
        }
        if (have.recognizer.empty() && !download(kRecognizerModel.url, engineDir + kRecognizerModel.name, kRecognizerModel.sha256, L"face recognition model")) {
            return false;
        }
        if (have.runtime.empty()) {
            const std::wstring zip = engineDir + L"onnxruntime.zip";
            if (!download(kRuntimeZipUrl, zip, kRuntimeZipSha256, L"ONNX Runtime")) {
                return false;
            }
            progress(L"Installing ONNX Runtime\u2026");
            // Windows 10+ ships bsdtar which can extract zip archives
            wchar_t sysDir[MAX_PATH] = {};
            GetSystemDirectoryW(sysDir, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(sysDir) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + engineDir.substr(0, engineDir.size() - 1) + L"\" \"" + kRuntimeZipEntry + L"\"";
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            DWORD exitCode = 1;
            if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 60000);
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            std::wstring extracted = engineDir + kRuntimeZipEntry;
            std::replace(extracted.begin(), extracted.end(), L'/', L'\\');
            const std::wstring dll = engineDir + kRuntimeDll;
            bool ok = exitCode == 0 && FileExists(extracted) && Sha256File(extracted) == kRuntimeDllSha256;
            if (ok) {
                DeleteFileW(dll.c_str());
                ok = !!MoveFileW(extracted.c_str(), dll.c_str());
            }
            fs::remove_all(engineDir + kRuntimeZipFolder, ec);
            DeleteFileW(zip.c_str());
            if (!ok) {
                error = L"Unable to extract onnxruntime.dll. Extract it manually from " + ToWide(kRuntimeZipUrl) + L" into " + engineDir;
                return false;
            }
        }
        return true;
    }

    bool Service::EnsureEngine(bool allowDownload, const std::function<void(const std::wstring&)>& progress, const std::atomic<bool>& cancel, std::wstring& error)
    {
        if (m_engine.IsLoaded()) {
            return true;
        }
        if (!IsEngineInstalled()) {
            if (!allowDownload) {
                error = L"Face recognition components are not installed";
                return false;
            }
            if (!InstallEngine(progress, cancel, error)) {
                return false;
            }
            m_engineTried = false;
        }
        if (m_engineTried) {
            error = m_engineError;
            return false;
        }
        m_engineTried = true;
        progress(L"Loading face recognition\u2026");
        EnginePaths p = FindEngine();
        if (!m_engine.Load(p.runtime, p.detector, p.recognizer, m_engineError)) {
            error = m_engineError;
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Cache

    bool Service::SaveMovie(const Movie& movie)
    {
        rapidjson::StringBuffer sb;
        JsonWriter w(sb);
        w.StartObject();
        w.Key("version");
        w.Int(kCacheVersion);
        SStr(w, "provider", movie.provider);
        SStr(w, "providerId", movie.providerId);
        WStr(w, "title", movie.title);
        w.Key("year");
        w.Int(movie.year);
        w.Key("isEpisode");
        w.Bool(movie.isEpisode);
        w.Key("season");
        w.Int(movie.season);
        w.Key("episode");
        w.Int(movie.episode);
        w.Key("charactersChecked");
        w.Bool(movie.charactersChecked);
        w.Key("cast");
        w.StartArray();
        for (const auto& p : movie.cast) {
            w.StartObject();
            WritePersonFields(w, p);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        return WriteAll(PathFor(L"movies", movie.key, L".json"), std::string(sb.GetString(), sb.GetSize()));
    }

    bool Service::LoadMovie(const std::string& key, Movie& movie)
    {
        std::string data;
        if (!ReadAll(PathFor(L"movies", key, L".json"), data)) {
            return false;
        }
        rapidjson::Document d;
        d.Parse(data.c_str(), data.size());
        if (d.HasParseError() || !d.IsObject() || JNum(d, "version") != kCacheVersion) {
            return false;
        }
        movie.key = key;
        movie.provider = JStr(d, "provider");
        movie.providerId = JStr(d, "providerId");
        movie.title = JWStr(d, "title");
        movie.year = (int)JNum(d, "year");
        movie.isEpisode = JBool(d, "isEpisode");
        movie.season = (int)JNum(d, "season", -1);
        movie.episode = (int)JNum(d, "episode", -1);
        movie.charactersChecked = JBool(d, "charactersChecked");
        movie.cast.clear();
        if (const auto* cast = JArr(d, "cast")) {
            for (const auto& v : cast->GetArray()) {
                Person p;
                ReadPersonFields(v, p);
                if (!p.id.empty()) {
                    movie.cast.push_back(std::move(p));
                }
            }
        }
        return !movie.cast.empty();
    }

    bool Service::SavePerson(const Person& p)
    {
        rapidjson::StringBuffer sb;
        JsonWriter w(sb);
        w.StartObject();
        w.Key("version");
        w.Int(kCacheVersion);
        WritePersonFields(w, p);
        w.Key("knownFor");
        w.StartArray();
        for (const auto& k : p.knownFor) {
            w.StartObject();
            WStr(w, "title", k.title);
            w.Key("year");
            w.Int(k.year);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        return WriteAll(PathFor(L"people", p.id, L".json"), std::string(sb.GetString(), sb.GetSize()));
    }

    bool Service::LoadPerson(Person& p)
    {
        std::string data;
        if (!ReadAll(PathFor(L"people", p.id, L".json"), data)) {
            return false;
        }
        rapidjson::Document d;
        d.Parse(data.c_str(), data.size());
        if (d.HasParseError() || !d.IsObject() || JNum(d, "version") != kCacheVersion) {
            return false;
        }
        // movie specific fields (character, order) are kept
        auto s = JWStr(d, "birthday");
        if (!s.empty()) {
            p.birthday = s;
        }
        s = JWStr(d, "deathday");
        if (!s.empty()) {
            p.deathday = s;
        }
        s = JWStr(d, "birthplace");
        if (!s.empty()) {
            p.birthplace = s;
        }
        if (p.imageUrl.empty()) {
            p.imageUrl = JStr(d, "image");
        }
        p.knownFor.clear();
        if (const auto* arr = JArr(d, "knownFor")) {
            for (const auto& v : arr->GetArray()) {
                p.knownFor.push_back({ JWStr(v, "title"), (int)JNum(v, "year") });
            }
        }
        p.detailsLoaded = true;
        return true;
    }

    void Service::LoadLearned(const std::string& movieKey)
    {
        if (m_learned.count(movieKey)) {
            return;
        }
        auto& learned = m_learned[movieKey];
        std::string data;
        if (!ReadAll(PathFor(L"learned", movieKey, L".bin"), data) || data.size() < 8 || data.compare(0, 4, "XRL1") != 0) {
            return;
        }
        size_t pos = 4;
        auto read = [&](void* dst, size_t n) {
            if (pos + n > data.size()) {
                return false;
            }
            memcpy(dst, data.data() + pos, n);
            pos += n;
            return true;
        };
        uint32_t people = 0;
        if (!read(&people, 4)) {
            return;
        }
        for (uint32_t i = 0; i < people; i++) {
            uint16_t idLen = 0;
            uint32_t count = 0;
            if (!read(&idLen, 2) || pos + idLen > data.size()) {
                return;
            }
            std::string id(data.data() + pos, idLen);
            pos += idLen;
            if (!read(&count, 4) || count > 64) {
                return;
            }
            auto& vec = learned[id];
            for (uint32_t j = 0; j < count; j++) {
                FaceEmbedding e;
                if (!read(e.data(), sizeof(float) * kEmbeddingSize)) {
                    return;
                }
                vec.push_back(e);
            }
        }
    }

    void Service::SaveLearned(const std::string& movieKey)
    {
        const auto& learned = m_learned[movieKey];
        std::string data = "XRL1";
        uint32_t people = (uint32_t)learned.size();
        data.append((const char*)&people, 4);
        for (const auto& kv : learned) {
            uint16_t idLen = (uint16_t)kv.first.size();
            uint32_t count = (uint32_t)kv.second.size();
            data.append((const char*)&idLen, 2);
            data.append(kv.first);
            data.append((const char*)&count, 4);
            for (const auto& e : kv.second) {
                data.append((const char*)e.data(), sizeof(float) * kEmbeddingSize);
            }
        }
        WriteAll(PathFor(L"learned", movieKey, L".bin"), data);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Scene timeline: who was recognized in which analyzed frame of a given file

    struct Service::Timeline {
        struct Face {
            std::string id;
            float x, y, w, h;
        };
        std::vector<std::pair<int64_t, int64_t>> covered; // analyzed ranges, sorted and disjoint
        std::set<int64_t> cuts;                            // camera cuts found while analyzing
        std::map<int64_t, std::vector<Face>> frames;       // analyzed frame time -> recognized faces

        void AddCovered(int64_t s, int64_t e)
        {
            if (e < s) {
                return;
            }
            covered.emplace_back(s, e);
            std::sort(covered.begin(), covered.end());
            std::vector<std::pair<int64_t, int64_t>> merged;
            for (const auto& r : covered) {
                // samples are up to kFarStepMs apart, so close ranges are continuous
                if (!merged.empty() && r.first <= merged.back().second + kFarStepMs) {
                    merged.back().second = std::max(merged.back().second, r.second);
                } else {
                    merged.push_back(r);
                }
            }
            covered = std::move(merged);
        }

        // Parts of [s, e] that weren't analyzed yet, ignoring gaps shorter than minLen
        std::vector<std::pair<int64_t, int64_t>> Uncovered(int64_t s, int64_t e, int64_t minLen) const
        {
            std::vector<std::pair<int64_t, int64_t>> out;
            int64_t cur = s;
            for (const auto& r : covered) {
                if (r.second < cur) {
                    continue;
                }
                if (r.first > e) {
                    break;
                }
                if (r.first > cur && r.first - cur >= minLen) {
                    out.emplace_back(cur, r.first);
                }
                cur = std::max(cur, r.second);
            }
            if (e - cur >= minLen) {
                out.emplace_back(cur, e);
            }
            return out;
        }

        bool CutBetween(int64_t a, int64_t b) const
        {
            if (a > b) {
                std::swap(a, b);
            }
            auto it = cuts.upper_bound(a);
            return it != cuts.end() && *it <= b;
        }
    };

    struct Service::Observation {
        int frame = -1;             // index of the analyzed frame
        int64_t timeMs = 0;
        float x = 0, y = 0, w = 0, h = 0; // relative to the frame
        FaceEmbedding emb{};
        int castIndex = -1;         // set by IdentifyObservations
        float score = 0;
    };

    bool Service::LoadTimeline(const std::string& key, Timeline& timeline)
    {
        std::string data;
        if (!ReadAll(PathFor(L"timeline", key, L".json"), data)) {
            return false;
        }
        rapidjson::Document d;
        d.Parse(data.c_str(), data.size());
        if (d.HasParseError() || !d.IsObject() || (int)JNum(d, "v") != 1) {
            return false;
        }
        if (const auto* arr = JArr(d, "covered")) {
            for (const auto& r : arr->GetArray()) {
                if (r.IsArray() && r.Size() == 2 && r[0].IsNumber() && r[1].IsNumber()) {
                    timeline.covered.emplace_back(r[0].GetInt64(), r[1].GetInt64());
                }
            }
            std::sort(timeline.covered.begin(), timeline.covered.end());
        }
        if (const auto* arr = JArr(d, "cuts")) {
            for (const auto& c : arr->GetArray()) {
                if (c.IsNumber()) {
                    timeline.cuts.insert(c.GetInt64());
                }
            }
        }
        if (const auto* arr = JArr(d, "frames")) {
            for (const auto& f : arr->GetArray()) {
                if (!f.IsArray() || f.Size() != 2 || !f[0].IsNumber() || !f[1].IsArray()) {
                    continue;
                }
                auto& faces = timeline.frames[f[0].GetInt64()];
                for (const auto& v : f[1].GetArray()) {
                    if (v.IsArray() && v.Size() == 5 && v[0].IsString()) {
                        faces.push_back({ v[0].GetString(), (float)v[1].GetDouble(), (float)v[2].GetDouble(), (float)v[3].GetDouble(), (float)v[4].GetDouble() });
                    }
                }
            }
        }
        return true;
    }

    void Service::SaveTimeline(const std::string& key, const Timeline& timeline)
    {
        rapidjson::StringBuffer sb;
        JsonWriter w(sb);
        auto round3 = [](float v) {
            return std::round(v * 1000.0) / 1000.0;
        };
        w.StartObject();
        w.Key("v");
        w.Int(1);
        w.Key("covered");
        w.StartArray();
        for (const auto& r : timeline.covered) {
            w.StartArray();
            w.Int64(r.first);
            w.Int64(r.second);
            w.EndArray();
        }
        w.EndArray();
        w.Key("cuts");
        w.StartArray();
        for (int64_t c : timeline.cuts) {
            w.Int64(c);
        }
        w.EndArray();
        w.Key("frames");
        w.StartArray();
        for (const auto& kv : timeline.frames) {
            w.StartArray();
            w.Int64(kv.first);
            w.StartArray();
            for (const auto& f : kv.second) {
                w.StartArray();
                w.String(f.id.c_str(), (rapidjson::SizeType)f.id.size());
                w.Double(round3(f.x));
                w.Double(round3(f.y));
                w.Double(round3(f.w));
                w.Double(round3(f.h));
                w.EndArray();
            }
            w.EndArray();
            w.EndArray();
        }
        w.EndArray();
        w.EndObject();
        WriteAll(PathFor(L"timeline", key, L".json"), std::string(sb.GetString(), sb.GetSize()));
    }

    bool Service::IdentifyObservations(const Movie& movie, std::vector<Observation>& obs, const std::vector<std::vector<float>>& castScores,
                                       const std::vector<int>& shotOf, bool learn)
    {
        const size_t n = obs.size();
        for (auto& o : obs) {
            o.castIndex = -1;
            o.score = 0;
        }
        if (n == 0) {
            return false;
        }

        // 1. group the faces per person: very similar faces anywhere in the scene, or reasonably similar faces
        //    at the same place in consecutive samples of the same shot (a head turning away from the camera).
        //    Two faces of the same frame are never the same person.
        struct Link {
            size_t a, b;
            float strength;
        };
        std::vector<Link> links;
        for (size_t a = 0; a < n; a++) {
            for (size_t b = a + 1; b < n; b++) {
                const Observation& oa = obs[a];
                const Observation& ob = obs[b];
                if (oa.frame == ob.frame) {
                    continue;
                }
                const float sim = CosineSimilarity(oa.emb, ob.emb);
                if (sim >= kSameFaceScore) {
                    links.push_back({ a, b, sim });
                } else if (sim >= kTrackFaceScore && shotOf[oa.frame] == shotOf[ob.frame]
                           && std::llabs(oa.timeMs - ob.timeMs) <= kMaxShotGapMs
                           && IoU(oa.x, oa.y, oa.w, oa.h, ob.x, ob.y, ob.w, ob.h) >= kTrackIoU) {
                    links.push_back({ a, b, sim + 0.2f });
                }
            }
        }
        std::sort(links.begin(), links.end(), [](const Link & l, const Link & r) {
            return l.strength > r.strength;
        });
        std::vector<size_t> parent(n);
        std::vector<std::set<int>> framesOf(n);
        for (size_t i = 0; i < n; i++) {
            parent[i] = i;
            framesOf[i].insert(obs[i].frame);
        }
        std::function<size_t(size_t)> find = [&](size_t i) {
            while (parent[i] != i) {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        };
        auto disjoint = [](const std::set<int>& a, const std::set<int>& b) {
            for (int f : a) {
                if (b.count(f)) {
                    return false;
                }
            }
            return true;
        };
        for (const auto& l : links) {
            size_t ra = find(l.a), rb = find(l.b);
            if (ra == rb || !disjoint(framesOf[ra], framesOf[rb])) {
                continue;
            }
            if (framesOf[ra].size() < framesOf[rb].size()) {
                std::swap(ra, rb);
            }
            parent[rb] = ra;
            framesOf[ra].insert(framesOf[rb].begin(), framesOf[rb].end());
            framesOf[rb].clear();
        }
        std::map<size_t, std::vector<size_t>> groups;
        for (size_t i = 0; i < n; i++) {
            groups[find(i)].push_back(i);
        }

        // 2. name each group from its best views: a single clear frontal face is enough, and a group whose
        //    faces mostly agree on the same person can be named with less similarity
        struct Candidate {
            size_t root;
            int cast;
            float score;
            bool strong;
        };
        std::vector<Candidate> candidates;
        const size_t castCount = movie.cast.size();
        for (const auto& g : groups) {
            const auto& members = g.second;
            std::vector<float> score(castCount, -1.0f);
            for (size_t c = 0; c < castCount; c++) {
                std::vector<float> s;
                for (size_t m : members) {
                    if (castScores[m][c] > -1.0f) {
                        s.push_back(castScores[m][c]);
                    }
                }
                if (s.empty()) {
                    continue;
                }
                std::sort(s.begin(), s.end(), std::greater<float>());
                const size_t k = std::min<size_t>(3, s.size());
                float mean = 0;
                for (size_t i = 0; i < k; i++) {
                    mean += s[i];
                }
                mean /= k;
                score[c] = 0.6f * s[0] + 0.4f * mean;
            }
            int best = -1, second = -1;
            for (int c = 0; c < (int)castCount; c++) {
                if (score[c] <= -1.0f) {
                    continue;
                }
                if (best < 0 || score[c] > score[best]) {
                    second = best;
                    best = c;
                } else if (second < 0 || score[c] > score[second]) {
                    second = c;
                }
            }
            if (best < 0) {
                continue;
            }
            const float margin = score[best] - (second >= 0 ? score[second] : -1.0f);
            const bool strong = score[best] >= kMatchScore && margin >= kMatchMargin;
            const bool weak = score[best] >= kWeakMatchScore && margin >= kWeakMatchMargin;
            bool voted = false;
            if (!strong && !weak && members.size() >= 3 && score[best] >= kVoteScore && margin >= kVoteMargin) {
                size_t votes = 0;
                for (size_t m : members) {
                    const auto& cs = castScores[m];
                    if (std::max_element(cs.begin(), cs.end()) - cs.begin() == best) {
                        votes++;
                    }
                }
                voted = votes >= members.size() * kVoteShare;
            }
            if (strong || weak || voted) {
                candidates.push_back({ g.first, best, score[best], strong });
            }
        }

        // 3. one person per group; a person can own several groups only if they never share a frame
        //    (the same actor split in two groups, e.g. with and without glasses)
        std::sort(candidates.begin(), candidates.end(), [](const Candidate & a, const Candidate & b) {
            return a.score > b.score;
        });
        std::map<int, std::set<int>> framesOfCast;
        bool learnedChanged = false;
        auto& learned = m_learned[movie.key];
        for (const auto& c : candidates) {
            auto& used = framesOfCast[c.cast];
            if (!disjoint(used, framesOf[c.root])) {
                continue;
            }
            used.insert(framesOf[c.root].begin(), framesOf[c.root].end());
            const auto& members = groups[c.root];
            for (size_t m : members) {
                obs[m].castIndex = c.cast;
                obs[m].score = c.score;
            }
            if (!learn || !c.strong) {
                continue;
            }
            // remember how the actor looks in this very movie: the clearest view and the most different
            // view of the same person (another angle), both help the next scenes
            size_t bestView = members[0];
            for (size_t m : members) {
                if (castScores[m][c.cast] > castScores[bestView][c.cast]) {
                    bestView = m;
                }
            }
            std::vector<size_t> add;
            if (castScores[bestView][c.cast] >= kLearnScore) {
                add.push_back(bestView);
            }
            size_t novel = SIZE_MAX;
            for (size_t m : members) {
                if (m != bestView && CosineSimilarity(obs[m].emb, obs[bestView].emb) >= 0.6f && castScores[m][c.cast] < 0.40f
                        && (novel == SIZE_MAX || castScores[m][c.cast] < castScores[novel][c.cast])) {
                    novel = m;
                }
            }
            if (novel != SIZE_MAX && !add.empty()) {
                add.push_back(novel);
            }
            auto& vec = learned[movie.cast[c.cast].id];
            for (size_t m : add) {
                const bool known = std::any_of(vec.begin(), vec.end(), [&](const FaceEmbedding & e) {
                    return CosineSimilarity(e, obs[m].emb) > 0.95f;
                });
                if (known) {
                    continue;
                }
                if (vec.size() >= kMaxLearnedPerPerson) {
                    vec.erase(vec.begin());
                }
                vec.push_back(obs[m].emb);
                learnedChanged = true;
            }
        }
        return learnedChanged;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Cast lookup

    bool Service::FetchTmdbMovie(const MediaQuery& q, const std::string& key, Movie& movie, std::wstring& error)
    {
        const auto headers = TmdbHeaders(key);
        std::string body;
        long status = 0;
        const std::string title = UrlEncode(ToUtf8(q.title));
        const bool tv = q.IsEpisode();

        auto search = [&](bool withYear) {
            std::string query = "include_adult=false&query=" + title;
            if (withYear && q.year) {
                query += (tv ? "&first_air_date_year=" : "&year=") + std::to_string(q.year);
            }
            return HttpGet(TmdbUrl(tv ? "search/tv" : "search/movie", key, query), headers, body, &status);
        };
        rapidjson::Document d;
        const rapidjson::Value* results = nullptr;
        for (bool withYear : { true, false }) {
            if (!withYear && !q.year) {
                break;
            }
            if (!search(withYear)) {
                if (status == 401) {
                    error = L"TMDb rejected the API key";
                    return false;
                }
                continue;
            }
            d.Parse(body.c_str(), body.size());
            results = d.HasParseError() ? nullptr : JArr(d, "results");
            if (results && !results->Empty()) {
                break;
            }
            results = nullptr;
        }
        if (!results) {
            return false;
        }

        // prefer an exact title match among the first results
        const rapidjson::Value* best = &(*results)[0];
        const std::wstring wanted = ToLower(q.title);
        for (rapidjson::SizeType i = 0; i < std::min<rapidjson::SizeType>(results->Size(), 5); i++) {
            const auto& r = (*results)[i];
            if (ToLower(JWStr(r, tv ? "name" : "title")) == wanted || ToLower(JWStr(r, tv ? "original_name" : "original_title")) == wanted) {
                best = &r;
                break;
            }
        }
        const int id = (int)JNum(*best, "id");
        movie.provider = "tmdb";
        movie.title = JWStr(*best, tv ? "name" : "title");
        movie.year = YearOf(JStr(*best, tv ? "first_air_date" : "release_date"));
        movie.isEpisode = tv;
        movie.season = q.season;
        movie.episode = q.episode;
        movie.providerId = (tv ? "tv/" : "movie/") + std::to_string(id);

        std::vector<const rapidjson::Value*> castArrays;
        rapidjson::Document credits;
        bool ok = false;
        if (tv) {
            ok = HttpGet(TmdbUrl("tv/" + std::to_string(id) + "/season/" + std::to_string(q.season) + "/episode/" + std::to_string(q.episode) + "/credits", key), headers, body);
            if (ok) {
                credits.Parse(body.c_str(), body.size());
                ok = !credits.HasParseError();
            }
            if (ok) {
                if (auto* a = JArr(credits, "cast")) {
                    castArrays.push_back(a);
                }
                if (auto* a = JArr(credits, "guest_stars")) {
                    castArrays.push_back(a);
                }
            }
            if (castArrays.empty()) {
                ok = HttpGet(TmdbUrl("tv/" + std::to_string(id) + "/aggregate_credits", key), headers, body);
                if (ok) {
                    credits.Parse(body.c_str(), body.size());
                    if (!credits.HasParseError()) {
                        if (auto* a = JArr(credits, "cast")) {
                            castArrays.push_back(a);
                        }
                    }
                }
            }
        } else {
            ok = HttpGet(TmdbUrl("movie/" + std::to_string(id) + "/credits", key), headers, body);
            if (ok) {
                credits.Parse(body.c_str(), body.size());
                if (!credits.HasParseError()) {
                    if (auto* a = JArr(credits, "cast")) {
                        castArrays.push_back(a);
                    }
                }
            }
        }

        std::set<std::string> seen;
        for (const auto* arr : castArrays) {
            for (const auto& c : arr->GetArray()) {
                Person p;
                p.id = "tmdb:" + std::to_string((int64_t)JNum(c, "id"));
                if (!seen.insert(p.id).second) {
                    continue;
                }
                p.name = JWStr(c, "name");
                p.character = JWStr(c, "character");
                if (p.character.empty()) {
                    if (const auto* roles = JArr(c, "roles")) {
                        if (!roles->Empty()) {
                            p.character = JWStr((*roles)[0], "character");
                        }
                    }
                }
                const std::string profile = JStr(c, "profile_path");
                if (!profile.empty()) {
                    p.imageUrl = "https://image.tmdb.org/t/p/w185" + profile;
                }
                p.order = (int)movie.cast.size();
                movie.cast.push_back(std::move(p));
            }
        }
        if (movie.cast.size() > kMaxCast) {
            movie.cast.resize(kMaxCast);
        }
        if (movie.cast.empty()) {
            error = L"TMDb has no cast for " + movie.title;
            return false;
        }
        return true;
    }

    bool Service::FetchWikidataMovie(const MediaQuery& q, Movie& movie, std::wstring& error)
    {
        const bool tv = q.IsEpisode();
        const std::wstring wanted = ToLower(q.title);
        int bestScore = 0;
        std::string bestId;
        std::wstring bestLabel;

        // score the candidates using their short description, e.g. "2010 film by Christopher Nolan"
        auto consider = [&](const std::string & id, const std::wstring & label, const std::wstring & description, int bonus) {
            const std::wstring desc = ToLower(description);
            auto has = [&](const wchar_t* s) {
                return desc.find(s) != std::wstring::npos;
            };
            const bool isFilm = has(L"film") || has(L"movie");
            const bool isSeries = has(L"series") || has(L"sitcom") || has(L"television") || has(L"tv show") || has(L"anime")
                                  || has(L"soap opera") || has(L"telenovela");
            int score = bonus;
            if (tv ? (isSeries && !has(L"film series")) : isFilm) {
                score += 10;
            } else if (isFilm || isSeries) {
                score += 4;
            } else {
                return;
            }
            if (has(L"episode") || has(L"season") || has(L"soundtrack") || has(L"film series") || has(L"franchise")
                    || has(L"video game") || has(L"novel") || has(L"book")) {
                score -= 12;
            }
            if (ToLower(label) == wanted) {
                score += 5;
            }
            if (q.year && has(std::to_wstring(q.year).c_str())) {
                score += 8;
            }
            if (score > bestScore) {
                bestScore = score;
                bestId = id;
                bestLabel = label;
            }
        };

        std::string body;
        rapidjson::Document d;
        const std::string search = "https://www.wikidata.org/w/api.php?action=wbsearchentities&format=json&language=en&uselang=en&type=item&limit=20&search="
                                   + UrlEncode(ToUtf8(q.title));
        if (!HttpGet(search, { L"Accept: application/json" }, body)) {
            error = L"Wikidata is not reachable";
            return false;
        }
        d.Parse(body.c_str(), body.size());
        if (const auto* results = d.HasParseError() ? nullptr : JArr(d, "search")) {
            for (const auto& r : results->GetArray()) {
                consider(JStr(r, "id"), JWStr(r, "label"), JWStr(r, "description"), 0);
            }
        }

        // label search is prefix based and punctuation sensitive ("Philosophers Stone" vs "Philosopher's Stone"),
        // so fall back to the full text search restricted to films or TV series
        if (bestScore < 10) {
            const std::string fulltext = "https://www.wikidata.org/w/api.php?action=query&list=search&format=json&srlimit=8&srsearch="
                                         + UrlEncode(ToUtf8(q.title) + (tv ? " haswbstatement:P31=Q5398426" : " haswbstatement:P31=Q11424"));
            std::vector<std::string> ids;
            if (HttpGet(fulltext, { L"Accept: application/json" }, body)) {
                d.Parse(body.c_str(), body.size());
                auto it = d.HasParseError() || !d.IsObject() ? d.MemberEnd() : d.FindMember("query");
                if (it != d.MemberEnd()) {
                    if (const auto* hits = JArr(it->value, "search")) {
                        for (const auto& h : hits->GetArray()) {
                            const std::string id = JStr(h, "title");
                            if (IsWikidataId(id)) {
                                ids.push_back(id);
                            }
                        }
                    }
                }
            }
            if (!ids.empty()) {
                std::string joined;
                for (const auto& id : ids) {
                    joined += (joined.empty() ? "" : "|") + id;
                }
                if (HttpGet("https://www.wikidata.org/w/api.php?action=wbgetentities&format=json&props=labels%7Cdescriptions&languages=en&ids=" + UrlEncode(joined),
                            { L"Accept: application/json" }, body)) {
                    d.Parse(body.c_str(), body.size());
                    auto ents = d.HasParseError() || !d.IsObject() ? d.MemberEnd() : d.FindMember("entities");
                    if (ents != d.MemberEnd() && ents->value.IsObject()) {
                        for (size_t i = 0; i < ids.size(); i++) {
                            auto e = ents->value.FindMember(ids[i].c_str());
                            if (e == ents->value.MemberEnd() || !e->value.IsObject()) {
                                continue;
                            }
                            auto text = [&](const char* group) {
                                auto g = e->value.FindMember(group);
                                if (g == e->value.MemberEnd() || !g->value.IsObject()) {
                                    return std::wstring();
                                }
                                auto en = g->value.FindMember("en");
                                return en != g->value.MemberEnd() ? JWStr(en->value, "value") : std::wstring();
                            };
                            // the search relevance order matters, the first hits get a small bonus
                            consider(ids[i], text("labels"), text("descriptions"), std::max(0, 3 - (int)i));
                        }
                    }
                }
            }
        }
        if (bestId.empty()) {
            return false;
        }

        const std::string sparql =
            "SELECT ?actor ?actorLabel ?charLabel ?image ?dob ?dod ?pobLabel ?filmDate ?links WHERE {"
            " wd:" + bestId + " p:P161 ?st . ?st ps:P161 ?actor ."
            " OPTIONAL { ?st pq:P453 ?char . }"
            " OPTIONAL { ?actor wdt:P18 ?image . }"
            " OPTIONAL { ?actor wdt:P569 ?dob . }"
            " OPTIONAL { ?actor wdt:P570 ?dod . }"
            " OPTIONAL { ?actor wdt:P19 ?pob . }"
            " OPTIONAL { ?actor wikibase:sitelinks ?links . }"
            " OPTIONAL { wd:" + bestId + " wdt:P577 ?filmDate . }"
            " SERVICE wikibase:label { bd:serviceParam wikibase:language \"en\". } }";
        if (!HttpGet("https://query.wikidata.org/sparql?format=json&query=" + UrlEncode(sparql), { L"Accept: application/sparql-results+json" }, body)) {
            error = L"Wikidata query failed";
            return false;
        }
        d.Parse(body.c_str(), body.size());
        if (d.HasParseError() || !d.IsObject() || !d.HasMember("results")) {
            return false;
        }
        const auto* bindings = JArr(d["results"], "bindings");
        if (!bindings) {
            return false;
        }

        movie.provider = "wikidata";
        movie.providerId = bestId;
        movie.title = bestLabel;
        movie.isEpisode = tv;
        movie.season = q.season;
        movie.episode = q.episode;
        std::map<std::string, size_t> index;
        std::map<std::string, int> links;
        for (const auto& b : bindings->GetArray()) {
            if (!movie.year) {
                movie.year = YearOf(SparqlValue(b, "filmDate"));
            }
            const std::string qid = WikidataId(SparqlValue(b, "actor"));
            if (qid.empty()) {
                continue;
            }
            links[qid] = std::max(links[qid], atoi(SparqlValue(b, "links").c_str()));
            auto it = index.find(qid);
            if (it == index.end()) {
                Person p;
                p.id = "wd:" + qid;
                p.name = ToWide(SparqlValue(b, "actorLabel"));
                if (p.name.empty() || p.name == ToWide(qid)) {
                    continue; // no English label
                }
                p.order = (int)movie.cast.size();
                index[qid] = movie.cast.size();
                movie.cast.push_back(std::move(p));
                it = index.find(qid);
            }
            Person& p = movie.cast[it->second];
            auto fill = [](std::wstring & dst, const std::string & src) {
                if (dst.empty() && !src.empty() && src.compare(0, 4, "http") != 0 && src.compare(0, 2, "_:") != 0 && !IsWikidataId(src)) {
                    dst = ToWide(src);
                }
            };
            // one row per role, e.g. twins or a character with several names
            const std::string role = SparqlValue(b, "charLabel");
            if (!role.empty() && role.compare(0, 4, "http") != 0 && role.compare(0, 2, "_:") != 0 && !IsWikidataId(role)) {
                AddCharacter(p.character, ToWide(role));
            }
            const std::string dob = SparqlValue(b, "dob"), dod = SparqlValue(b, "dod");
            fill(p.birthday, dob.substr(0, std::min<size_t>(10, dob.size())));
            fill(p.deathday, dod.substr(0, std::min<size_t>(10, dod.size())));
            fill(p.birthplace, SparqlValue(b, "pobLabel"));
            if (p.imageUrl.empty()) {
                p.imageUrl = WikidataImageUrl(SparqlValue(b, "image"));
            }
        }
        // Wikidata has no billing order: the most notable people first, so the main cast survives the cap
        std::stable_sort(movie.cast.begin(), movie.cast.end(), [&](const Person & a, const Person & b) {
            return links[a.id.substr(3)] > links[b.id.substr(3)];
        });
        for (size_t i = 0; i < movie.cast.size(); i++) {
            movie.cast[i].order = (int)i;
        }
        if (movie.cast.size() > kMaxCast) {
            movie.cast.resize(kMaxCast);
        }
        if (movie.cast.empty()) {
            error = L"Wikidata has no cast for " + movie.title;
            return false;
        }
        return true;
    }

    // Wikidata rarely knows the roles ("character role" qualifier), the cast section of the Wikipedia article does.
    // It is also the billing order and often lists more actors. Returns true when the movie was updated.
    bool Service::CompleteCastFromWikipedia(Movie& movie)
    {
        if (movie.provider != "wikidata" || !IsWikidataId(movie.providerId)) {
            return false;
        }
        std::string body;
        rapidjson::Document d;
        if (!HttpGet("https://www.wikidata.org/w/api.php?action=wbgetentities&format=json&props=sitelinks&sitefilter=enwiki&ids=" + movie.providerId,
                     { L"Accept: application/json" }, body)) {
            return false; // offline, try again next time
        }
        d.Parse(body.c_str(), body.size());
        std::string article;
        if (!d.HasParseError() && d.IsObject() && d.HasMember("entities") && d["entities"].IsObject()) {
            auto e = d["entities"].FindMember(movie.providerId.c_str());
            if (e != d["entities"].MemberEnd() && e->value.IsObject() && e->value.HasMember("sitelinks") && e->value["sitelinks"].IsObject()) {
                auto en = e->value["sitelinks"].FindMember("enwiki");
                if (en != e->value["sitelinks"].MemberEnd()) {
                    article = JStr(en->value, "title");
                }
            }
        }
        if (article.empty()) {
            movie.charactersChecked = true;
            return true;
        }
        if (!HttpGet("https://en.wikipedia.org/w/api.php?action=parse&format=json&formatversion=2&redirects=1&prop=wikitext&page=" + UrlEncode(article),
                     { L"Accept: application/json" }, body)) {
            return false;
        }
        d.Parse(body.c_str(), body.size());
        if (d.HasParseError() || !d.IsObject() || !d.HasMember("parse") || !d["parse"].IsObject()) {
            movie.charactersChecked = true;
            return true;
        }
        const std::vector<WikiCastEntry> entries = ParseWikipediaCast(JWStr(d["parse"], "wikitext"));

        // the linked articles give the Wikidata ids, so that an actor is found even if spelled differently
        std::map<std::wstring, std::string> qidOf; // link -> Q id of a human
        std::vector<std::wstring> titles;
        for (const auto& e : entries) {
            if (!e.link.empty() && std::find(titles.begin(), titles.end(), e.link) == titles.end()) {
                titles.push_back(e.link);
            }
        }
        for (size_t i = 0; i < titles.size(); i += 50) {
            std::string joined;
            for (size_t k = i; k < std::min(titles.size(), i + 50); k++) {
                joined += (joined.empty() ? "" : "|") + ToUtf8(titles[k]);
            }
            if (!HttpGet("https://en.wikipedia.org/w/api.php?action=query&format=json&formatversion=2&redirects=1&prop=pageprops&ppprop=wikibase_item&titles="
                         + UrlEncode(joined), { L"Accept: application/json" }, body)) {
                return false;
            }
            d.Parse(body.c_str(), body.size());
            if (d.HasParseError() || !d.IsObject() || !d.HasMember("query") || !d["query"].IsObject()) {
                continue;
            }
            const auto& query = d["query"];
            std::map<std::wstring, std::wstring> renamed;
            for (const char* group : { "normalized", "redirects" }) {
                if (const auto* list = JArr(query, group)) {
                    for (const auto& r : list->GetArray()) {
                        renamed[JWStr(r, "from")] = JWStr(r, "to");
                    }
                }
            }
            std::map<std::wstring, std::string> pageQid;
            if (const auto* pages = JArr(query, "pages")) {
                for (const auto& pg : pages->GetArray()) {
                    if (pg.IsObject() && pg.HasMember("pageprops") && pg["pageprops"].IsObject()) {
                        pageQid[JWStr(pg, "title")] = JStr(pg["pageprops"], "wikibase_item");
                    }
                }
            }
            for (size_t k = i; k < std::min(titles.size(), i + 50); k++) {
                std::wstring t = titles[k];
                for (int hop = 0; hop < 3 && renamed.count(t); hop++) {
                    t = renamed[t];
                }
                auto q = pageQid.find(t);
                if (q != pageQid.end() && IsWikidataId(q->second)) {
                    qidOf[titles[k]] = q->second;
                }
            }
        }

        // only people: a link may point to the character's own article
        std::map<std::string, Person> found;
        if (!qidOf.empty()) {
            std::string values;
            for (const auto& kv : qidOf) {
                values += " wd:" + kv.second;
            }
            const std::string sparql =
                "SELECT ?actor ?actorLabel ?image ?dob ?dod ?pobLabel WHERE {"
                " VALUES ?actor {" + values + " } ?actor wdt:P31 wd:Q5 ."
                " OPTIONAL { ?actor wdt:P18 ?image . }"
                " OPTIONAL { ?actor wdt:P569 ?dob . }"
                " OPTIONAL { ?actor wdt:P570 ?dod . }"
                " OPTIONAL { ?actor wdt:P19 ?pob . }"
                " SERVICE wikibase:label { bd:serviceParam wikibase:language \"en\". } }";
            if (!HttpGet("https://query.wikidata.org/sparql?format=json&query=" + UrlEncode(sparql), { L"Accept: application/sparql-results+json" }, body)) {
                return false; // try again next time rather than losing the actors
            }
            {
                d.Parse(body.c_str(), body.size());
                const auto* bindings = !d.HasParseError() && d.IsObject() && d.HasMember("results") ? JArr(d["results"], "bindings") : nullptr;
                if (!bindings) {
                    return false;
                }
                {
                    for (const auto& b : bindings->GetArray()) {
                        const std::string qid = WikidataId(SparqlValue(b, "actor"));
                        if (found.count(qid)) {
                            continue;
                        }
                        Person p;
                        p.id = "wd:" + qid;
                        p.name = ToWide(SparqlValue(b, "actorLabel"));
                        const std::string dob = SparqlValue(b, "dob"), dod = SparqlValue(b, "dod"), pob = SparqlValue(b, "pobLabel");
                        p.birthday = ToWide(dob.substr(0, std::min<size_t>(10, dob.size())));
                        p.deathday = ToWide(dod.substr(0, std::min<size_t>(10, dod.size())));
                        if (!IsWikidataId(pob)) {
                            p.birthplace = ToWide(pob);
                        }
                        p.imageUrl = WikidataImageUrl(SparqlValue(b, "image"));
                        found[qid] = std::move(p);
                    }
                }
            }
        }

        // merge, the Wikipedia list order becomes the billing order
        std::vector<Person> ordered;
        std::vector<bool> used(movie.cast.size(), false);
        for (const auto& e : entries) {
            auto q = qidOf.find(e.link);
            const std::string qid = q != qidOf.end() && found.count(q->second) ? q->second : std::string();
            int index = -1;
            for (size_t i = 0; i < movie.cast.size() && index < 0; i++) {
                if (!qid.empty() ? movie.cast[i].id == "wd:" + qid : NameKey(movie.cast[i].name) == NameKey(e.actor)) {
                    index = (int)i;
                }
            }
            Person* p = nullptr;
            if (index >= 0) {
                if (!used[index]) {
                    used[index] = true;
                    ordered.push_back(movie.cast[index]);
                    ordered.back().character.clear(); // Wikipedia names the roles more completely
                }
                p = &*std::find_if(ordered.begin(), ordered.end(), [&](const Person & o) {
                    return o.id == movie.cast[index].id;
                });
            } else if (!qid.empty()) {
                auto existing = std::find_if(ordered.begin(), ordered.end(), [&](const Person & o) {
                    return o.id == "wd:" + qid;
                });
                if (existing == ordered.end()) {
                    ordered.push_back(found[qid]);
                    existing = ordered.end() - 1;
                }
                p = &*existing;
            }
            if (p) {
                AddCharacter(p->character, e.character);
            }
        }
        for (size_t i = 0; i < movie.cast.size(); i++) {
            if (!used[i]) {
                ordered.push_back(movie.cast[i]);
            }
        }
        // keep a role from Wikidata when Wikipedia didn't give one
        for (auto& p : ordered) {
            if (p.character.empty()) {
                for (const auto& old : movie.cast) {
                    if (old.id == p.id) {
                        p.character = old.character;
                    }
                }
            }
        }
        if (ordered.size() > kMaxCast) {
            ordered.resize(kMaxCast);
        }
        for (size_t i = 0; i < ordered.size(); i++) {
            ordered[i].order = (int)i;
        }
        movie.cast = std::move(ordered);
        movie.charactersChecked = true;
        return true;
    }

    std::shared_ptr<Movie> Service::ResolveMovie(const MediaQuery& q, const std::string& tmdbKey, const std::atomic<bool>& cancel, std::wstring& error)
    {
        const std::string key = ToUtf8(MovieKey(q, tmdbKey.empty() ? "wikidata" : "tmdb"));
        {
            std::lock_guard<std::mutex> lock(m_dataLock);
            auto it = m_movies.find(key);
            if (it != m_movies.end()) {
                return it->second;
            }
        }
        auto movie = std::make_shared<Movie>();
        bool ok = LoadMovie(key, *movie);
        if (ok && movie->provider == "wikidata" && !movie->charactersChecked && !cancel) {
            // cached before the character names were completed
            if (CompleteCastFromWikipedia(*movie)) {
                SaveMovie(*movie);
            }
        }
        if (!ok && !cancel) {
            movie = std::make_shared<Movie>();
            if (!tmdbKey.empty()) {
                ok = FetchTmdbMovie(q, tmdbKey, *movie, error);
            }
            if (!ok && !cancel) {
                std::wstring wdError;
                movie = std::make_shared<Movie>();
                ok = FetchWikidataMovie(q, *movie, wdError);
                if (ok && !cancel) {
                    CompleteCastFromWikipedia(*movie);
                }
                if (!ok && error.empty()) {
                    error = wdError;
                }
            }
            if (ok) {
                movie->key = key;
                SaveMovie(*movie);
            }
        }
        if (!ok) {
            return nullptr;
        }
        movie->key = key;
        std::lock_guard<std::mutex> lock(m_dataLock);
        m_movies[key] = movie;
        return movie;
    }

    bool Service::FetchTmdbPerson(Person& p, const std::string& key, const std::string& currentId)
    {
        if (key.empty()) {
            return false;
        }
        std::string body;
        const std::string id = p.id.substr(5);
        if (!HttpGet(TmdbUrl("person/" + id, key, "append_to_response=movie_credits,tv_credits"), TmdbHeaders(key), body)) {
            return false;
        }
        rapidjson::Document d;
        d.Parse(body.c_str(), body.size());
        if (d.HasParseError() || !d.IsObject()) {
            return false;
        }
        p.birthday = JWStr(d, "birthday");
        p.deathday = JWStr(d, "deathday");
        p.birthplace = JWStr(d, "place_of_birth");
        if (p.imageUrl.empty() && !JStr(d, "profile_path").empty()) {
            p.imageUrl = "https://image.tmdb.org/t/p/w185" + JStr(d, "profile_path");
        }

        struct Credit {
            std::wstring title;
            int year;
            double votes;
        };
        std::vector<Credit> credits;
        auto collect = [&](const char* group, bool tv) {
            auto it = d.FindMember(group);
            if (it == d.MemberEnd()) {
                return;
            }
            const auto* cast = JArr(it->value, "cast");
            if (!cast) {
                return;
            }
            for (const auto& c : cast->GetArray()) {
                const std::string cid = (tv ? "tv/" : "movie/") + std::to_string((int64_t)JNum(c, "id"));
                if (cid == currentId) {
                    continue;
                }
                // talk shows, news, award ceremonies... where the person appears as themself
                const std::wstring character = ToLower(JWStr(c, "character"));
                if (character.find(L"self") != std::wstring::npos || character.find(L"himself") != std::wstring::npos || character.find(L"herself") != std::wstring::npos) {
                    continue;
                }
                if (const auto* genres = JArr(c, "genre_ids")) {
                    bool skip = false;
                    for (const auto& g : genres->GetArray()) {
                        if (g.IsInt() && (g.GetInt() == 10767 || g.GetInt() == 10763 || g.GetInt() == 99)) {
                            skip = true;
                        }
                    }
                    if (skip) {
                        continue;
                    }
                }
                credits.push_back({ JWStr(c, tv ? "name" : "title"), YearOf(JStr(c, tv ? "first_air_date" : "release_date")), JNum(c, "vote_count") });
            }
        };
        collect("movie_credits", false);
        collect("tv_credits", true);
        std::stable_sort(credits.begin(), credits.end(), [](const Credit & a, const Credit & b) {
            return a.votes > b.votes;
        });
        p.knownFor.clear();
        std::set<std::wstring> titles;
        for (const auto& c : credits) {
            if (p.knownFor.size() >= kMaxKnownFor) {
                break;
            }
            if (!c.title.empty() && titles.insert(ToLower(c.title)).second) {
                p.knownFor.push_back({ c.title, c.year });
            }
        }
        return true;
    }

    bool Service::FetchWikidataPerson(Person& p, const std::string& currentId)
    {
        const std::string qid = p.id.substr(3);
        const std::string sparql =
            "SELECT ?film ?filmLabel (MIN(?date) AS ?first) (MAX(?sl) AS ?links) WHERE {"
            " ?film wdt:P161 wd:" + qid + " ; wikibase:sitelinks ?sl ."
            " VALUES ?type { wd:Q11424 wd:Q5398426 wd:Q506240 wd:Q24856 }"
            " ?film wdt:P31 ?type ."
            " OPTIONAL { ?film wdt:P577 ?date . }"
            " OPTIONAL { ?film wdt:P580 ?date . }"
            " SERVICE wikibase:label { bd:serviceParam wikibase:language \"en\". ?film rdfs:label ?filmLabel . } }"
            " GROUP BY ?film ?filmLabel ORDER BY DESC(?links) LIMIT 12";
        const std::string person =
            "SELECT ?dob ?dod ?pobLabel ?image WHERE {"
            " OPTIONAL { wd:" + qid + " wdt:P569 ?dob . }"
            " OPTIONAL { wd:" + qid + " wdt:P570 ?dod . }"
            " OPTIONAL { wd:" + qid + " wdt:P19 ?pob . }"
            " OPTIONAL { wd:" + qid + " wdt:P18 ?image . }"
            " SERVICE wikibase:label { bd:serviceParam wikibase:language \"en\". } } LIMIT 1";

        std::string body;
        rapidjson::Document d;
        if (p.birthday.empty() || p.imageUrl.empty()) {
            if (HttpGet("https://query.wikidata.org/sparql?format=json&query=" + UrlEncode(person), { L"Accept: application/sparql-results+json" }, body)) {
                d.Parse(body.c_str(), body.size());
                if (!d.HasParseError() && d.IsObject() && d.HasMember("results")) {
                    if (const auto* b = JArr(d["results"], "bindings")) {
                        if (!b->Empty()) {
                            const auto& r = (*b)[0];
                            const std::string dob = SparqlValue(r, "dob"), dod = SparqlValue(r, "dod");
                            if (p.birthday.empty()) {
                                p.birthday = ToWide(dob.substr(0, std::min<size_t>(10, dob.size())));
                            }
                            if (p.deathday.empty()) {
                                p.deathday = ToWide(dod.substr(0, std::min<size_t>(10, dod.size())));
                            }
                            if (p.birthplace.empty()) {
                                p.birthplace = ToWide(SparqlValue(r, "pobLabel"));
                            }
                            if (p.imageUrl.empty()) {
                                p.imageUrl = WikidataImageUrl(SparqlValue(r, "image"));
                            }
                        }
                    }
                }
            }
        }

        if (!HttpGet("https://query.wikidata.org/sparql?format=json&query=" + UrlEncode(sparql), { L"Accept: application/sparql-results+json" }, body)) {
            return false;
        }
        d.Parse(body.c_str(), body.size());
        if (d.HasParseError() || !d.IsObject() || !d.HasMember("results")) {
            return false;
        }
        const auto* bindings = JArr(d["results"], "bindings");
        if (!bindings) {
            return false;
        }
        p.knownFor.clear();
        std::set<std::wstring> titles;
        for (const auto& b : bindings->GetArray()) {
            if (p.knownFor.size() >= kMaxKnownFor) {
                break;
            }
            if (WikidataId(SparqlValue(b, "film")) == currentId) {
                continue;
            }
            std::wstring title = ToWide(SparqlValue(b, "filmLabel"));
            if (title.empty() || IsWikidataId(SparqlValue(b, "filmLabel"))) {
                continue;
            }
            if (titles.insert(ToLower(title)).second) {
                p.knownFor.push_back({ title, YearOf(SparqlValue(b, "first")) });
            }
        }
        return true;
    }

    bool Service::LoadPersonDetails(Person& person, const std::string& currentProviderId, const std::string& tmdbApiKey)
    {
        if (person.detailsLoaded) {
            return true;
        }
        if (LoadPerson(person)) {
            EnsureHeadshot(person);
            return true;
        }
        bool ok = false;
        if (person.id.compare(0, 5, "tmdb:") == 0) {
            ok = FetchTmdbPerson(person, tmdbApiKey, currentProviderId);
        } else if (person.id.compare(0, 3, "wd:") == 0) {
            ok = FetchWikidataPerson(person, currentProviderId);
        }
        if (ok) {
            person.detailsLoaded = true;
            std::lock_guard<std::mutex> lock(m_dataLock);
            SavePerson(person);
        }
        EnsureHeadshot(person);
        return ok;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Faces

    bool Service::EnsureHeadshot(Person& p)
    {
        const std::wstring path = PathFor(L"images", p.id, L".jpg");
        if (FileExists(path)) {
            p.imagePath = path;
            return true;
        }
        const std::wstring missing = PathFor(L"images", p.id, L".none");
        if (p.imageUrl.empty() || FileExists(missing)) {
            return false;
        }
        std::string body;
        long status = 0;
        if (HttpGet(p.imageUrl, {}, body, &status) && body.size() > 100) {
            if (WriteAll(path, body)) {
                p.imagePath = path;
                return true;
            }
        } else if (status == 404) {
            WriteAll(missing, std::string());
        }
        return false;
    }

    const std::vector<FaceEmbedding>& Service::GetReferenceEmbeddings(const Person& p)
    {
        auto it = m_referenceEmbeddings.find(p.id);
        if (it != m_referenceEmbeddings.end()) {
            return it->second;
        }
        auto& list = m_referenceEmbeddings[p.id];

        const std::wstring path = PathFor(L"faces", p.id, L".sface.bin");
        std::string data;
        if (ReadAll(path, data) && data.size() >= 8 && data.compare(0, 4, "XRE1") == 0) {
            uint32_t count = 0;
            memcpy(&count, data.data() + 4, 4);
            if (data.size() == 8 + size_t(count) * sizeof(float) * kEmbeddingSize) {
                list.resize(count);
                for (uint32_t i = 0; i < count; i++) {
                    memcpy(list[i].data(), data.data() + 8 + size_t(i) * sizeof(float) * kEmbeddingSize, sizeof(float) * kEmbeddingSize);
                }
                return list;
            }
        }

        if (p.imagePath.empty() || !m_engine.IsLoaded()) {
            // maybe a transient download failure, try again next time
            m_referenceEmbeddings.erase(p.id);
            static const std::vector<FaceEmbedding> none;
            return none;
        }
        std::string bytes;
        ImageBGR img;
        if (ReadAll(p.imagePath, bytes) && ImageFromEncoded((const uint8_t*)bytes.data(), bytes.size(), img)) {
            auto faces = m_engine.Detect(img, 0.5f, 16);
            // a headshot should contain one dominant face
            auto best = std::max_element(faces.begin(), faces.end(), [](const FaceDetection & a, const FaceDetection & b) {
                return a.w * a.h * a.score < b.w * b.h * b.score;
            });
            FaceEmbedding e;
            if (best != faces.end() && m_engine.Embed(img, *best, e)) {
                list.push_back(e);
            }
        }
        // also cache "no usable face" to avoid retrying every time
        std::string out = "XRE1";
        uint32_t count = (uint32_t)list.size();
        out.append((const char*)&count, 4);
        for (const auto& e : list) {
            out.append((const char*)e.data(), sizeof(float) * kEmbeddingSize);
        }
        WriteAll(path, out);
        return list;
    }

    // ---------------------------------------------------------------------------------------------------------------

    std::shared_ptr<Result> Service::Process(const Request& req, const std::function<void(const std::wstring&)>& progress, const std::atomic<bool>& cancel,
                                             const std::function<void(std::shared_ptr<Result>)>& partial)
    {
        auto result = std::make_shared<Result>();
        auto cancelled = [&]() {
            result->status = Result::Status::Cancelled;
            return result;
        };

        progress(L"Identifying the title\u2026");
        const MediaQuery q = ParseMediaName(req.mediaName);
        if (q.title.empty()) {
            result->status = Result::Status::MovieNotFound;
            result->message = L"Unable to guess the title from the file name";
            return result;
        }
        std::wstring error;
        auto cached = ResolveMovie(q, req.tmdbApiKey, cancel, error);
        if (cancel) {
            return cancelled();
        }
        if (!cached) {
            result->status = Result::Status::MovieNotFound;
            result->message = L"Couldn't find \u201C" + q.title + (q.year ? L" (" + std::to_wstring(q.year) + L")" : L"") + L"\u201D";
            if (!error.empty()) {
                result->message += L"\n" + error;
            }
            return result;
        }

        // the result owns its own copy, the UI may modify it freely
        result->movie = std::make_shared<Movie>(*cached);
        Movie& movie = *result->movie;
        for (auto& p : movie.cast) {
            LoadPerson(p);
            const std::wstring img = PathFor(L"images", p.id, L".jpg");
            if (FileExists(img)) {
                p.imagePath = img;
            }
        }

        auto finishDetails = [&]() {
            std::vector<Person*> pending;
            for (const auto& m : result->scene) {
                Person& p = movie.cast[m.castIndex];
                if (!p.detailsLoaded) {
                    pending.push_back(&p);
                }
            }
            if (pending.empty()) {
                return;
            }
            progress(L"Getting details about " + pending[0]->name + (pending.size() > 1 ? L" and others\u2026" : L"\u2026"));
            std::atomic<size_t> next{ 0 };
            std::vector<std::thread> workers;
            for (size_t t = 0; t < std::min<size_t>(4, pending.size()); t++) {
                workers.emplace_back([&] {
                    for (size_t k; !cancel && (k = next++) < pending.size();) {
                        LoadPersonDetails(*pending[k], movie.providerId, req.tmdbApiKey);
                    }
                });
            }
            for (auto& w : workers) {
                w.join();
            }
        };

        const int64_t pos = req.positionMs;

        // headshots are fetched in parallel, a cast list easily has 40 of them
        auto prefetchHeadshots = [&]() {
            std::vector<size_t> pending;
            {
                std::lock_guard<std::mutex> lock(m_dataLock);
                for (size_t i = 0; i < movie.cast.size(); i++) {
                    const Person& p = movie.cast[i];
                    if (!p.imagePath.empty() || p.imageUrl.empty() || FileExists(PathFor(L"images", p.id, L".none"))) {
                        continue;
                    }
                    pending.push_back(i);
                }
            }
            std::atomic<size_t> next{ 0 }, done{ 0 };
            std::vector<std::thread> workers;
            for (size_t t = 0; t < std::min<size_t>(4, pending.size()); t++) {
                workers.emplace_back([&] {
                    for (size_t k; !cancel && (k = next++) < pending.size();) {
                        EnsureHeadshot(movie.cast[pending[k]]);
                        done++;
                    }
                });
            }
            while (done < pending.size() && !cancel) {
                progress(L"Downloading cast photos\u2026 " + std::to_wstring(done.load()) + L"/" + std::to_wstring(pending.size()));
                Sleep(100);
            }
            for (auto& w : workers) {
                w.join();
            }
        };

        std::wstring engineError;
        if (!EnsureEngine(req.allowEngineDownload, progress, cancel, engineError)) {
            if (cancel) {
                return cancelled();
            }
            // still useful: show the whole cast
            prefetchHeadshots();
            result->status = Result::Status::Ok;
            result->recognitionAvailable = false;
            result->message = engineError;
            return cancel ? cancelled() : result;
        }
        result->recognitionAvailable = true;
        prefetchHeadshots();
        if (cancel) {
            return cancelled();
        }

        // reference faces of the cast (computed only once) and how they looked in this movie so far
        progress(L"Learning the cast faces\u2026");
        const size_t castCount = movie.cast.size();
        std::vector<std::vector<FaceEmbedding>> refs(castCount);
        {
            std::lock_guard<std::mutex> lock(m_dataLock);
            LoadLearned(movie.key);
            const auto& learned = m_learned[movie.key];
            for (size_t i = 0; i < castCount; i++) {
                if (cancel) {
                    return cancelled();
                }
                refs[i] = GetReferenceEmbeddings(movie.cast[i]);
                auto l = learned.find(movie.cast[i].id);
                if (l != learned.end()) {
                    refs[i].insert(refs[i].end(), l->second.begin(), l->second.end());
                }
            }
        }
        std::map<std::string, int> castIndexOf;
        for (size_t i = 0; i < castCount; i++) {
            castIndexOf[movie.cast[i].id] = (int)i;
        }

        // what was already analyzed in this very file
        const std::string timelineKey = movie.key + "-" + FileIdentity(req.mediaPath.empty() ? req.mediaName : req.mediaPath)
                                        + "-" + CastIdentity(movie.cast);
        Timeline timeline;
        {
            std::lock_guard<std::mutex> lock(m_dataLock);
            LoadTimeline(timelineKey, timeline);
        }

        struct Frame {
            int64_t timeMs;
            Signature sig;
            bool current;           // the frame captured from the renderer
        };
        std::vector<Frame> frames;
        std::vector<Observation> obs;
        std::vector<std::vector<float>> castScores; // per observation, per cast member

        auto analyzeFrame = [&](int64_t timeMs, const ImageBGR& img, bool current) {
            const int index = (int)frames.size();
            frames.push_back({ timeMs, ColorSignature(img), current });
            const auto faces = m_engine.Detect(img, 0.6f, std::max(24, img.height / 30));
            for (const auto& f : faces) {
                Observation o;
                if (!m_engine.Embed(img, f, o.emb)) {
                    continue;
                }
                o.frame = index;
                o.timeMs = timeMs;
                o.x = f.x / img.width;
                o.y = f.y / img.height;
                o.w = f.w / img.width;
                o.h = f.h / img.height;
                std::vector<float> s(castCount, -1.0f);
                for (size_t c = 0; c < castCount; c++) {
                    for (const auto& e : refs[c]) {
                        s[c] = std::max(s[c], CosineSimilarity(o.emb, e));
                    }
                }
                obs.push_back(o);
                castScores.push_back(std::move(s));
            }
            return (int)faces.size();
        };

        // Camera cuts between consecutive decoded samples. The captured frame can look different (renderer tone
        // mapping, scaling), so it joins the shot of the most similar decoded sample of the same moment.
        auto computeShots = [&](std::vector<int>& shotOf, std::set<int64_t>& cuts) {
            shotOf.assign(frames.size(), -1);
            std::vector<int> order;
            for (int i = 0; i < (int)frames.size(); i++) {
                if (!frames[i].current) {
                    order.push_back(i);
                }
            }
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return frames[a].timeMs < frames[b].timeMs;
            });
            int shot = 0;
            for (size_t k = 0; k < order.size(); k++) {
                if (k > 0) {
                    const Frame& a = frames[order[k - 1]];
                    const Frame& b = frames[order[k]];
                    const int64_t dt = b.timeMs - a.timeMs;
                    const bool cut = SignatureDistance(a.sig, b.sig) > kCutDistance;
                    if (cut || dt > kMaxShotGapMs) {
                        shot++;
                        if (cut && dt <= kMaxShotGapMs) {
                            cuts.insert(a.timeMs + dt / 2);
                        }
                    }
                }
                shotOf[order[k]] = shot;
            }
            for (int i = 0; i < (int)frames.size(); i++) {
                if (!frames[i].current) {
                    continue;
                }
                int best = -1;
                float bestDist = kCutDistance;
                for (int j : order) {
                    const float d = SignatureDistance(frames[i].sig, frames[j].sig);
                    if (std::llabs(frames[j].timeMs - frames[i].timeMs) <= kNearStepMs && d <= bestDist) {
                        best = j;
                        bestDist = d;
                    }
                }
                shotOf[i] = best >= 0 ? shotOf[best] : ++shot;
            }
        };

        // names the faces seen so far and stores them in the timeline
        auto commit = [&](bool learn) {
            std::vector<int> shotOf;
            std::set<int64_t> cuts;
            computeShots(shotOf, cuts);
            std::lock_guard<std::mutex> lock(m_dataLock);
            if (IdentifyObservations(movie, obs, castScores, shotOf, learn)) {
                SaveLearned(movie.key);
            }
            timeline.cuts.insert(cuts.begin(), cuts.end());
            for (int i = 0; i < (int)frames.size(); i++) {
                const Frame& f = frames[i];
                if (f.current) {
                    // a decoded sample of practically the same moment already represents it
                    const bool represented = std::any_of(frames.begin(), frames.end(), [&](const Frame & o) {
                        return !o.current && std::llabs(o.timeMs - f.timeMs) < kNearStepMs / 2;
                    });
                    if (represented) {
                        continue;
                    }
                }
                auto& faces = timeline.frames[f.timeMs];
                faces.clear();
                for (const auto& o : obs) {
                    if (o.frame == i && o.castIndex >= 0) {
                        faces.push_back({ movie.cast[o.castIndex].id, o.x, o.y, o.w, o.h });
                    }
                }
            }
            SaveTimeline(timelineKey, timeline);
        };

        // A face of the paused frame that couldn't be named by itself (turned away, blurred) is most likely the
        // person recognized at the same place a moment before or after, in the same shot.
        auto trackCurrent = [&]() {
            std::set<int> used;
            for (const auto& o : obs) {
                if (frames[o.frame].current && o.castIndex >= 0) {
                    used.insert(o.castIndex);
                }
            }
            for (auto& o : obs) {
                if (!frames[o.frame].current || o.castIndex >= 0) {
                    continue;
                }
                float bestIoU = kTrackIoU;
                int best = -1;
                for (auto it = timeline.frames.lower_bound(pos - kOnScreenMs); it != timeline.frames.end() && it->first <= pos + kOnScreenMs; ++it) {
                    if (timeline.CutBetween(it->first, pos)) {
                        continue;
                    }
                    for (const auto& f : it->second) {
                        auto c = castIndexOf.find(f.id);
                        const float v = IoU(o.x, o.y, o.w, o.h, f.x, f.y, f.w, f.h);
                        if (c != castIndexOf.end() && !used.count(c->second) && v > bestIoU) {
                            bestIoU = v;
                            best = c->second;
                        }
                    }
                }
                if (best >= 0) {
                    o.castIndex = best;
                    used.insert(best);
                }
            }
        };

        const int64_t sceneStart = std::max<int64_t>(0, pos - kSceneBeforeMs);
        int64_t sceneEnd = pos + kSceneAfterMs;

        auto buildResult = [&]() {
            result->scene.clear();
            result->sceneStartMs = sceneStart;
            result->sceneEndMs = sceneEnd;
            std::map<int, SceneMatch> people;
            std::map<int, int64_t> nearest;
            int analyzed = 0;
            for (auto it = timeline.frames.lower_bound(sceneStart); it != timeline.frames.end() && it->first <= sceneEnd; ++it) {
                analyzed++;
                const int64_t dt = std::llabs(it->first - pos);
                const bool now = dt <= kOnScreenMs && !timeline.CutBetween(it->first, pos);
                for (const auto& f : it->second) {
                    auto c = castIndexOf.find(f.id);
                    if (c == castIndexOf.end()) {
                        continue;
                    }
                    SceneMatch& m = people[c->second];
                    if (m.appearances++ == 0) {
                        m.castIndex = c->second;
                        m.firstMs = m.lastMs = it->first;
                    }
                    m.firstMs = std::min(m.firstMs, it->first);
                    m.lastMs = std::max(m.lastMs, it->first);
                    if (now && (!m.onScreen || dt < nearest[c->second])) {
                        m.onScreen = true;
                        nearest[c->second] = dt;
                        m.x = f.x;
                        m.y = f.y;
                        m.w = f.w;
                        m.h = f.h;
                    }
                }
            }
            // the captured frame is what is really on screen
            for (const auto& o : obs) {
                if (!frames[o.frame].current || o.castIndex < 0) {
                    continue;
                }
                SceneMatch& m = people[o.castIndex];
                if (m.appearances == 0) {
                    m.castIndex = o.castIndex;
                    m.appearances = 1;
                    m.firstMs = m.lastMs = pos;
                }
                m.onScreen = true;
                m.score = o.score;
                m.x = o.x;
                m.y = o.y;
                m.w = o.w;
                m.h = o.h;
            }
            result->framesAnalyzed = analyzed;
            for (const auto& kv : people) {
                result->scene.push_back(kv.second);
            }
            // on screen now from left to right, then the rest of the scene, most seen first
            std::sort(result->scene.begin(), result->scene.end(), [](const SceneMatch & a, const SceneMatch & b) {
                if (a.onScreen != b.onScreen) {
                    return a.onScreen;
                }
                if (a.onScreen) {
                    return a.x < b.x;
                }
                return a.appearances != b.appearances ? a.appearances > b.appearances : a.firstMs < b.firstMs;
            });
        };

        auto sendPartial = [&]() {
            if (!partial || cancel) {
                return;
            }
            auto copy = std::make_shared<Result>(*result);
            copy->movie = std::make_shared<Movie>(movie);
            copy->status = Result::Status::Ok;
            partial(copy);
        };

        // opens (or reuses) the decoder of the playing file
        auto openSampler = [&]() -> bool {
            if (m_sampler && m_samplerPath == req.mediaPath && m_sampler->IsOpen()) {
                return true;
            }
            progress(L"Opening the video for scene analysis\u2026");
            m_sampler.reset();
            m_samplerPath.clear();
            std::wstring samplerError;
            for (const auto& dir : m_videoLibDirs) {
                auto sampler = std::make_unique<VideoSampler>(dir);
                samplerError.clear();
                if (sampler->Open(req.mediaPath, samplerError)) {
                    m_sampler = std::move(sampler);
                    m_samplerPath = req.mediaPath;
                    return true;
                }
            }
            result->sceneNote = samplerError.empty() ? std::wstring(L"Scene analysis is unavailable: the FFmpeg libraries were not found") : samplerError;
            return false;
        };

        // 1. the paused frame: the one given by the player, or decoded from the file
        ImageBGR decodedFrame;
        if (req.frame.empty() && !req.mediaPath.empty() && openSampler()) {
            progress(L"Reading the paused frame\u2026");
            m_sampler->Sample(pos, pos + 1000, 1000, 1920, [&](int64_t, ImageBGR & img) {
                decodedFrame = std::move(img);
                return false;
            }, cancel);
        }
        const ImageBGR& pausedFrame = req.frame.empty() ? decodedFrame : req.frame;
        if (!pausedFrame.empty()) {
            progress(L"Looking for faces\u2026");
            result->facesDetected = analyzeFrame(pos, pausedFrame, true);
        } else {
            result->facesDetected = -1;
        }
        if (cancel) {
            return cancelled();
        }

        // 2. the parts of the scene that weren't analyzed before, closest to the pause first
        struct Piece {
            int64_t start, end, step;
        };
        std::vector<Piece> pieces;
        auto planPieces = [&]() {
            pieces.clear();
            const int64_t nearStart = std::max(sceneStart, pos - kNearMs);
            std::vector<Piece> nearPieces, farPieces;
            for (const auto& r : timeline.Uncovered(nearStart, sceneEnd, kNearStepMs)) {
                nearPieces.push_back({ r.first, r.second, kNearStepMs });
            }
            for (const auto& r : timeline.Uncovered(sceneStart, nearStart, kFarStepMs)) {
                farPieces.push_back({ r.first, r.second, kFarStepMs });
            }
            std::sort(nearPieces.begin(), nearPieces.end(), [&](const Piece & a, const Piece & b) {
                return std::llabs((a.start + a.end) / 2 - pos) < std::llabs((b.start + b.end) / 2 - pos);
            });
            std::sort(farPieces.begin(), farPieces.end(), [](const Piece & a, const Piece & b) {
                return a.end > b.end;
            });
            pieces = nearPieces;
            pieces.insert(pieces.end(), farPieces.begin(), farPieces.end());
        };
        if (!req.mediaPath.empty()) {
            planPieces();
        }
        if (!pieces.empty()) {
            if (!openSampler()) {
                pieces.clear();
            }
            if (m_sampler) {
                const int64_t duration = m_sampler->GetDurationMs();
                if (duration > 0 && sceneEnd > duration) {
                    sceneEnd = std::max(pos, duration);
                    planPieces();
                }
            }
        }
        if (cancel) {
            return cancelled();
        }

        commit(false);
        trackCurrent();
        buildResult();

        if (!pieces.empty()) {
            sendPartial();
            int64_t total = 0, done = 0;
            for (const auto& p : pieces) {
                total += (p.end - p.start) / p.step + 1;
            }
            const ULONGLONG deadline = GetTickCount64() + kSceneBudgetMs;
            bool stopped = false;
            for (size_t pi = 0; pi < pieces.size() && !stopped; pi++) {
                const Piece piece = pieces[pi];
                // decoding and face analysis run in parallel
                std::mutex qLock;
                std::condition_variable qCv;
                std::deque<std::pair<int64_t, ImageBGR>> queue;
                bool producerDone = false;
                std::atomic<bool> stop{ false };
                std::thread producer([&] {
                    m_sampler->Sample(piece.start, piece.end, piece.step, kSampleWidth, [&](int64_t t, ImageBGR & img) {
                        std::unique_lock<std::mutex> l(qLock);
                        qCv.wait(l, [&] { return queue.size() < 3 || stop; });
                        if (stop) {
                            return false;
                        }
                        queue.emplace_back(t, std::move(img));
                        qCv.notify_all();
                        return true;
                    }, stop);
                    std::lock_guard<std::mutex> l(qLock);
                    producerDone = true;
                    qCv.notify_all();
                });
                int64_t last = -1;
                for (;;) {
                    std::pair<int64_t, ImageBGR> item;
                    {
                        std::unique_lock<std::mutex> l(qLock);
                        qCv.wait(l, [&] { return !queue.empty() || producerDone; });
                        if (queue.empty()) {
                            break;
                        }
                        item = std::move(queue.front());
                        queue.pop_front();
                        qCv.notify_all();
                    }
                    if (cancel || GetTickCount64() > deadline) {
                        std::lock_guard<std::mutex> l(qLock);
                        stop = true;
                        qCv.notify_all();
                        break;
                    }
                    analyzeFrame(item.first, item.second, false);
                    last = item.first;
                    done++;
                    progress(L"Analyzing the scene\u2026 " + std::to_wstring(std::min<int64_t>(99, done * 100 / std::max<int64_t>(1, total))) + L"%");
                }
                producer.join();
                if (!stop) {
                    timeline.AddCovered(piece.start, piece.end);
                } else {
                    stopped = true;
                    if (last >= piece.start) {
                        timeline.AddCovered(piece.start, last);
                    }
                }
                if (cancel) {
                    commit(false); // keep what was analyzed, the next request continues from there
                    return cancelled();
                }
                commit(false);
                trackCurrent();
                buildResult();
                if (pi + 1 < pieces.size() && !stopped) {
                    sendPartial();
                }
            }
            if (stopped) {
                result->sceneNote = L"Only part of the scene could be analyzed in time.";
            }
        }

        // final naming, also remembers new views of the actors for the next scenes
        progress(L"Recognizing actors\u2026");
        commit(true);
        trackCurrent();
        buildResult();
        result->sceneAnalyzed = timeline.Uncovered(sceneStart, sceneEnd, kFarStepMs).empty();
        result->fromCache = pieces.empty() && result->sceneAnalyzed;
        if (result->facesDetected < 0 && !result->sceneAnalyzed && result->sceneNote.empty()) {
            result->message = L"The current video frame couldn't be captured with this video renderer.";
        }
        finishDetails();
        if (cancel) {
            return cancelled();
        }
        result->status = Result::Status::Ok;
        return result;
    }
}
