#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "bptree.h"

struct key_value {
	unsigned char len;
	char key[64];
	unsigned long long value;
};

struct bench_option {
	unsigned int page_bits;
	int rounds;
	int read;
	int random;
};

static void usage();

struct bench_option opts = {
	12,
	50000,
	1,
	0
};

int main(int argc, char *argv[])
{
	int rc = 0;
	int ch = 0;
	bpt_handle h = NULL;
	int i;
	struct timespec start, stop;
	double time;
	struct key_value *kv = NULL;

	while ((ch = getopt(argc, argv, "p:n:o:rh")) != -1) {
		switch (ch) {
		case 'p':
			opts.page_bits = atoi(optarg);
			break;
		case 'n':
			opts.rounds = atoi(optarg);
			break;
		case 'o':
			if (strcmp(optarg, "read") == 0) {
				opts.read = 1;
			} else if (strcmp(optarg, "write") == 0) {
				opts.read = 0;
			} else {
				fprintf(stderr, "Illegal operation:%s\n", optarg);
				goto out;
			}
			break;
		case 'r':
			opts.random = 1;
			break;
		case 'h':
		default:
			usage();
			goto out;
		}
	}

	/* Create/Open database */
	h = bpt_open("bpt.dat", opts.page_bits);
	if (h == NULL) {
		fprintf(stderr, "Failed to create/open bplustree!\n");
		goto out;
	}

	kv = malloc(opts.rounds * sizeof(struct key_value));
	if (kv == NULL) {
		fprintf(stderr, "Failed to allocate key value buffer!\n");
		goto out;
	}

	memset(kv, 0, opts.rounds * sizeof(struct key_value));

	/* Fill in keys */
	for (i = 0; i < opts.rounds; i++) {
		kv[i].len = sprintf(kv[i].key, "benchmark_%08d", i);
		kv[i].value = i + 2;
	}

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
	
	/* Start bench */
	if (!opts.random) {
		for (i = 0; i < opts.rounds; i++) {
			rc = bpt_insertkey(h, (unsigned char *)kv[i].key,
					   kv[i].len, 0, kv[i].value);
			if (rc != 0) {
				fprintf(stderr, "Failed to insert key: %s\n", kv[i].key);
				goto out;
			}
		}
	}

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stop);

	time = (stop.tv_sec - start.tv_sec) + (stop.tv_nsec - start.tv_nsec) / 1e9;

	printf("Bench summary: \n");
	printf("Page bits: %d\n", opts.page_bits);
	printf("Number of keys: %d\n", opts.rounds);
	printf("Operation: %s\n", opts.read ? "read" : "write");
	printf("IO pattern: %s\n", opts.random ? "random" : "sequential");
	printf("Elapsed time: %f seconds\n", time);

 out:
	if (h) {
		bpt_close(h);
	}
	return rc;
}

static void usage()
{
	printf("usage: bench [-p <page-bits>] [-n <num-keys>] [-o <read/write>] [-r]\n");
	printf("default options:\n"
	       "\tPage bits      : %d\n"
	       "\tNumber of keys : %d\n"
	       "\tOperation      : %s\n"
	       "\tIO pattern     : %s\n",
	       opts.page_bits, opts.rounds, opts.read ? "read" : "write",
	       opts.random ? "random" : "sequential");
}
