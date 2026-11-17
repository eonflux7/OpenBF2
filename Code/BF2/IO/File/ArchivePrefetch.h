// [PERF] Archive prefetch: worker threads inflate a level's archive entries ahead of the main thread.
//
// Serially, every read open of an archive entry seeks the archive file and inflates on the main
// thread: entries up to 64 KiB inside ZipArchive::open, larger ones as the caller reads the stream.
// On a PR level load that was about 2 s of opens (54,000) and 2.2 s of texture reads, plus mesh and
// sound reads, all inflate.
//
// A load records, in order, each archive entry it opens for the first time (archive file and entry
// name) and writes the list beside the effect cache, one file per level, mode and size. The next
// load of that level reads the list when its archives are mounted and hands the entries to worker
// threads, each with its own handles on the archive files. ZipArchive::open then takes the
// inflated bytes instead of inflating them: a MemStream for small entries, as before, and a
// PrefetchedZipStream (ZipArchive.cpp) over the buffer for large ones. An entry a worker has not
// started is read by the main thread as before; one in progress is waited for. The bytes are the
// same entry the serial path would read (archive file, position and sizes are compared), so the
// loaded state does not change. Workers stay at most a fixed number of bytes ahead of the main
// thread, and entries the main thread has passed are dropped.

#ifndef IO_FILE_ARCHIVEPREFETCH_H
#define IO_FILE_ARCHIVEPREFETCH_H

#include <memory>
#include <string>
#include <vector>

#include "unzip.h"

namespace dice {
namespace hfe {
namespace io {

class ZipArchive;

// Main thread. Begin ends any previous prefetch, reads manifestPath (an @HOME@ path), queues its
// entries and starts recording. End writes the recorded list to the manifest and stops the workers.
void archivePrefetchBegin(const std::string& manifestPath);
void archivePrefetchEnd();

// ZipArchive bookkeeping (main thread): mounted archives by file name.
void archivePrefetchRegister(ZipArchive* archive, const std::string& filename);
void archivePrefetchUnregister(ZipArchive* archive);

// ZipArchive::open: note an entry opened for reading; take its inflated bytes if a worker has them.
void archivePrefetchRecord(const std::string& archive, const std::string& entry);
std::unique_ptr<std::vector<char> > archivePrefetchTake(const std::string& archive,
                                                        const std::string& entry,
                                                        const unz_file_info& info,
                                                        const unz_file_pos& pos);

} // namespace io
} // namespace hfe
} // namespace dice

#endif // IO_FILE_ARCHIVEPREFETCH_H
