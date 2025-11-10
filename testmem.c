#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

int main(int argc, char *argv[])
{
	int pagesize = getpagesize();
	unsigned long i, accesspattern, accessed, numpages;
	struct timeval ts1, ts2;
	int delay = 0, iters = -1, repeat = 0;
	unsigned long dur_us;
	char *mem = NULL;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s numpages access_pattern [delay(sec)] [iters]\n",
			argv[0]);
		return 1;
	}
	numpages = atoi(argv[1]);
	accesspattern = atoi(argv[2]);
	if (argc > 3) {
		repeat = 1;
		delay = atoi(argv[3]);
	}
	if (argc > 4)
		iters = atoi(argv[4]);

	mem = malloc(pagesize * numpages);
	if (mem == NULL) {
		fprintf(stderr, "cannot allocate %d pages; exiting\n",
			numpages);
		return 1;
	}

	printf("%10s %20s\n", "Est(us)", "PagesAccessed");
again:
	accessed = 0;
	gettimeofday(&ts1, NULL);
	for (i = 0; i < numpages; i+= accesspattern) {
		if (!mem[(i * pagesize)])
			mem[(i * pagesize)]++;
		else
			mem[(i * pagesize)]--;
		accessed++;
	}
	gettimeofday(&ts2, NULL);
	dur_us = 1000000 * (ts2.tv_sec - ts1.tv_sec) +
		(ts2.tv_usec - ts1.tv_usec);

	printf("%10lu %20lu\n", dur_us, accessed);
	if (repeat && (iters == -1 || --iters > 0)) {
		sleep(delay);
		goto again;
	}
	return 0;
}
