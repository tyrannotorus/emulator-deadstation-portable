#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <ctype.h>
#include <fcntl.h>
#include <cinttypes>

#include "Common/Log.h"
#include "Common/File/Path.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/TimeUtil.h"


bool free_disk_space(const Path &path, int64_t &space) {

	struct statvfs diskstat;
	int res = statvfs(path.c_str(), &diskstat);

	if (res == 0) {
		// Not sure why we're excluding Android here anyway...
		if (diskstat.f_flag & ST_RDONLY) {
			// No space to write.
			space = 0;
			return true;
		}
		space = (uint64_t)diskstat.f_bavail * (uint64_t)diskstat.f_frsize;
		return true;
	}

	// We can't know how much is free.
	return false;
}
