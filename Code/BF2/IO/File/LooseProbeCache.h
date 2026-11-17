// [PERF] Directory listings for loose-file existence probes during a level load.
//
// FileManager::fileExistsEx asks the disk for each name along every search path; a miss cost about
// 65 us under Wine (Win32FileSystem::fileExists, one FindFirstFileW), and a PR level load misses
// some 12,000 times, mostly the per-level texture variants in the Geometries and Textures parts.
// While a load runs, Win32FileSystem::fileExists lists each directory once and answers from the
// listing. Names whose lookup Windows treats specially (wildcards, short names, trailing dots or
// spaces, drive or root paths, non-ASCII) still go to the disk. Any write, delete, move or copy
// through the file system drops every listing, so a file the load writes is seen as before.
//
// BF2_PERF_PROBECACHE=0 probes the disk every time.

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
