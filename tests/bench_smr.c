#define __GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define __USE_GNU
#include <sched.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "bench_smr.h"

#define PRINT_THD 0
#define N_OPS (10000000)

int rcu_spin = 0;
__thread int thd_local_id;
unsigned long p99_log[N_OPS] CACHE_ALIGNED;

static int cmpfunc(const void * a, const void * b)
{
	unsigned long aa, bb;
	aa = *(unsigned long*)a;
	bb = *(unsigned long*)b;
	if (bb>aa) return 1;
	if (bb<aa) return -1;
	return 0;
}

static void out_latency(unsigned long *re, int num)
{
	int i;
	unsigned long long sum = 0;

	for(i=0; i<num; i++) sum += (unsigned long long)re[i];
	qsort(re, num, sizeof(unsigned long), cmpfunc);
	printf("thd %d tot %d avg %llu 99.9 %lu 99 %lu min %lu max %lu\n", thd_local_id, num, sum/num,
	       re[num/1000], re[num/100], re[num-1], re[0]);
	printf("#### %lu\n", re[num/100]);
}

static void Init_thd(void)
{
	thd_set_affinity(pthread_self(), thd_local_id);
#if BENCH_OP == 1
	ps_init_period(&ps, 1);
#elif BENCH_OP == 2
	rcu_register_thread();
	rcu_init();
#elif BENCH_OP == 6
	ck_epoch_register(&global_epoch, &(epoch_records[thd_local_id].record));
#elif BENCH_OP == 7
	ck_brlock_read_register(&brlock, &(brlock_readers[thd_local_id].reader));
#endif
	return ;
}

void
spin_delay(unsigned long long cycles)
{
	unsigned long long s, e, curr;

	s = ps_tsc();
	e = s + cycles;

	curr = s;
	while (curr < e) curr = ps_tsc();
	
	return;
}

void *
nil_call(void *arg)
{
	(void)arg;

	return 0;
}

void bench_read(void) {
	int i, id, ret = 0;
	unsigned long long s1, e1, cost, rcu_cost = 0;
	
	(void)ret;
	id = thd_local_id;
	
	for (i = 0 ; i < N_OPS; i++) {
		/* printf("begin read id %d %d\n", id, i); */
		s1 = ps_tsc();
#if BENCH_OP == 1
		ps_enter(&ps);
		/* ret = nil_call(NULL); */
		ps_exit(&ps);
#elif BENCH_OP == 2
		rcu_read_lock();
		ret = nil_call(NULL);
		e1 = ps_tsc();
		assert(e1 > s1);
		rcu_cost = e1-s1;
		ps_mem_fence();
		spin_delay(rcu_spin);
		s1 = ps_tsc();
		rcu_read_unlock();
#elif BENCH_OP == 3
		ck_pflock_read_lock(&pfrwlock);
		ck_pflock_read_unlock(&pfrwlock);
#elif BENCH_OP == 4
		ck_spinlock_mcs_lock(&mcslock, &(mcslocks[id].lock_context));
		ck_spinlock_mcs_unlock(&mcslock, &(mcslocks[id].lock_context));
#elif BENCH_OP == 5
		ck_spinlock_ticket_lock(&ticketlock);
		ck_spinlock_ticket_unlock(&ticketlock);
#elif BENCH_OP == 6
		ck_epoch_begin(&global_epoch, &(epoch_records[id].record));
		ck_epoch_end(&global_epoch, &(epoch_records[id].record));
#elif BENCH_OP == 7
		ck_brlock_read_lock(&brlock, &(brlock_readers[id].reader));
		ck_brlock_read_unlock(&(brlock_readers[id].reader));
#endif
		e1 = ps_tsc();
		assert(e1 > s1);
		cost = e1-s1+rcu_cost;
		rcu_cost = 0;
		if (id == PRINT_THD) p99_log[i] = (unsigned long)cost;
	}

	if (id == PRINT_THD) {
		printf("read worst case\n");
		out_latency(p99_log, N_OPS);
	}
	return;
}

