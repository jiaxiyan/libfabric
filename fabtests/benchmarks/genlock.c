/*
 * Copyright (c) 2026 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under the BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Micro-benchmark: ofi_genlock_lock / ofi_genlock_unlock throughput
 * for each lock type (MUTEX, SPINLOCK, NOOP, NONE).
 *
 * Measures the overhead of the genlock dispatch path in a tight
 * single-threaded loop.  Useful for comparing indirect function-pointer
 * dispatch (before commit a7a71df3f) vs inline switch dispatch (after).
 *
 * The genlock struct and inline helpers are duplicated here from
 * ofi_lock.h so the benchmark builds as a standalone fabtests
 * component without pulling in the full libfabric internal header
 * chain.
 *
 * Usage: fi_genlock_bench [-i iterations]
 *        Default: 100 000 000 iterations
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>
#include <pthread.h>

/* ---- minimal genlock replica (mirrors include/ofi_lock.h) ---------- */

enum ofi_lock_type {
	OFI_LOCK_MUTEX,
	OFI_LOCK_SPINLOCK,
	OFI_LOCK_NOOP,
	OFI_LOCK_NONE,
};

struct ofi_genlock {
	enum ofi_lock_type	lock_type;
	union {
		pthread_mutex_t		mutex;
		pthread_spinlock_t	spinlock;
	} base;
};

static int genlock_init(struct ofi_genlock *lock, enum ofi_lock_type type)
{
	lock->lock_type = type;
	switch (type) {
	case OFI_LOCK_SPINLOCK:
		return pthread_spin_init(&lock->base.spinlock,
					 PTHREAD_PROCESS_PRIVATE);
	case OFI_LOCK_MUTEX:
	case OFI_LOCK_NOOP:
		return pthread_mutex_init(&lock->base.mutex, NULL);
	case OFI_LOCK_NONE:
		return 0;
	default:
		return -1;
	}
}

static void genlock_destroy(struct ofi_genlock *lock)
{
	switch (lock->lock_type) {
	case OFI_LOCK_SPINLOCK:
		pthread_spin_destroy(&lock->base.spinlock);
		break;
	case OFI_LOCK_MUTEX:
	case OFI_LOCK_NOOP:
		pthread_mutex_destroy(&lock->base.mutex);
		break;
	default:
		break;
	}
}

/*
 * After commit a7a71df3f these are inline switch dispatches.
 * Before that commit they were indirect calls through function pointers:
 *   lock->lock(&lock->base);
 *   lock->unlock(&lock->base);
 *
 * Toggle USE_FPTR to benchmark the old (indirect) path.
 */
#ifdef USE_FPTR

/* ---------- pre-a7a71df3f: indirect function-pointer dispatch ------- */

typedef void (*genlock_op_t)(void *);

struct ofi_genlock_fptr {
	enum ofi_lock_type	lock_type;
	union {
		pthread_mutex_t		mutex;
		pthread_spinlock_t	spinlock;
	} base;
	genlock_op_t	lock;
	genlock_op_t	unlock;
};

static void fptr_mutex_lock(void *p)   { pthread_mutex_lock(p); }
static void fptr_mutex_unlock(void *p) { pthread_mutex_unlock(p); }
static void fptr_spin_lock(void *p)    { pthread_spin_lock(p); }
static void fptr_spin_unlock(void *p)  { pthread_spin_unlock(p); }
static void fptr_noop(void *p)         { (void) p; }

static int genlock_fptr_init(struct ofi_genlock_fptr *lock,
			     enum ofi_lock_type type)
{
	lock->lock_type = type;
	switch (type) {
	case OFI_LOCK_SPINLOCK:
		lock->lock   = fptr_spin_lock;
		lock->unlock = fptr_spin_unlock;
		return pthread_spin_init(&lock->base.spinlock,
					 PTHREAD_PROCESS_PRIVATE);
	case OFI_LOCK_MUTEX:
		lock->lock   = fptr_mutex_lock;
		lock->unlock = fptr_mutex_unlock;
		return pthread_mutex_init(&lock->base.mutex, NULL);
	case OFI_LOCK_NOOP:
		lock->lock   = fptr_noop;
		lock->unlock = fptr_noop;
		return pthread_mutex_init(&lock->base.mutex, NULL);
	case OFI_LOCK_NONE:
		lock->lock   = fptr_noop;
		lock->unlock = fptr_noop;
		return 0;
	default:
		return -1;
	}
}

