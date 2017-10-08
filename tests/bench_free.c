#include <stdio.h>
#include <stdlib.h>

#include <ps_slab.h>

PS_SLAB_CREATE(local, PS_CACHE_LINE, PS_PAGE_SIZE)
PS_SLAB_CREATE(remote, 2*PS_CACHE_LINE, PS_PAGE_SIZE)

#define CPU_FREQ 2000000
#define TIMER_FREQ (CPU_FREQ*10)
#define ITER       (10000000)
#define FREE_BATCH 64
#define RB_SZ   ((PS_NUMCORES-1)*FREE_BATCH)
#define ALLOC_BATCH 10000
void *ptrs[ALLOC_BATCH];
unsigned long cost[ITER] PS_ALIGNED;
unsigned long alloc[ITER] PS_ALIGNED;
void * volatile ring_buffer[RB_SZ] PS_ALIGNED;
unsigned long long free_tsc, alloc_tsc;
__thread int thd_local_id;
int printer_id;

static int cmpfunc(const void * a, const void * b)
{
	unsigned long aa, bb;
	aa = *(unsigned long*)a;
	bb = *(unsigned long*)b;
	if (bb>aa) return 1;
	if (bb<aa) return -1;
	return 0;
}

static void out_latency(unsigned long *re, int num, char *label)
{
	int i;
	unsigned long long sum = 0;

	for(i=0; i<num; i++) sum += (unsigned long long)re[i];
	qsort(re, num, sizeof(unsigned long), cmpfunc);
	printf("thd %d tot %d avg %llu 99.9 %lu 99 %lu min %lu max %lu\n", thd_local_id, 
	       num, sum/num, re[num/1000], re[num/100], re[num-1], re[0]);
	printf("%s %lu\n", label, re[num/100]);
}

void
consumer(void)
{
	char *s, *h;
	int id = thd_local_id, tf = 0;
	unsigned long b, e, i, k = 0;
	unsigned long long start, end;

	b = (id-1)*FREE_BATCH;
	e = id*FREE_BATCH;
	meas_barrier(PS_NUMCORES);

c_begin:
	for(i = b; i<e; i++) {
		while (!ring_buffer[i]) ;
		s = (char *)ring_buffer[i];
		if (s == (void *)-1) goto c_end;
		ring_buffer[i] = NULL;
		assert(i == ((int *)s)[0]);
		h = s-sizeof(struct ps_mheader);
		h[0] = 0;
		ps_mem_fence();

		start = ps_tsc();
		ps_slab_free_remote(s);
		end = ps_tsc();
		if (id == printer_id && k < ITER /* && mmin > 200 */) cost[k++] = end-start;
		tf++;
	}
	goto c_begin;
c_end:
	/* printf("thd %d free tot %d k %d %d\n", id, tf, k, tf/k); */
	if (id == printer_id) out_latency(cost, k, "####");
}

void
producer(void)
{
	void *s;
	unsigned long i, k = 0, b = 0;
	unsigned long long start, end;

	meas_barrier(PS_NUMCORES);

p_begin:
	for(i=b; i<RB_SZ; i+=(PS_NUMCORES-1)) {
		if (ring_buffer[i]) continue;
		start = ps_tsc();
		s = ps_slab_alloc_remote();
		end = ps_tsc();
		assert(s);
		((int *)s)[0] = i;
		ps_mem_fence();
		ring_buffer[i] = s;
		if (k < ITER) alloc[k] = end-start;
		if ((++k) == (PS_NUMCORES-1)*ITER) goto p_out;
	}
	b = (b+1) % FREE_BATCH;
	goto p_begin;
p_out:
	out_latency(alloc, ITER, "@@@@");
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
	int i, ret, *s;
	
	printf("Starting test for remote frees\n");
	for(i=0; i<RB_SZ; i++) {
		s = (int *)ps_slab_alloc_remote();
		s[0] = i;
		ring_buffer[i] = (void *)s;
	}

	for (i = 1; i < PS_NUMCORES; i++) {
		ret = pthread_create(&child[i], 0, child_fn, (void *)i);
		if (ret) {
			perror("pthread create thd fail\n");
			printf("pthread create of child %d\n", i);
			exit(-1);
		}
	}

	producer();
	for(i=0; i<RB_SZ; i++) ring_buffer[i] = (void *)-1;
	
	for (i = 1; i < PS_NUMCORES; i++) {
		pthread_join(child[i], NULL);
	}
}

void
test_local(void)
{
	int i, j, k = 0, l = 0;
	unsigned long long e, s;

	for (i = 0 ; i < ALLOC_BATCH ; i++) ptrs[i] = ps_slab_alloc_local();
	for (i = 0 ; i < ALLOC_BATCH ; i++) {
		s = ps_tsc();
		ps_slab_free_local(ptrs[i]);
		e = ps_tsc();
	}

	for(j=0; j<ITER/ALLOC_BATCH; j++) {
		for (i = 0 ; i < ALLOC_BATCH ; i++) {
			s = ps_tsc();
			ptrs[i] = ps_slab_alloc_local();
			e = ps_tsc();
			alloc[l++] = e-s;
		}
		for (i = 0 ; i < ALLOC_BATCH ; i++) {
			s = ps_tsc();
			ps_slab_free_local(ptrs[i]);
			e = ps_tsc();
			cost[k++] = e-s;
		}
	}
	out_latency(alloc, l, "aloc");
	out_latency(cost, k, "free");
}

void set_smp_affinity()
{
	char cmd[64];
	/* everything done is the python script. */
	sprintf(cmd, "python set_smp_affinity.py %d %d", 40, getpid());
	system(cmd);
}

int
main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("usage: %s print_core_id\n", argv[0]);
		exit(-1);
	}
	printer_id = atoi(argv[1]);
	thd_local_id = 0;
	set_smp_affinity();
	thd_set_affinity(pthread_self(), 0);
	printf("%d cores:\n", PS_NUMCORES);
	/* timer_gap(); */

	/* test_local(); */
	test_remote_frees();

	return 0;
}
