#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <semaphore.h>
#include <signal.h>
#include <assert.h>
#include "sysdef.h"
#include "bptree.h"

struct key_value {
	unsigned char len;
	char key[64];
	unsigned long long value;
};

struct bench_option {
	unsigned int node_bits;
	int rounds;
	enum {
		BENCH_OP_READ,
		BENCH_OP_WRITE,
		BENCH_OP_RW,
	} op;
	bool random;
	bool no_cleanup;
	unsigned int cache_capacity;
	unsigned int nr_threads;
	unsigned int nr_processes;
};

struct shm_bench_data {
	unsigned int ready_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	unsigned int index;
	unsigned int nr_kvs;
	struct key_value kvs[0];
};

struct thread_info {
	pthread_t thread;
	struct bpt_mgr *mgr;
	sem_t *sem;
	int op;
	struct shm_bench_data *bench_data;
};

static void usage();
static void print_seperator();
static void do_bench(bptree_t h, struct shm_bench_data *bench_data, int op);

struct bench_option opts = {
	.node_bits = 12,
	.rounds = 64*1024,
	.op = BENCH_OP_WRITE,
	.random = false,
	.no_cleanup = false,
	.cache_capacity = 0,
	.nr_threads = 1,
	.nr_processes = 1
};

/* Per process global data */
bool is_parent = true;
int shmfd = -1;
const char *shm_name = "/bpt_bench";
size_t shm_size = 0;
struct shm_bench_data *bench_data = MAP_FAILED;
const char *bench_sem_name = "/bpt-bench";
sem_t *bench_sem = SEM_FAILED;
struct bpt_mgr *mgr = NULL;

static const char *bench_op_str(int op)
{
	switch (op) {
	case BENCH_OP_READ:
		return "read";
	case BENCH_OP_WRITE:
		return "write";
	case BENCH_OP_RW:
		return "rw";
	default:
		return "unknown";
	}
}

static void dump_options(struct bench_option *options)
{
	printf("Page bits           : %d\n", options->node_bits);
	printf("Number of keys      : %d\n", options->rounds);
	printf("Operation           : %s\n", bench_op_str(options->op));
	printf("IO pattern          : %s\n", options->random ? "random" : "sequential");
	printf("Cache capacity      : %d\n", options->cache_capacity);
	printf("Number of threads   : %d\n", options->nr_threads);
	printf("Number of processes : %d\n", options->nr_processes);
	printf("No clean up         : %s\n", options->no_cleanup ? "true" : "false");
}

static void vperror(const char *fmt, ...)
{
	int old_errno = errno;
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	if (vsnprintf(buf, sizeof(buf), fmt, ap) == -1) {
		buf[sizeof(buf) - 1] = '\0';
	}
	va_end(ap);

	errno = old_errno;

	perror(buf);
}

static void bench_cleanup()
{
	if (mgr) {
		bpt_closemgr(mgr);
	}
	if (bench_sem != SEM_FAILED) {
		sem_close(bench_sem);
		if (is_parent) {
			sem_unlink(bench_sem_name);
		}
	}
	if (shmfd) {
		if (bench_data != MAP_FAILED) {
			munmap(bench_data, shm_size);
		}
		close(shmfd);
		if (is_parent) {
			shm_unlink(shm_name);
		}
	}
}

static void bench_sig_handler(const int sig)
{
	if ((sig != SIGTERM) && (sig != SIGQUIT) &&
	    (sig != SIGINT) && (sig != SIGSEGV)) {
		return;
	}

	bench_cleanup();

	exit(EXIT_FAILURE);
}

static void dump_bpt_iostat(struct bpt_iostat *iostat)
{
	printf("pool maps    : %lld\n", iostat->pool_maps);
	printf("pool unmaps  : %lld\n", iostat->pool_unmaps);
	printf("latch hits   : %lld\n", iostat->latch_hits);
	printf("latch evicts : %lld\n", iostat->latch_evicts);
}

