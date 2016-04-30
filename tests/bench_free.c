#include <stdio.h>
#include <stdlib.h>

#include <ps_slab.h>

PS_SLAB_CREATE(local, PS_CACHE_LINE, PS_PAGE_SIZE)
PS_SLAB_CREATE(remote, PS_CACHE_LINE, PS_PAGE_SIZE)

#define CPU_FREQ 2000000
#define TIMER_FREQ (CPU_FREQ*10)
#define ITER       (10000000)
#define RB_SZ   (1024 * 32)
void *ptrs[ITER/10];
unsigned long cost[ITER] PS_ALIGNED;
unsigned long alloc[ITER] PS_ALIGNED;
void * volatile ring_buffer[RB_SZ] PS_ALIGNED;
unsigned long long free_tsc, alloc_tsc;
__thread int thd_local_id;

static int cmpfunc(const void * a, const void * b)
{
	return ( *(int*)b - *(int*)a);
}

void
consumer(void)
{
	char *s, *h;
	unsigned long i = 0, jump = PS_NUMCORES-1, k = 0;
	unsigned long long start, end, tot = 0, mmax, mmin;

	i = thd_local_id-1;
	mmin = 1000000;
	meas_barrier(PS_NUMCORES);

	while(1) {
		while (!ring_buffer[i]) ;
		s = (char *)ring_buffer[i];
		ring_buffer[i] = NULL;
		if (s == (void *)-1) break;
		h = s-sizeof(struct ps_mheader);
		h[0] = 0;
		ps_mem_fence();

		start = ps_tsc();
		ps_slab_free_remote(s);
		end = ps_tsc();
		if (thd_local_id == 1 && k < ITER) {
			cost[k] = end-start;
			tot += cost[k];
			if (cost[k] > mmax) mmax = cost[k];
			if (cost[k] < mmin) mmin = cost[k];
			k++;
		}
		i += jump;
		if (i >= RB_SZ) i = thd_local_id-1;
	}
	if (thd_local_id == 1) {
		int t = tot/TIMER_FREQ;
		qsort(cost, k, sizeof(unsigned long), cmpfunc);
		free_tsc = tot / k;
		printf("remote free timer %d avg %llu max %llu %lu %lu min %llu\n", t, free_tsc, mmax, cost[t], cost[k/100], mmin);
		printf("remote free 99p %lu 99.9p %lu 99.99p %lu\n", cost[k/100], cost[k/1000], cost[k/10000]);
	}
}

void
producer(void)
{
	void *s;
	unsigned long i, k = 0;
	unsigned long long start, end, tot = 0, mmax, mmin;

	mmax = 0;
	mmin = 1000000;
	meas_barrier(PS_NUMCORES);

	for (i = 0 ; i < (PS_NUMCORES-1)*ITER ; i++) {
		unsigned long off = i % RB_SZ;
		
		while (ring_buffer[off]) ; 

		start = ps_tsc();
		s = ps_slab_alloc_remote();
		end = ps_tsc();
		alloc[k] = end-start;
		tot += alloc[k];
		if (k < ITER) {
			if (alloc[k] > mmax) mmax = alloc[k];
			if (alloc[k] < mmin) mmin = alloc[k];
			k++;
		}

		assert(s);
		ring_buffer[off] = s;
	}
	qsort(alloc, k, sizeof(unsigned long), cmpfunc);
	alloc_tsc = tot / ITER;
	int t = tot/TIMER_FREQ;
	printf("remote alloc timer %d avg %llu max %llu %lu %lu min %llu\n", t, alloc_tsc, mmax, alloc[t], alloc[k/100], mmin);
}

void *
child_fn(void *d)
{
	(void)d;
	
	thd_local_id = (int)d;
	thd_set_affinity(pthread_self(), (int)d);
	consumer();
	
	return NULL;
}

void
test_remote_frees(void)
{
	pthread_t child[PS_NUMCORES];
	int i, ret;
	
	printf("Starting test for remote frees\n");

	for (i = 1; i < PS_NUMCORES; i++) {
		ret = pthread_create(&child[i], 0, child_fn, (void *)i);
		if (ret) {
			perror("pthread create of child\n");
			exit(-1);
		}
	}

	for(i=0; i<RB_SZ; i++) ring_buffer[i] = ps_slab_alloc_remote();
	producer();
	for(i=0; i<RB_SZ; i++) ring_buffer[i] = (void *)-1;
	
	for (i = 1; i < PS_NUMCORES; i++) {
		pthread_join(child[i], NULL);
	}
}

void
test_local(void)
{
	int i, j, k = 0;
	unsigned long long s, mmin, start, subt = 0;
	unsigned long long e, mmax = 0, tot = 0, end;

	mmin = 10000000;
	for (i = 0 ; i < ITER/10 ; i++) ptrs[i] = ps_slab_alloc_local();
	for (i = 0 ; i < ITER/10 ; i++) {
		s = ps_tsc();
		ps_slab_free_local(ptrs[i]);
		e = ps_tsc();
	}

	for(j=0; j<10; j++) {
		for (i = 0 ; i < ITER/10 ; i++) ptrs[i] = ps_slab_alloc_local();
		start = ps_tsc();
		for (i = 0 ; i < ITER/10 ; i++) {
			s = ps_tsc();
			ps_slab_free_local(ptrs[i]);
			e = ps_tsc();
			cost[k] = e-s;
			tot += cost[k]; 
			if (cost[k] > mmax) mmax = cost[k];
			if (cost[k] < mmin) mmin = cost[k];
			k++;

		}
		end = ps_tsc();
		subt += (end-start);
	}
	qsort(cost, k, sizeof(unsigned long), cmpfunc);
	int t = subt/TIMER_FREQ;
	printf("local free timer %d avg %llu max %llu %lu 99p %lu min %llu\n", t, tot/ITER, mmax, cost[t], cost[k/100], mmin);
}

void timer_gap(void)
{
	int i, j;
	unsigned long long s, e, start, end;

	for(i=0; i<ITER/10; i++) {
		s = ps_tsc();
		for(j=0; j<10; j++) {
			j++;
			j--;
		}
		e = ps_tsc();
		cost[i] = e-s;
	}

	start = ps_tsc();
	for(i=0; i<ITER; i++) {
		s = ps_tsc();
		for(j=0; j<10; j++) {
			j++;
			j--;
		}
		e = ps_tsc();
		cost[i] = e-s;
	}
	end = ps_tsc();

	qsort(cost, ITER, sizeof(unsigned long), cmpfunc);
	printf("total timer %lu\n", (end-start)/TIMER_FREQ);
	/* for(i=0; i<50; i++) printf("%d ", cost[i]); */
	/* printf("\n"); */
}

void set_smp_affinity()
{
	char cmd[64];
	/* everything done is the python script. */
	sprintf(cmd, "python set_smp_affinity.py %d %d", 40, getpid());
	system(cmd);
}

int
main(void)
{
	thd_local_id = 0;
	set_smp_affinity();
	thd_set_affinity(pthread_self(), 0);
	printf("%d cores:\n", PS_NUMCORES);
	/* timer_gap(); */

	/* test_local(); */
	test_remote_frees();

	return 0;
}
