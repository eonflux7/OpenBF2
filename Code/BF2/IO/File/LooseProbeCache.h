// [PERF] Directory listings for loose-file existence and file-information queries.
//
// FileManager::fileExistsEx asks the disk for each name along every search path; a miss cost about
// 65 us under Wine (Win32FileSystem::fileExists, one FindFirstFileW), and a PR level load misses
// some 12,000 times, mostly the per-level texture variants in the Geometries and Textures parts.
// The effect manager asks for the size and write time of each effect source, include and cache
// file (Win32FileSystem::getFileInfo, the same FindFirstFileW), some 9,000 times per hosted load.
//
// From the start of a game (BF2Engine::startGame) and during a level load (GameLogic, until
// endLoadStats), both are answered from a listing of the directory, made once with the same
// FindFirstFileW records. Names whose lookup Windows treats specially (wildcards, short names,
// alternate streams, trailing dots or spaces, root paths, non-ASCII, two entries differing only in
// case) still go to the disk. A name written, deleted, moved or copied through the file system, and
// its directories, go to the disk for the rest of the window, and listings inside it are dropped,
// so a file the load writes, or a stream still being written, is seen as before.
//
// BF2_PERF_PROBECACHE=0 asks the disk every time.

#ifndef IO_FILE_LOOSEPROBECACHE_H
#define IO_FILE_LOOSEPROBECACHE_H

namespace dice {
namespace hfe {
namespace io {

#if defined(_WIN32)
void looseProbeCacheBegin();
void looseProbeCacheEnd();
#else
inline void looseProbeCacheBegin() {}
inline void looseProbeCacheEnd() {}
#endif

} // namespace io
} // namespace hfe
} // namespace dice

#endif // IO_FILE_LOOSEPROBECACHE_H
