/*
 * evict.c - drop a file (and its segment files) from the OS page cache with
 * posix_fadvise(POSIX_FADV_DONTNEED). Used by the read-stream/AIO benchmark to
 * force cold reads from storage without needing /proc/sys/vm/drop_caches.
 *
 * Usage: evict <path> [<path> ...]   (each <path> plus <path>.1, .2, ... )
 * Build: cc -O2 -o evict evict.c
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
evict_one(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return;					/* segment does not exist; stop the caller's loop */
	fdatasync(fd);				/* flush dirty pages so DONTNEED can drop them */
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	close(fd);
}

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
	{
		char seg[4096];
		int n;

		evict_one(argv[i]);		/* base segment */
		for (n = 1; n < 100000; n++)
		{
			int fd;

			snprintf(seg, sizeof(seg), "%s.%d", argv[i], n);
			fd = open(seg, O_RDONLY);
			if (fd < 0)
				break;			/* no more segments */
			fdatasync(fd);
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
			close(fd);
		}
	}
	return 0;
}
