/*
 * wss-v3.c	Estimate the working set size (WSS) for a cgroupv2 on Linux.
 *		Version 3: uses cgroups instead of processes.
 *
 * This is a proof of concept that uses idle page tracking from Linux 4.3+, for
 * a page-based WSS estimation. This version snapshots the entire system's idle
 * page flags, which is efficient for analyzing large processes, but not tiny
 * processes. For those, see wss-v1.c. There is also wss.pl, which uses can be
 * over 10x faster and works on older Linux, however, uses the referenced page
 * flag and has its own caveats. These tools can be found here:
 *
 * http://www.brendangregg.com/wss.pl
 *
 * Currently written for x86_64 and default page size only. Early version:
 * probably has bugs.
 *
 * COMPILE: gcc -o wss-v1 wss-v1.c
 *
 * REQUIREMENTS: Linux 4.3+
 *
 * USAGE: wss-v3 CGROUP_PATH duration(s)
 *
 * COLUMNS:
 *	- Est(s):  Estimated WSS measurement duration: this accounts for delays
 *	           with setting and reading pagemap data, which inflates the
 *	           intended sleep duration.
 *	- Ref(MB): Referenced (Mbytes) during the specified duration.
 *	           This is the working set size metric.
 *
 * WARNING: This tool sets and reads system and process page flags, which can
 * take over one second of CPU time, during which application may experience
 * slightly higher latency (eg, 5%). Consider these overheads. Also, this is
 * activating some new kernel code added in Linux 4.3 that you may have never
 * executed before. As is the case for any such code, there is the risk of
 * undiscovered kernel panics (I have no specific reason to worry, just being
 * paranoid). Test in a lab environment for your kernel versions, and consider
 * this experimental: use at your own risk.
 *
 * Copyright 2018 Netflix, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License")
 *
 * 13-Jan-2018	Brendan Gregg	Created this.
 *
 * 9-Nov-2025 Alan Maguire modified to use cgroups
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

// see Documentation/vm/pagemap.txt:
#define PFN_MASK		(~(0x1ffLLU << 55))

#define IDLEMAP_CHUNK_SIZE	8
#define IDLEMAP_BUF_SIZE	4096

// big enough to span 740 Gbytes:
#define MAX_IDLEMAP_SIZE	(20 * 1024 * 1024)

// from mm/page_idle.c:
#ifndef BITMAP_CHUNK_SIZE
#define BITMAP_CHUNK_SIZE	8
#endif

#define KPAGECGROUP_CHUNK_SIZE	8192

// globals
int g_debug = 0;		// 1 == some, 2 == verbose
int g_quiet = 0;
int g_activepages = 0;
int g_totalpages = 0;
int g_walkedpages = 0;
char *g_idlepath = "/sys/kernel/mm/page_idle/bitmap";
char *g_kpagecgrouppath = "/proc/kpagecgroup";
unsigned long long *g_idlebuf;
unsigned long long g_idlebufsize;

const char *g_cgroup;

/*
 * This code must operate on bits in the pageidle bitmap and uses inode
 * associated with cgroupv2 path to match pfns to the cgroup.
 * Doing this one by one via syscall read/write on a large process can take too
 * long, eg, 7 minutes for a 130 Gbyte process. Instead, I copy (snapshot) the
 * idle bitmap and pagemap into our memory with the fewest syscalls allowed,
 * and then process them with load/stores. Much faster, at the cost of some memory.
 */

int cgroupidle(unsigned long long inode)
{
	unsigned long long inodebuf[KPAGECGROUP_CHUNK_SIZE] = {};
	unsigned long long idlemapp, idlebits, pfn = 0;
	int i, pagefd, err = 0, bytes_read, inodes_read;

	// open pagemap for virtual to PFN translation
	if ((pagefd = open(g_kpagecgrouppath, O_RDONLY)) < 0) {
		perror("Can't read pagemap file");
		return 2;
	}

	while ((bytes_read = read(pagefd, inodebuf, sizeof(inodebuf))) > 0) {
		inodes_read = bytes_read/8;
		for (i = 0; i < inodes_read; i++, pfn++) {
			if (inodebuf[i] != inode)
				continue;
			g_totalpages++;
			// read idle bit
			idlemapp = (pfn / 64) * BITMAP_CHUNK_SIZE;
			if (idlemapp > g_idlebufsize) {
				printf("ERROR: bad PFN read from page map.\n");
				err = 1;
				goto out;
			}
			idlebits = g_idlebuf[idlemapp];
			if (g_debug > 1) {
				fprintf(stderr, "R: pfn %llx idlebits %llx\n",
					pfn, idlebits);
			}

			if (!(idlebits & (1ULL << (pfn % 64)))) {
				g_activepages++;
			}
		}
		g_walkedpages += i;
	}
	if (bytes_read < 0)
		err = -errno;
out:
	close(pagefd);

	return err;
}

