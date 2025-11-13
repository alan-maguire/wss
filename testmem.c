#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[])
{
	unsigned long i, accesspattern, accessed, numpages, set, unset;
	int delay = 0, iters = -1, repeat = 0;
	int pagesize = getpagesize();
	struct timeval ts1, ts2;
	unsigned long dur_us;
	char *mem = NULL;
	int quiet = 0;
	int debug = 0;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s numpages access_pattern [delay(sec)] [iters]\n",
			argv[0]);
		return 1;
	}
	debug = getenv("DEBUG") != NULL;
	quiet = getenv("QUIET") != NULL;
	numpages = atoi(argv[1]);
	accesspattern = atoi(argv[2]);
	if (argc > 3) {
		repeat = 1;
		delay = atoi(argv[3]);
	}
	if (argc > 4)
		iters = atoi(argv[4]);

	mem = mmap(NULL, pagesize * numpages, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_LOCKED, -1, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "cannot allocate %d pages (%s); exiting\n",
			numpages, strerror(errno));
		return 1;
	}

	if (!quiet)
		printf("%10s %20s %20s %20s\n", "Est(us)", "PagesAccessed", "Set", "Unset");
again:
	accessed = set = unset = 0;
	gettimeofday(&ts1, NULL);
	for (i = 0; i < numpages; i+= accesspattern) {
		if (!mem[(i * pagesize)]) {
			mem[(i * pagesize)]++;
			set++;
		} else {
			mem[(i * pagesize)]--;
			unset++;
		}
		accessed++;
	}
	gettimeofday(&ts2, NULL);
	dur_us = 1000000 * (ts2.tv_sec - ts1.tv_sec) +
		(ts2.tv_usec - ts1.tv_usec);

	if (!quiet)
		printf("%10lu %20lu %20lu %20lu\n", dur_us, accessed, set, unset);
	if (repeat && (iters == -1 || --iters > 0)) {
		sleep(delay);
		goto again;
	}
	return 0;
}