static void *benchmark_thread(void *arg)
{
	int rc;
	struct thread_info *ti;
	struct shm_bench_data *sbd;
	bptree_t h;

	ti = (struct thread_info *)arg;
	sbd = ti->bench_data;

	/* Open a b+tree handle from manager */
	h = bpt_open(ti->mgr);
	if (h == NULL) {
		fprintf(stderr, "Failed to create/open bplustree!\n");
		goto out;
	}

	/* Mutex unlocked if condition signaled */
	rc = pthread_mutex_lock(&sbd->mutex);
	if (rc != 0) {
		fprintf(stderr, "Failed to lock mutex, error:%d\n", rc);
		goto out;
	}

	__sync_add_and_fetch(&sbd->ready_threads, 1);
	
	rc = pthread_cond_wait(&sbd->cond, &sbd->mutex);
	if (rc != 0) {
		fprintf(stderr, "Failed to wait cond, error:%d\n", rc);
		goto out;
	}

	rc = pthread_mutex_unlock(&sbd->mutex);
	if (rc != 0) {
		fprintf(stderr, "Failed to unlock mutex, error:%d\n", rc);
		goto out;
	}

	printf("thread:%ld benchmarking started...\n", gettid());

	do_bench(h, sbd, ti->op);

 out:
	sem_post(ti->sem);
	if (h) {
		bpt_close(h);
	}
	return NULL;
}

static void bench_prepare_data(struct shm_bench_data *sbd,
			       int nr_kvs,
			       bool random)
{
	int i;
	
	/* prepare key/value pairs */
	sbd->nr_kvs = nr_kvs;
	for (i = 0; i < sbd->nr_kvs; i++) {
		sbd->kvs[i].len = sprintf(sbd->kvs[i].key,
					  "benchmark_%08d", i);
		sbd->kvs[i].value = i;
	}

	if (random) {
		int j;
		int x;
		struct key_value temp;
		
		/* Random exchange the position of the key value */
		x = 0;
		srand(time(NULL));
		do {
			i = rand() % nr_kvs;
			j = rand() % nr_kvs;
			temp = sbd->kvs[i];
			sbd->kvs[i] = sbd->kvs[j];
			sbd->kvs[j] = temp;
			x++;
		} while(x < (nr_kvs / 2));
	}
}

static void bench_prefill_data(bptree_t h,
			       struct shm_bench_data *sbd)
{
	int rc;
	struct key_value *kv = NULL;
	int i;
	
	for (i = 0; i < sbd->nr_kvs; i++) {
		kv = &sbd->kvs[i];
		rc = bpt_insertkey(h, (unsigned char *)kv->key,
				   kv->len, 0, kv->value);
		if (rc != 0) {
			fprintf(stderr, "Failed to insert key: %s\n",
				kv->key);
			assert(0);
		}
	}
}

static void bench_cleanup_data(bptree_t h,
			       struct shm_bench_data *sbd)
{
	int rc;
	struct key_value *kv = NULL;
	int i;
	
	for (i = 0; i < sbd->nr_kvs; i++) {
		kv = &sbd->kvs[i];
		rc = bpt_deletekey(h, (unsigned char *)kv->key,
				   kv->len, 0);
		if (rc != 0) {
			fprintf(stderr, "Failed to delete key: %s\n",
				kv->key);
		}
	}
}

