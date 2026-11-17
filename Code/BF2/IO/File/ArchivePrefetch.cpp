// [PERF] Archive prefetch; see ArchivePrefetch.h.

#include "ArchivePrefetch.h"

#include "FileManager.h"
#include "ZipArchive.h"
#include "../../Core/LoadTrace.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace dice {
namespace hfe {
namespace io {

namespace {

// How far, in inflated bytes, the workers may run ahead of the main thread.
const unsigned long long kBudgetBytes = 256ull << 20;
// Entries this many places behind the latest one taken are dropped (the main thread has passed
// them: the load's order differs from the recorded one there).
const std::size_t kDropBehind = 512;

enum JobState { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_GONE };

struct Job
{
    std::string                          archive;    // archive file name
    std::string                          key;        // archive + '\n' + entry
    std::string                          entry;
    unz_file_info                        info;
    unz_file_pos                         pos;
    JobState                             state;
    bool                                 drop;       // passed while a worker ran it
    std::unique_ptr<std::vector<char> >  data;       // null after a failed read
};

struct Pool
{
    std::mutex                               mutex;
    std::condition_variable                  work;       // a job queued, room in the budget, stop
    std::condition_variable                  finished;   // a job done
    std::vector<Job>                         jobs;       // in recorded order
    std::unordered_map<std::string, size_t>  index;      // key -> job, main thread only
    std::size_t                              next;       // first job a worker may not have taken
    std::size_t                              dropped;    // jobs below this are gone
    unsigned long long                       ahead;      // bytes done and not yet taken
    bool                                     stop;
    std::vector<std::thread>                 threads;

    Pool() : next(0), dropped(0), ahead(0), stop(false) {}
};

Pool* g_pool = 0;

// Recording (main thread).
bool                            g_recording = false;
std::string                     g_manifestPath;
std::vector<std::string>        g_recorded;      // "archive\tentry", first opens in order
std::unordered_set<std::string> g_recordedKeys;

std::map<ZipArchive*, std::string> g_archives;   // mounted ZipArchives (main thread)

std::string jobKey(const std::string& archive, const std::string& entry)
{
    std::string key(archive);
    key += '\n';
    key += entry;
    return key;
}

void dropBelow(Pool& pool, std::size_t limit)
{
    for (; pool.dropped < limit && pool.dropped < pool.jobs.size(); ++pool.dropped)
    {
        Job& job = pool.jobs[pool.dropped];
        if (job.state == JOB_QUEUED)
            job.state = JOB_GONE;
        else if (job.state == JOB_DONE)
        {
            if (job.data)
                pool.ahead -= job.data->size();
            job.data.reset();
            job.state = JOB_GONE;
        }
        else if (job.state == JOB_RUNNING)
            job.drop = true;                        // its worker discards the bytes
    }
}

void worker(Pool* pool)
{
    std::map<std::string, unzFile> handles;
    for (;;)
    {
        Job* job = 0;
        {
            std::unique_lock<std::mutex> lock(pool->mutex);
            for (;;)
            {
                if (pool->stop)
                    break;
                while (pool->next < pool->jobs.size() && pool->jobs[pool->next].state != JOB_QUEUED)
                    ++pool->next;
                if (pool->next >= pool->jobs.size())
                    break;
                if (pool->ahead < kBudgetBytes)
                {
                    job = &pool->jobs[pool->next++];
                    job->state = JOB_RUNNING;
                    break;
                }
                pool->work.wait(lock);
            }
        }
        if (!job)
            break;

        std::unique_ptr<std::vector<char> > data;
        {
            loadtrace::Scope trace("file.prefetch", job->entry.c_str());   // [DEV] BF2_LOAD_TRACE
            unzFile& handle = handles[job->archive];
            if (!handle)
                handle = unzOpen(job->archive.c_str());
            const unsigned int size = static_cast<unsigned int>(job->info.uncompressed_size);
            if (handle && unzGoToFilePos(handle, &job->pos) == UNZ_OK
                && unzOpenCurrentFile(handle) == UNZ_OK)
            {
                data.reset(new std::vector<char>(size));
                const int got = size ? unzReadCurrentFile(handle, &(*data)[0], size) : 0;
                unzCloseCurrentFile(handle);
                if (got != static_cast<int>(size))
                    data.reset();
                else
                    trace.addBytes(size);
            }
        }

        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            if (job->drop)
                job->state = JOB_GONE;
            else
            {
                if (data)
                    pool->ahead += data->size();
                job->data = std::move(data);
                job->state = JOB_DONE;
            }
        }
        pool->finished.notify_all();
    }
    for (std::map<std::string, unzFile>::iterator it = handles.begin(); it != handles.end(); ++it)
        if (it->second)
            unzClose(it->second);
}

void stopPool()
{
    if (!g_pool)
        return;
    {
        std::lock_guard<std::mutex> lock(g_pool->mutex);
        g_pool->stop = true;
    }
    g_pool->work.notify_all();
    for (std::size_t i = 0; i < g_pool->threads.size(); ++i)
        g_pool->threads[i].join();
    delete g_pool;
    g_pool = 0;
}

bool readManifest(const std::string& path, std::string& text)
{
    if (!fileManager)
        return false;
    IStream* const in = fileManager->open(path, FILEMODE_READ, static_cast<FileAccess>(0), 0);
    if (!in)
        return false;
    char chunk[65536];
    for (;;)
    {
        const unsigned int got = in->read(chunk, sizeof chunk);
        if (!got)
            break;
        text.append(chunk, got);
    }
    in->release();
    return true;
}

} // namespace

void archivePrefetchBegin(const std::string& manifestPath)
{
    stopPool();
    g_recording = true;
    g_manifestPath = manifestPath;
    g_recorded.clear();
    g_recordedKeys.clear();

    loadtrace::Scope trace("file.prefetch.queue", manifestPath.c_str());   // [DEV] BF2_LOAD_TRACE
    std::string text;
    if (!readManifest(manifestPath, text))
        return;

    std::map<std::string, ZipArchive*> byName;
    for (std::map<ZipArchive*, std::string>::iterator it = g_archives.begin(); it != g_archives.end(); ++it)
        byName[it->second] = it->first;

    Pool* const pool = new Pool;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const std::size_t tab = text.find('\t', start);
        if (tab != std::string::npos && tab < end)
        {
            const std::string archive(text, start, tab - start);
            const std::string entry(text, tab + 1, end - tab - 1);
            std::map<std::string, ZipArchive*>::iterator arc = byName.find(archive);
            ZipArchive::Entry* const found = arc == byName.end() ? 0 : arc->second->findEntry(entry);
            if (found)
            {
                const std::string key = jobKey(archive, entry);
                if (pool->index.find(key) == pool->index.end())
                {
                    pool->index[key] = pool->jobs.size();
                    pool->jobs.push_back(Job());
                    Job& job = pool->jobs.back();
                    job.archive = archive;
                    job.key = key;
                    job.entry = entry;
                    job.info = found->info;
                    job.pos = found->pos;
                    job.state = JOB_QUEUED;
                    job.drop = false;
                }
            }
        }
        start = end + 1;
    }
    trace.addBytes(static_cast<long long>(pool->jobs.size()));
    if (pool->jobs.empty())
    {
        delete pool;
        return;
    }

