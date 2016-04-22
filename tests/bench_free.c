#include <stdio.h>
#include <stdlib.h>

#include <ps_slab.h>

PS_SLAB_CREATE(local, PS_CACHE_LINE, PS_PAGE_SIZE)
PS_SLAB_CREATE(remote, PS_CACHE_LINE, PS_PAGE_SIZE)

#define ITER       (10000000)
#define RB_SZ   (1024 * 32)
void *ptrs[ITER];
unsigned long cost[ITER/10] PS_ALIGNED;
unsigned long alloc[RB_SZ] PS_ALIGNED;
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
	struct small *s;
	unsigned long i = 0, jump = PS_NUMCORES-1, num = 0;
	unsigned long long start, end, tot = 0, mmax, mmin;

	i = thd_local_id-1;
	meas_barrier(PS_NUMCORES);

	while(1) {
		unsigned long off = i % RB_SZ;

		while (!ring_buffer[off]) ;
		s = ring_buffer[off];
		ring_buffer[off] = NULL;
		if (s == (void *)-1) break;

		start = ps_tsc();
		ps_slab_free_remote(s);
		end = ps_tsc();
		if (thd_local_id == 1) {
			cost[num % RB_SZ] = end-start;
			tot += cost[numi % RB_SZ];
			if (cost[num % RB_SZ] > mmax) mmax = cost[num % RB_SZ];
			if (cost[num % RB_SZ] < mmin) mmin = cost[num % RB_SZ];
			num++;
		}
		i += jump;
	}
	free_tsc = tot / num;
	printf("remote free avg %llu max %llu min %llu\n", free_tsc, mmax, mmin);
}

void
producer(void)
{
	void *s;
	unsigned long i;
	unsigned long long start, end, tot = 0, mmax, mmin;

	mmax = 0;
	mmin = 1000000;
	meas_barrier(PS_NUMCORES);

	for (i = 0 ; i < RB_ITER ; i++) {
		unsigned long off = i % RB_SZ;
		
		while (ring_buffer[off]) ; 

		start = ps_tsc();
		s = ps_slab_alloc_remote();
		end = ps_tsc();
		alloc[off] = end-start;
		tot += alloc[off];
		if (alloc[off] > mmax) mmax = alloc[off];
		if (alloc[off] < mmin) mmin = alloc[off];

		assert(s);
		ring_buffer[off] = s;
	}
	alloc_tsc = tot / RB_ITER;
	printf("remote alloc avg %llu max %llu min %llu\n", alloc_tsc, mmax, mmin);
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
	int i, j;
	unsigned long long s, mmin = 10000000;
	unsigned long long e, mmax = 0, tot = 0;

	ps_slab_free_local(ps_slab_alloc_local());
	for (j = 0 ; j < 10 ; j++) {
		for (i = 0 ; i < ITER/10 ; i++) ptrs[i] = ps_slab_alloc_local();
		for (i = 0 ; i < ITER/10 ; i++) {
			s = ps_tsc();
			ps_slab_free_local(ptrs[i]);
			e = s_tsc();
			cos[i] = e-s;
			tot += cos[i]; 
			if (cos[i] > mmax) mmax = cos[i];
			if (cos[i] < mmin) mmin = cos[i];
		}
	}
	printf("local free avg %llu max %llu min %llu", tot/ITER, mman, mmin);
	qsort(cos, ITER/10, sizeof(unsigned long), cmpfunc);
	for(i=0; i<50; i++) printf("%d ", cost[i]);
	printf("\n");
}

void
stats_print(struct ps_mem *m)
{
	struct ps_slab_stats s;
	int i;

	printf("Stats for slab @ %p\n", (void*)m);
	ps_slabptr_stats(m, &s);
	for (i = 0 ; i < PS_NUMCORES ; i++) {
		printf("\tcore %d, slabs %ld, partial slabs %ld, nfree %ld, nremote %ld\n", 
		       i, s.percore[i].nslabs, s.percore[i].npartslabs, s.percore[i].nfree, s.percore[i].nremote);
	}
}

int
main(void)
{
	thd_local_id = 0;
	thd_set_affinity(pthread_self(), 0);

	test_local();

//	stats_print(&__ps_mem_l);
	test_remote_frees();

	return 0;
}
