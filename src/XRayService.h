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

// X-Ray: "who is in this scene" for the paused position.
//  1. the movie/episode is identified from the file name
//  2. its cast is fetched from TMDb (when an API key is configured) or Wikidata (free, no key)
//  3. faces in the paused frame and in frames sampled around it (the whole scene, all camera angles) are
//     detected, grouped per person across frames and compared, offline, against the cast headshots only
//  4. matched actors are enriched with birth date, birth place and other movies
// Everything that comes from the network or is computed (cast lists, people, headshots, face embeddings,
// a timeline of who appears when) is cached on disk so the work is done only once.
// This file deliberately has no MFC dependency.

#include "XRayFaceEngine.h"
#include "XRayVideoSampler.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace XRay
{
    struct KnownFor {
        std::wstring title;
        int year = 0;
    };

    struct Person {
        std::string id;             // provider qualified, e.g. "tmdb:31" or "wd:Q2263"
        std::wstring name;
        std::wstring character;
        std::string imageUrl;
        std::wstring imagePath;     // local cached headshot, empty if none
        std::wstring birthday;      // yyyy-mm-dd, may be partial or empty
        std::wstring deathday;
        std::wstring birthplace;
        std::vector<KnownFor> knownFor;
        bool detailsLoaded = false;
        int order = 0;
    };

    struct MediaQuery {
        std::wstring title;
        int year = 0;
        int season = -1;
        int episode = -1;

        bool IsEpisode() const { return season >= 0 && episode >= 0; }
    };

    // Extracts title/year/season/episode from a release style file name,
    // e.g. "The.Dark.Knight.2008.1080p.BluRay.x264-GRP.mkv" -> "The Dark Knight" (2008)
    MediaQuery ParseMediaName(const std::wstring& fileName);

    struct Movie {
        std::string key;
        std::string provider;       // "tmdb" or "wikidata"
        std::string providerId;
        std::wstring title;
        int year = 0;
        bool isEpisode = false;
        int season = -1;
        int episode = -1;
        std::vector<Person> cast;
        bool charactersChecked = false; // character names were completed from Wikipedia (Wikidata titles only)
    };

    struct SceneMatch {
        int castIndex = -1;         // index into Movie::cast
        float score = 0;
        float x = 0, y = 0, w = 0, h = 0; // face rectangle relative to the frame (0..1), when onScreen
        bool onScreen = false;      // in the current shot, around the paused position
        int appearances = 0;        // number of analyzed frames of the scene showing this person
        int64_t firstMs = 0, lastMs = 0;
    };

    struct Result {
        enum class Status { Ok, MovieNotFound, Error, Cancelled };

        Status status = Status::Error;
        std::wstring message;
        std::shared_ptr<Movie> movie;
        std::vector<SceneMatch> scene; // recognized people: on screen now (left to right), then the rest of the scene
        int facesDetected = 0;      // faces in the paused frame, -1 if it couldn't be captured
        bool recognitionAvailable = false;
        bool fromCache = false;
        bool sceneAnalyzed = false; // frames around the paused position were analyzed as well
        int framesAnalyzed = 0;     // frames of the scene known to the timeline
        int64_t sceneStartMs = 0, sceneEndMs = 0;
        std::wstring sceneNote;     // why the scene couldn't be analyzed, if so
    };

    struct Request {
        std::wstring mediaName;     // file name or stream title
        std::wstring mediaPath;     // local file used to sample the scene, empty for streams/discs
        int64_t positionMs = 0;
        ImageBGR frame;
        std::string tmdbApiKey;     // optional, v3 API key or v4 read access token
        bool allowEngineDownload = false;
    };

    class Service
    {
    public:
        // cacheDir: writable folder for all X-Ray data; appDir: folder searched for onnxruntime.dll and the models
        Service(const std::wstring& cacheDir, const std::wstring& appDir);
        ~Service();
        Service(const Service&) = delete;
        Service& operator=(const Service&) = delete;

        // Folders searched, in order, for the FFmpeg DLLs (*-lav-*.dll) used for scene analysis.
        // Default: appDir\LAVFilters64 (LAVFilters on 32-bit). Call before Start.
        void SetVideoLibraryDirs(std::vector<std::wstring> dirs);

        // Starts processing asynchronously. While running, PostMessage(hNotify, msg, kNotifyProgress, 0) is sent
        // whenever GetProgress() changes, PostMessage(hNotify, msg, kNotifyPartial, 0) when TakePartialResult()
        // has a preliminary result (e.g. the paused frame only, while the rest of the scene is analyzed) and
        // PostMessage(hNotify, msg, kNotifyDone, 0) once TakeResult() is ready.
        void Start(Request request, HWND hNotify, UINT msg);
        void Cancel();
        bool IsBusy() const;
        std::wstring GetProgress() const;
        std::shared_ptr<Result> TakePartialResult();
        std::shared_ptr<Result> TakeResult();

        // True if onnxruntime.dll and both models are available (possibly not loaded yet)
        bool IsEngineInstalled() const;
        // Approximate download size of the missing engine components, in MB
        int GetEngineDownloadSizeMB() const;
        // Fills birthday, birth place and other movies of a caller owned Person (blocking, cached on disk).
        // Thread safe; currentProviderId is excluded from the "known for" list.
        bool LoadPersonDetails(Person& person, const std::string& currentProviderId, const std::string& tmdbApiKey);
        // Synchronous processing, used by the worker thread. partial (optional) receives preliminary results.
        std::shared_ptr<Result> Process(const Request& request, const std::function<void(const std::wstring&)>& progress, const std::atomic<bool>& cancel,
                                        const std::function<void(std::shared_ptr<Result>)>& partial = nullptr);

        const std::wstring& GetCacheDir() const { return m_cacheDir; }

        static constexpr WPARAM kNotifyProgress = 0;
        static constexpr WPARAM kNotifyDone = 1;
        static constexpr WPARAM kNotifyPartial = 2;

    private:
        struct Job;
        struct Timeline;
        struct Observation;
        struct EnginePaths {
            std::wstring runtime, detector, recognizer;
        };

        EnginePaths FindEngine() const;
        bool InstallEngine(const std::function<void(const std::wstring&)>& progress, const std::atomic<bool>& cancel, std::wstring& error);
        bool EnsureEngine(bool allowDownload, const std::function<void(const std::wstring&)>& progress, const std::atomic<bool>& cancel, std::wstring& error);

        std::shared_ptr<Movie> ResolveMovie(const MediaQuery& q, const std::string& tmdbKey, const std::atomic<bool>& cancel, std::wstring& error);
        bool FetchTmdbMovie(const MediaQuery& q, const std::string& key, Movie& movie, std::wstring& error);
        bool FetchWikidataMovie(const MediaQuery& q, Movie& movie, std::wstring& error);
        bool CompleteCastFromWikipedia(Movie& movie);
        bool FetchTmdbPerson(Person& p, const std::string& key, const std::string& currentId);
        bool FetchWikidataPerson(Person& p, const std::string& currentId);

        bool EnsureHeadshot(Person& p);
        const std::vector<FaceEmbedding>& GetReferenceEmbeddings(const Person& p);

        void LoadLearned(const std::string& movieKey);
        void SaveLearned(const std::string& movieKey);
        bool LoadTimeline(const std::string& key, Timeline& timeline);
        void SaveTimeline(const std::string& key, const Timeline& timeline);
        // Groups the faces seen across the scene per person and names each group (sets Observation::castIndex)
        bool IdentifyObservations(const Movie& movie, std::vector<Observation>& obs, const std::vector<std::vector<float>>& castScores,
                                  const std::vector<int>& shotOf, bool learn); // true if learned embeddings changed, call with m_dataLock held

        bool SaveMovie(const Movie& movie);
        bool LoadMovie(const std::string& key, Movie& movie);
        bool SavePerson(const Person& p);
        bool LoadPerson(Person& p);

        bool HttpGet(const std::string& url, const std::vector<std::wstring>& headers, std::string& body, long* status = nullptr);
        bool HttpDownloadFile(const std::string& url, const std::wstring& path, const std::function<bool(uint64_t, uint64_t)>& progress);
        std::vector<std::wstring> TmdbHeaders(const std::string& key) const;
        std::string TmdbUrl(const std::string& path, const std::string& key, const std::string& query = {}) const;

        std::wstring PathFor(const wchar_t* folder, const std::string& id, const wchar_t* ext) const;

        std::wstring m_cacheDir;
        std::wstring m_appDir;
        std::vector<std::wstring> m_videoLibDirs;
        void* m_hInternet = nullptr;
        FaceEngine m_engine;
        bool m_engineTried = false;
        std::wstring m_engineError;

        std::mutex m_dataLock; // protects the caches below and m_movies
        std::map<std::string, std::vector<FaceEmbedding>> m_referenceEmbeddings;
        std::map<std::string, std::map<std::string, std::vector<FaceEmbedding>>> m_learned; // movie -> person -> embeddings
        std::map<std::string, std::shared_ptr<Movie>> m_movies;

        std::unique_ptr<VideoSampler> m_sampler; // kept open between requests on the same file
        std::wstring m_samplerPath;

        std::shared_ptr<Job> m_job;
        mutable std::mutex m_jobLock;
        std::condition_variable m_jobDone;
        int m_runningJobs = 0;
        std::mutex m_processLock; // only one Process() at a time
    };
}
