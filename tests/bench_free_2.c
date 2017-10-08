#include <stdio.h>
#include <stdlib.h>

#include <ps_slab.h>

#define REMOTE_SZ (2*PS_CACHE_LINE - sizeof(struct ps_mheader))
PS_SLAB_CREATE(local, PS_CACHE_LINE, PS_PAGE_SIZE)
PS_SLAB_CREATE(remote, REMOTE_SZ, PS_PAGE_SIZE)

#define CPU_FREQ 2000000
#define TIMER_FREQ (CPU_FREQ*10)
#define ITER       (10000000)
#define RB_SZ   (32*PS_NUMCORES)
/* #define NODE_SZ    (PS_CACHE_LINE/sizeof(void *)) */
#define NODE_SZ    (1)
/* #define NODE_SZ    ((PS_PAGE_SIZE-PS_CACHE_LINE)/sizeof(void *)) */
/* #define PRINTER (PS_NUMCORES-1) */
#define PRINTER (1)
struct node {
        volatile void * p[1];
	/* char  padding[PS_CACHE_LINE]; */
}PS_PACKED PS_ALIGNED;
volatile struct node ring_buffer[RB_SZ] PS_ALIGNED;
unsigned long cost[ITER] PS_ALIGNED;
unsigned long alloc[ITER] PS_ALIGNED;
unsigned long long free_tsc, alloc_tsc;
__thread int thd_local_id;

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
	printf("%s thd %d tot %d avg %llu 99.9 %lu 99 %lu min %lu max %lu\n", label, thd_local_id, 
	       num, sum/num, re[num/1000], re[num/100], re[num-1], re[0]);
	printf("#### %lu\n", re[num/100]);
}

void
consumer(void)
{
	char *s, *h;
	int id = thd_local_id, tf = 0;
	unsigned long i = 0, jump = PS_NUMCORES-1, k = 0, j;
	unsigned long long start, end, tot = 0, mmax, mmin;

	i = id-1;
	mmin = 1000000;
	meas_barrier(PS_NUMCORES);

	while(1) {
		for (j=0; j<NODE_SZ; j++) {
			if (!ring_buffer[i].p[j]) continue;

			s = (char *)(ring_buffer[i].p[j]);
			if (s == (void *)-1) goto c_out;
			ring_buffer[i].p[j] = NULL;
			assert(i == ((int *)s)[0]);
			h = s-sizeof(struct ps_mheader);
			h[0] = 0;
			ps_mem_fence();

			start = ps_tsc();
			ps_slab_free_remote(s);
			end = ps_tsc();
			mmin = end-start;
			if (id == PRINTER && k < ITER && mmin > 200) cost[k++] = mmin;
			tf++;
		}
		i += jump;
		if (i >= RB_SZ) i = id-1;
	}
c_out:
	if (id == PRINTER) {
		out_latency(cost, k, "remote_free");
	}
}

void
producer(void)
{
	void *s;
	unsigned long i = 0, k = 0, j, c = 0;
	unsigned long long start, end, tot = 0, mmax, mmin;

	mmax = 0;
	mmin = 1000000;
	meas_barrier(PS_NUMCORES);
	j = NODE_SZ/2;
	while (1) {
		for (i=0; i<RB_SZ; i++) {
			if (ring_buffer[i].p[j]) continue;
			start = ps_tsc();
			s = ps_slab_alloc_remote();
			end = ps_tsc();
			assert(s);
			((int *)s)[0] = i;
			ps_mem_fence();
			ring_buffer[i].p[j] = s;
			if (c < ITER) alloc[c++] = end-start;
			if ((++k) >= (PS_NUMCORES-1)*ITER) goto p_out;
		}
		j = (j+1)%NODE_SZ;
	}
p_out:
	out_latency(alloc, c, "alloc");
	/* qsort(alloc, k, sizeof(unsigned long), cmpfunc); */
	/* alloc_tsc = tot / ITER; */
	/* int t = tot/TIMER_FREQ; */
	/* printf("remote alloc timer %d avg %llu max %llu %lu %lu min %llu\n", t, alloc_tsc, mmax, alloc[t], alloc[k/100], mmin); */
	/* printf("thd %d alloc tot %d\n", thd_local_id, k+RB_SZ); */
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
	int i, ret, *s, j;
	
	printf("Starting test for remote frees\n");
	for(i=0; i<RB_SZ; i++) {
		for (j=0; j<NODE_SZ; j++) {
			s = (int *)ps_slab_alloc_remote();
			s[0] = i;
			ring_buffer[i].p[j] = (void *)s;
		}
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
	for(i=0; i<RB_SZ; i++) for (j=0; j<NODE_SZ; j++) ring_buffer[i].p[j] = (void *)-1;
	
	for (i = 1; i < PS_NUMCORES; i++) {
		pthread_join(child[i], NULL);
	}
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