static void do_bench(bptree_t h, struct shm_bench_data *sbd, int op)
{
	int rc;
	unsigned int i;
	nodeno_t node_no;
	
	while ((i = __sync_add_and_fetch(&sbd->index, 1)) < sbd->nr_kvs) {
		if (op == BENCH_OP_READ) {
			node_no = bpt_findkey(h, (unsigned char *)sbd->kvs[i].key,
					      sbd->kvs[i].len);
			if (node_no == 0) {
				fprintf(stderr, "Failed to find key: %s\n",
					sbd->kvs[i].key);
				break;
			}
		} else if (op == BENCH_OP_WRITE) {
			rc = bpt_insertkey(h, (unsigned char *)sbd->kvs[i].key,
					   sbd->kvs[i].len, 0,
					   sbd->kvs[i].value);
			if (rc != 0) {
				fprintf(stderr, "Failed to insert key: %s\n",
					sbd->kvs[i].key);
				break;
			}
		} else {
			fprintf(stderr, "Mixed read/write not supported yet!\n");
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	int rc = 0;
	int ch = 0;
	bptree_t h = NULL;
	struct bpt_iostat iostat;
	int i;
	struct timespec start, end;
	double t;
	struct thread_info *ti = NULL;
	pid_t pid = -1;
	pthread_mutexattr_t mutex_attr;
	pthread_condattr_t cond_attr;

	while ((ch = getopt(argc, argv, "p:n:o:rc:t:P:Ch")) != -1) {
		switch (ch) {
		case 'p':
			opts.node_bits = atoi(optarg);
			break;
		case 'n':
			opts.rounds = atoi(optarg);
			if (opts.rounds == 0) {
				fprintf(stderr, "rounds must greater than 0\n");
				goto out;
			}
			break;
		case 'o':
			if (strcmp(optarg, "read") == 0) {
				opts.op = BENCH_OP_READ;
			} else if (strcmp(optarg, "write") == 0) {
				opts.op = BENCH_OP_WRITE;
			} else if (strcmp(optarg, "rw") == 0) {
				opts.op = BENCH_OP_RW;
			} else {
				fprintf(stderr, "Illegal operation:%s\n", optarg);
				goto out;
			}
			break;
		case 'r':
			opts.random = true;
			break;
		case 'c':
			opts.cache_capacity = atoi(optarg);
			if (opts.cache_capacity == 0) {
				fprintf(stderr, "cache capacity must greater than 0\n");
				goto out;
			}
			break;
		case 't':
			opts.nr_threads = atoi(optarg);
			if (opts.nr_threads == 0) {
				fprintf(stderr, "threads number must greater than 0\n");
				goto out;
			}
			break;
		case 'P':
			opts.nr_processes = atoi(optarg);
			if (opts.nr_processes == 0) {
				fprintf(stderr, "process number must greater than 0\n");
				goto out;
			}
			break;
		case 'C':
			opts.no_cleanup = true;
			break;
		case 'h':
		default:
			usage();
			goto out;
		}
	}

	/* Register signal handler */
	if (signal(SIGTERM, bench_sig_handler) == SIG_ERR) {
		vperror("catch SIGTERM failed!");
	}
	if (signal(SIGQUIT, bench_sig_handler) == SIG_ERR) {
		vperror("catch SIGQUIT failed!");
	}
	if (signal(SIGINT, bench_sig_handler) == SIG_ERR) {
		vperror("catch SIGINT failed!");
	}

	/* Create shared memory for kvs used for benchmarking */
	shm_size = offsetof(struct shm_bench_data, kvs) +
		opts.rounds * sizeof(struct key_value);

	shmfd = shm_open(shm_name, O_RDWR|O_CREAT|O_EXCL, 0666);
	if (shmfd == -1) {
		vperror("shm_open failed!");
		goto out;
	}

	rc = ftruncate(shmfd, shm_size);
	if (rc == -1) {
		vperror("ftruncate failed!");
		goto out;
	}

	bench_data = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, shmfd, 0);
	if (bench_data == MAP_FAILED) {
		vperror("mmap failed!");
		goto out;
	}
	memset(bench_data, 0, shm_size);

	/* Initialize mutex and cond to be shared between processes */
	pthread_mutexattr_init(&mutex_attr);
	pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
	pthread_mutex_init(&bench_data->mutex, &mutex_attr);
	pthread_mutexattr_destroy(&mutex_attr);
	
	pthread_condattr_init(&cond_attr);
	pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
	pthread_cond_init(&bench_data->cond, &cond_attr);
	pthread_condattr_destroy(&cond_attr);

	/* Prepare test key values */
	bench_prepare_data(bench_data, opts.rounds, opts.random);

	/* Create/Open b+tree */
	mgr = bpt_openmgr("bptbench.dat", opts.node_bits, 128, 13);
	if (mgr == NULL) {
		fprintf(stderr, "Failed to open/create b+tree manager!\n");
		goto out;
	}

	/* Open b+tree handle from manager */
	h = bpt_open(mgr);
	if (h == NULL) {
		fprintf(stderr, "Failed to open/create b+tree!\n");
		goto out;
	}

	/* Need to prefill data if we're doing read benchmark */
	if (opts.op == BENCH_OP_READ) {
		bench_prefill_data(h, bench_data);
	}

	/* Open semaphore for inter-process coordination */
	bench_sem = sem_open(bench_sem_name, O_RDWR|O_CREAT|O_EXCL,
			     0666, 0);
	if (bench_sem == SEM_FAILED) {
		vperror("create bench semaphore failed!");
		goto out;
	}
		
	/* Fork as many processes as requested */
	for (i = 0; i < (opts.nr_processes - 1); i++) {
		pid = fork();
		if (pid == -1) {
			vperror("fork %d failed!", i);
			goto out;
		}

		if (pid == 0) {
			/* We are child, go on to wait for
			 * signal to start benchmarking
			 */
			is_parent = false;
			break;
		} else {
			printf("forked process %d!\n", pid);
		}
	}

	/* Create threads if we need more than 1 thread */
	if (opts.nr_threads > 1) {
		/* Allocate thread info */
		ti = (struct thread_info *)malloc(opts.nr_threads * sizeof(*ti));
		if (ti == NULL) {
			fprintf(stderr, "Failed to allocate thread info!\n");
			rc = -1;
			goto out;
		}
		memset(ti, 0, opts.nr_threads * sizeof(*ti));
		
		/* Create threads */
		for (i = 0; i < (opts.nr_threads - 1); i++) {
			ti[i].mgr = mgr;
			ti[i].sem = bench_sem;
			ti[i].bench_data = bench_data;
			ti[i].op = opts.op;
			rc = pthread_create(&ti[i].thread, NULL,
					    benchmark_thread,
					    &ti[i]);
			if (rc != 0) {
				fprintf(stderr, "Failed to create thread %d!\n", i);
				goto out;
			}
		}
	}

	/* We are parent, wait for all threads get ready */
	if (is_parent) {
		while (bench_data->ready_threads <
		       (opts.nr_processes * opts.nr_threads - 1)) {
			usleep(10000);
		}

		/* Notify all threads to start benchmarking. */
		rc = pthread_mutex_lock(&bench_data->mutex);
		if (rc != 0) {
			fprintf(stderr, "Failed to lock bench mutex!\n");
			goto out;
		}
		printf("Ready, GO!\n");
		clock_gettime(CLOCK_MONOTONIC, &start);
		rc = pthread_cond_broadcast(&bench_data->cond);
		if (rc != 0) {
			fprintf(stderr, "Failed to broadcast bench condition!\n");
			goto out;
		}
		rc = pthread_mutex_unlock(&bench_data->mutex);
		if (rc != 0) {
			fprintf(stderr, "Failed to unlock bench mutex!\n");
			goto out;
		}
	} else {
		/* We are child, waiting for signal to start benchmarking */
		rc = pthread_mutex_lock(&bench_data->mutex);
		if (rc != 0) {
			fprintf(stderr, "Failed to lock mutex, error:%d\n", rc);
			goto out;
		}

		__sync_add_and_fetch(&bench_data->ready_threads, 1);
		rc = pthread_cond_wait(&bench_data->cond, &bench_data->mutex);
		if (rc != 0) {
			fprintf(stderr, "Failed to wait cond, error:%d\n", rc);
			goto out;
		}
		rc = pthread_mutex_unlock(&bench_data->mutex);
		if (rc != 0) {
			fprintf(stderr, "Failed to unlock mutex, error:%d\n", rc);
			goto out;
		}
	}

	printf("thread:%ld benchmarking started...\n", gettid());
	do_bench(h, bench_data, opts.op);

	if (is_parent) {
		for (i = 0; i < (opts.nr_processes * opts.nr_threads - 1); i++) {
			sem_wait(bench_sem);
		}
		printf("thread:%ld benchmarking done...\n", gettid());
	} else {
		sem_post(bench_sem);
		printf("thread:%ld benchmarking done...\n", gettid());
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	
	if (opts.nr_threads > 1) { // Multi-thread bench
		for (i = 0; i < (opts.nr_threads - 1); i++) {
			rc = pthread_join(ti[i].thread, NULL);
			if (rc != 0) {
				fprintf(stderr, "Failed to join thread %d\n", i);
				goto out;
			}
		}
		printf("process %d benchmarking done!\n", getpid());
	}

	bpt_getiostat(h, &iostat);

	/* Only print bench summary in parent process */
	if (is_parent) {
		t = ((end.tv_sec - start.tv_sec) * 1e9 +
		     (end.tv_nsec - start.tv_nsec)) / 1e9;
		
		printf("Bench summary: \n");
		print_seperator();
		dump_options(&opts);
		print_seperator();
		printf("Elapsed time: %f seconds\n", t);
		print_seperator();
		printf("BPT I/O statistics:\n");
		print_seperator();
		dump_bpt_iostat(&iostat);

		if (!opts.no_cleanup) {
			bench_cleanup_data(h, bench_data);
		}
	}

 out:
	if (ti) {
		free(ti);
	}

	if (h) {
		bpt_close(h);
	}

	bench_cleanup();

	return rc;
}

static void usage()
{
	printf("usage: bench [-p <node-bits>] [-n <#keys>] [-o <read|write>] [-r] \\\n"
	       "  [-c <capacity>] [-C] [-P <#processes>]\n"
	       " -r        random I/O\n"
	       " -C        do not cleanup data after test\n\n");
	printf("default options:\n");
	dump_options(&opts);
}

static void print_seperator()
{
	printf("========================================\n");
}