int setidlemap()
{
	char *p;
	int idlefd, i;
	// optimized: large writes allowed here:
	char buf[IDLEMAP_BUF_SIZE];

	for (i = 0; i < sizeof (buf); i++)
		buf[i] = 0xff;

	// set entire idlemap flags
	if ((idlefd = open(g_idlepath, O_WRONLY)) < 0) {
		perror("Can't write idlemap file");
		exit(2);
	}
	// only sets user memory bits; kernel is silently ignored
	while (write(idlefd, &buf, sizeof(buf)) > 0) {;}

	close(idlefd);

	return 0;
}

int loadidlemap()
{
	unsigned long long *p;
	int idlefd;
	ssize_t len;

	if ((g_idlebuf = malloc(MAX_IDLEMAP_SIZE)) == NULL) {
		printf("Can't allocate memory for idlemap buf (%d bytes)",
		    MAX_IDLEMAP_SIZE);
		exit(1);
	}

	// copy (snapshot) idlemap to memory
	if ((idlefd = open(g_idlepath, O_RDONLY)) < 0) {
		perror("Can't read idlemap file");
		exit(2);
	}
	p = g_idlebuf;
	// unfortunately, larger reads do not seem supported
	while ((len = read(idlefd, p, IDLEMAP_CHUNK_SIZE)) > 0) {
		p += IDLEMAP_CHUNK_SIZE;
		g_idlebufsize += len;
	}
	close(idlefd);

	return 0;
}

int main(int argc, char *argv[])
{
	double duration, mbytes;
	static struct timeval ts1, ts2, ts3, ts4;
	unsigned long long set_us, read_us, dur_us, slp_us, est_us;
	struct stat stat;
	unsigned long long inode;
	int first = 1, forever = 0;
	int fd;

	// options
	if (argc < 3) {
		fprintf(stderr, "USAGE: %s CGROUP_PATH duration(s) [forever]\n",
			argv[0]);
		exit(1);
	}
	if (getenv("DEBUG"))
		g_debug = atoi(getenv("DEBUG"));
	if (getenv("QUIET"))
		g_quiet = 1;
	if (argc >= 4)
		forever = strcmp(argv[3], "forever") == 0;
	g_cgroup = argv[1];
	fd = open(g_cgroup, O_RDONLY);
	if (fd < 0) {
		perror("could not open cgroup dir");
		exit(1);
	}
	if (fstat(fd, &stat) < 0) {
		perror("could not fstats cgroup dir\n");
		close(fd);
		exit(1);
	}
	inode = (unsigned long long)stat.st_ino;
	close(fd);
	duration = atof(argv[2]);
	if (duration < 0.01) {
		printf("Interval too short. Exiting.\n");
		return 1;
	}
	if (!g_quiet)
		printf("Watching '%s'(inode %lu) page references during %.2f seconds...\n",
		    g_cgroup, inode, duration);

again:
	g_activepages = 0;
	g_walkedpages = 0;
	g_totalpages = 0;
	// set idle flags
	gettimeofday(&ts1, NULL);
	setidlemap();

	// sleep
	gettimeofday(&ts2, NULL);
	usleep((int)(duration * 1000000));
	gettimeofday(&ts3, NULL);

	// read idle flags
	loadidlemap();
	if (cgroupidle(inode) < 0) {
		fprintf(stderr, "Error reading idle flags for cgroup.\n");
		exit(1);
	}

	gettimeofday(&ts4, NULL);

	// calculate times
	set_us = 1000000 * (ts2.tv_sec - ts1.tv_sec) +
	    (ts2.tv_usec - ts1.tv_usec);
	slp_us = 1000000 * (ts3.tv_sec - ts2.tv_sec) +
	    (ts3.tv_usec - ts2.tv_usec);
	read_us = 1000000 * (ts4.tv_sec - ts3.tv_sec) +
	    (ts4.tv_usec - ts3.tv_usec);
	dur_us = 1000000 * (ts4.tv_sec - ts1.tv_sec) +
	    (ts4.tv_usec - ts1.tv_usec);
	est_us = dur_us - (set_us / 2) - (read_us / 2);
	if (g_debug) {
		printf("set time  : %.3f s\n", (double)set_us / 1000000);
		printf("sleep time: %.3f s\n", (double)slp_us / 1000000);
		printf("read time : %.3f s\n", (double)read_us / 1000000);
		printf("dur time  : %.3f s\n", (double)dur_us / 1000000);
		// assume getpagesize() sized pages:
		printf("referenced: %d pages, %d Kbytes\n", g_activepages,
		    g_activepages * getpagesize());
		printf("walked    : %d pages, %ld Kbytes\n", g_walkedpages,
		    g_walkedpages * getpagesize());
	}

	// assume getpagesize() sized pages:
	mbytes = (g_activepages * getpagesize()) / (1024 * 1024);
	if (first && !g_quiet) {
		printf("%-7s %10s %20s %20s\n", "Est(s)", "Ref(MB)", "Ref(Pages)", "Total(Pages)");
		first = 0;
	}
	printf("%-7.3f %10.2f %20lu %20lu\n", (double)est_us / 1000000, mbytes, g_activepages, g_totalpages);

	if (forever)
		goto again;
	return 0;
}