void bench_update(void) {
	int i, id, ret = 0;
	unsigned long long s1, e1, cost, tot = 0;
	int iter;
	void *last_alloc;
	
	(void)ret;
	id = thd_local_id;
	iter = N_OPS;

/* #if BENCH_OP == 1 */
/* 	iter = N_OPS/PS_NUMCORES; */
/* #endif */

#if BENCH_OP == 1
		last_alloc = ps_mem_alloc_bench();
		assert(last_alloc);
		ps_mem_free_bench(last_alloc);
#endif

	for (i = 0 ; i < iter; i++) {
		s1 = ps_tsc();
#if BENCH_OP == 1
		ps_quiesce_bench();
#elif BENCH_OP == 2
		synchronize_rcu();
#elif BENCH_OP == 3
		ck_pflock_write_lock(&pfrwlock);
		ck_pflock_write_unlock(&pfrwlock);
#elif BENCH_OP == 4
		ck_spinlock_mcs_lock(&mcslock, &(mcslocks[id].lock_context));
		ck_spinlock_mcs_unlock(&mcslock, &(mcslocks[id].lock_context));
#elif BENCH_OP == 5
		ck_spinlock_ticket_lock(&ticketlock);
		ck_spinlock_ticket_unlock(&ticketlock);
#elif BENCH_OP == 6
		ck_epoch_synchronize(&global_epoch, &(epoch_records[id].record));
#elif BENCH_OP == 7
		ck_brlock_write_lock(&brlock);
		ck_brlock_write_unlock(&brlock);
#endif
		e1 = ps_tsc();
		assert(e1 > s1);
		cost = e1-s1;
		if (id == PRINT_THD) p99_log[i] = (unsigned long)cost;
	}
	if (id == PRINT_THD) {
		printf("update worst case\n");
		out_latency(p99_log, N_OPS);
	}

	return;
}

void * 
worker_update(void *arg)
{
	thd_local_id = (int)arg;
	Init_thd();
	/* printf("thd %d writer\n", thd_local_id); */

	meas_barrier(PS_NUMCORES);
	bench_update();
	usleep(50000);
	/* meas_barrier(PS_NUMCORES); */

	return 0;
}

void * 
worker_read(void *arg)
{	
	thd_local_id = (int)arg;
	Init_thd();
	/* printf("thd %d reader\n", thd_local_id); */

	meas_barrier(PS_NUMCORES);
	bench_read();
	usleep(50000);
	/* meas_barrier(PS_NUMCORES); */
	return 0;
}

void set_smp_affinity()
{
	char cmd[64];
	/* everything done is the python script. */
	sprintf(cmd, "python set_smp_affinity.py %d %d", 40, getpid());
	system(cmd);
}

int main(int argc, char *argv[])
{
	int i, ret, ar, aw;
	pthread_t thds[NUM_CPU];

	if (argc < 4) {
		printf("usage: %s #reader #writer test_type\n", argv[0]);
		exit(-1);
	}
	rcu_spin = atoi(argv[4]);
	ar = atoi(argv[1]);
	aw = atoi(argv[2]);
	assert(ar + aw == NUM_CPU);
	printf("%d cores readers %d writers %d\n", NUM_CPU, ar, aw);
	if (*argv[3] == 'r') ar--;
	else aw--;

	set_smp_affinity();
	ret = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (ret) {
		printf("cannot lock memory %d... exit.\n", ret);
		exit(-1);
	}
	Init();
	thd_local_id = 0;
	thd_set_affinity(pthread_self(), 0);
	memset(p99_log, 0, sizeof(p99_log));

	for (i = 1; ar>0; i++, ar--) {
		ret = pthread_create(&thds[i], 0, worker_read, (void *)i);
		if (ret) exit(-1);
	}
	for (; aw>0; i++, aw--) {
		ret = pthread_create(&thds[i], 0, worker_update, (void *)i);
		if (ret) exit(-1);
	}

	usleep(5000);

	if (*argv[3] == 'r')  worker_read((void *)0);
	else worker_update((void *)0);

	for (i = 1; i < NUM_CPU; i++) {
		pthread_join(thds[i], (void *)&ret);
	}

	return 0;
}