static void genlock_fptr_destroy(struct ofi_genlock_fptr *lock)
{
	genlock_destroy((struct ofi_genlock *) lock);
}

static inline void genlock_fptr_lock(struct ofi_genlock_fptr *lock)
{
	lock->lock(&lock->base);
}

static inline void genlock_fptr_unlock(struct ofi_genlock_fptr *lock)
{
	lock->unlock(&lock->base);
}

#endif /* USE_FPTR */

/* ---------- post-a7a71df3f: inline switch dispatch ------------------ */

static inline void genlock_lock(struct ofi_genlock *lock)
{
	switch (lock->lock_type) {
	case OFI_LOCK_SPINLOCK:
		pthread_spin_lock(&lock->base.spinlock);
		break;
	case OFI_LOCK_MUTEX:
		pthread_mutex_lock(&lock->base.mutex);
		break;
	case OFI_LOCK_NOOP:
	case OFI_LOCK_NONE:
	default:
		break;
	}
}

static inline void genlock_unlock(struct ofi_genlock *lock)
{
	switch (lock->lock_type) {
	case OFI_LOCK_SPINLOCK:
		pthread_spin_unlock(&lock->base.spinlock);
		break;
	case OFI_LOCK_MUTEX:
		pthread_mutex_unlock(&lock->base.mutex);
		break;
	case OFI_LOCK_NOOP:
	case OFI_LOCK_NONE:
	default:
		break;
	}
}

/* -------------------------------------------------------------------- */

static double elapsed_ns(struct timespec *t0, struct timespec *t1)
{
	return (t1->tv_sec - t0->tv_sec) * 1e9 +
	       (t1->tv_nsec - t0->tv_nsec);
}

static void bench_switch(const char *name, enum ofi_lock_type type, long iters)
{
	struct ofi_genlock lock;
	struct timespec t0, t1;
	double ns;
	long i;

	if (genlock_init(&lock, type)) {
		fprintf(stderr, "genlock_init failed for %s\n", name);
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++) {
		genlock_lock(&lock);
		genlock_unlock(&lock);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	ns = elapsed_ns(&t0, &t1);
	printf("  switch  %-14s  %10.2f ns/pair  %12.3f M pairs/s\n",
	       name, ns / iters, iters / (ns / 1e3));

	genlock_destroy(&lock);
}

#ifdef USE_FPTR
static void bench_fptr(const char *name, enum ofi_lock_type type, long iters)
{
	struct ofi_genlock_fptr lock;
	struct timespec t0, t1;
	double ns;
	long i;

	if (genlock_fptr_init(&lock, type)) {
		fprintf(stderr, "genlock_fptr_init failed for %s\n", name);
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++) {
		genlock_fptr_lock(&lock);
		genlock_fptr_unlock(&lock);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	ns = elapsed_ns(&t0, &t1);
	printf("  fptr    %-14s  %10.2f ns/pair  %12.3f M pairs/s\n",
	       name, ns / iters, iters / (ns / 1e3));

	genlock_fptr_destroy(&lock);
}
#endif

static void usage(void)
{
	printf("Usage: fi_genlock_bench [-i iterations]\n"
	       "\n"
	       "  Compile with -DUSE_FPTR to also benchmark the old\n"
	       "  indirect function-pointer dispatch path.\n");
}

struct lock_desc {
	const char		*name;
	enum ofi_lock_type	type;
};

static const struct lock_desc locks[] = {
	{ "MUTEX",    OFI_LOCK_MUTEX },
	{ "SPINLOCK", OFI_LOCK_SPINLOCK },
	{ "NOOP",     OFI_LOCK_NOOP },
	{ "NONE",     OFI_LOCK_NONE },
};

#define NLOCKS (sizeof(locks) / sizeof(locks[0]))

int main(int argc, char **argv)
{
	long iters = 100000000;
	int op;
	size_t i;

	while ((op = getopt(argc, argv, "i:h")) != -1) {
		switch (op) {
		case 'i':
			iters = atol(optarg);
			break;
		default:
			usage();
			return (op == 'h') ? 0 : 1;
		}
	}

	printf("genlock lock/unlock micro-benchmark  (%ld iterations)\n"
	       "------------------------------------------------------\n",
	       iters);

	for (i = 0; i < NLOCKS; i++) {
		bench_switch(locks[i].name, locks[i].type, iters);
#ifdef USE_FPTR
		bench_fptr(locks[i].name, locks[i].type, iters);
#endif
	}

	return 0;
}