    g_pool = pool;
    unsigned int threads = std::thread::hardware_concurrency();
    threads = threads > 2 ? threads / 2 : 1;
    if (threads > 4)
        threads = 4;
    for (unsigned int i = 0; i < threads; ++i)
        pool->threads.push_back(std::thread(worker, pool));
}

void archivePrefetchEnd()
{
    loadtrace::Scope trace("file.prefetch.end", g_manifestPath.c_str());   // [DEV] BF2_LOAD_TRACE
    stopPool();
    if (!g_recording)
        return;
    g_recording = false;
    if (fileManager && !g_recorded.empty())
    {
        IStream* const out = fileManager->open(g_manifestPath, static_cast<FileMode>(0),
                                               static_cast<FileAccess>(2), 0);
        if (out)
        {
            std::string text;
            for (std::size_t i = 0; i < g_recorded.size(); ++i)
            {
                text += g_recorded[i];
                text += '\n';
            }
            out->write(text.data(), static_cast<unsigned int>(text.size()));
            out->release();
        }
    }
    trace.addBytes(static_cast<long long>(g_recorded.size()));
    g_recorded.clear();
    g_recordedKeys.clear();
}

void archivePrefetchRegister(ZipArchive* archive, const std::string& filename)
{
    g_archives[archive] = filename;
}

void archivePrefetchUnregister(ZipArchive* archive)
{
    g_archives.erase(archive);
}

void archivePrefetchRecord(const std::string& archive, const std::string& entry)
{
    if (!g_recording || archive.find('\t') != std::string::npos
        || entry.find('\t') != std::string::npos || entry.find('\n') != std::string::npos)
        return;
    std::string key = jobKey(archive, entry);
    if (!g_recordedKeys.insert(key).second)
        return;
    key[archive.size()] = '\t';
    g_recorded.push_back(key);
}

std::unique_ptr<std::vector<char> > archivePrefetchTake(const std::string& archive,
                                                        const std::string& entry,
                                                        const unz_file_info& info,
                                                        const unz_file_pos& pos)
{
    std::unique_ptr<std::vector<char> > none;
    if (!g_pool)
        return none;
    std::unordered_map<std::string, size_t>::iterator it = g_pool->index.find(jobKey(archive, entry));
    if (it == g_pool->index.end())
        return none;
    const std::size_t at = it->second;
    g_pool->index.erase(it);

    std::unique_lock<std::mutex> lock(g_pool->mutex);
    Job& job = g_pool->jobs[at];
    if (job.state == JOB_QUEUED)
        job.state = JOB_GONE;                       // read it here, as before
    else if (job.state == JOB_RUNNING)
    {
        loadtrace::Scope wait("file.wait", job.entry.c_str());   // [DEV] BF2_LOAD_TRACE
        g_pool->finished.wait(lock, [&job] { return job.state != JOB_RUNNING; });
    }

    std::unique_ptr<std::vector<char> > data;
    if (job.state == JOB_DONE)
    {
        data = std::move(job.data);
        job.state = JOB_GONE;
        if (data)
            g_pool->ahead -= data->size();
        // The same entry the serial path would read: position and sizes as mounted now.
        if (data && (job.pos.pos_in_zip_directory != pos.pos_in_zip_directory
                     || job.pos.num_of_file != pos.num_of_file
                     || job.info.uncompressed_size != info.uncompressed_size
                     || job.info.compressed_size != info.compressed_size
                     || job.info.crc != info.crc))
            data.reset();
    }
    if (at >= kDropBehind)
        dropBelow(*g_pool, at - kDropBehind);
    lock.unlock();
    g_pool->work.notify_all();
    return data;
}

} // namespace io
} // namespace hfe
} // namespace dice
