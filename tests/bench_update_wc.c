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

#include <ck_brlock.h>
#include <ck_epoch.h>
#include <ck_pr.h>
#include <ck_spinlock.h>
#include <ck_rwlock.h>
#include <sys/mman.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <ps_smr.h>
#include <ps_plat.h>

#include <urcu.h>

// 0 -> ll, 1 -> parsec, 2-> urcu, 3->rwlock, 4->mcslock, 5->epoch, 6->brlock
#define BENCH_OP 2

#define CACHE_LINE PS_CACHE_LINE
#define PAGE_SIZE  PS_PAGE_SIZE
#define NUM_CPU    PS_NUMCORES
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE)))
#define TRACE_FILE "/tmp/ll_100p_key"
#define N_OPS (10000000)
#define N_LOG (N_OPS / 1)
#define N_1P  (N_OPS / 100 / NUM_CPU)

__thread int thd_local_id;
static char ops[N_OPS];
unsigned long p99_log[N_OPS] CACHE_ALIGNED;
unsigned long p99_log_r[N_OPS] CACHE_ALIGNED;

#if BENCH_OP == 0
/********************************************************/
/** Following functions/data structures used for tests **/
/********************************************************/

struct list_node {
	struct {
		ck_spinlock_mcs_t l;
		// avoid cache bouncing caused by prefetching! 
		char __padding[4*CACHE_LINE - sizeof(ck_spinlock_mcs_t)];
	} __attribute__((packed));
	char data[32];
	CK_SLIST_ENTRY(list_node) next;
} __attribute__((packed));
struct mcs_locks {
	ck_spinlock_mcs_context_t l_context;
	char __padding[2*CACHE_LINE - sizeof(ck_spinlock_mcs_context_t)];
};

static CK_SLIST_HEAD(test_list, list_node) ll_head = CK_SLIST_HEAD_INITIALIZER(ll_head);
struct mcs_locks mcs_locks[NUM_CPU];
struct mcs_locks mcs_locks_2[NUM_CPU];

void
ll_init(void)
{
	int i;

#define LL_LENGTH 100
#define NODES_PERTHD 2

	for (i = 0; i < (LL_LENGTH); i++) {
		ll_add((void *)((LL_LENGTH/NODES_PERTHD - 1) - i/NODES_PERTHD));
	}
	return;
}

void *
ll_add(void *arg)
{
	struct list_node *n;

	n = q_alloc(sizeof(struct list_node), 1);
	if (unlikely(!n)) {
		printf("Allocation error!\n");
		return NULL;
	}
	assert(!((unsigned long)n % CACHE_LINE));

	/* printf("q alloc n %p\n", n); */
	n->data[0] = (int)arg;
	CK_SLIST_INSERT_HEAD(&ll_head, n, next);

	return (void *)n;
}

void *
ll_remove(void *arg)
{
	int ret;
	struct list_node *n;
	(void)arg;

	n = CK_SLIST_FIRST(&ll_head);

	if (!n) return NULL;

	CK_SLIST_REMOVE_HEAD(&ll_head, next);
	ret = (int)(n->data[0]);
	q_free(n);

	return (void *)ret;
}

void *
ll_traverse(void *arg)
{
	struct list_node *n;
	int i;

	(void)arg;

	i = 0;
	CK_SLIST_FOREACH(n, &ll_head, next) {
		i++;
	}

	return (void *)i;
}

// pick the node to remove, and add a new one;
void *
ll_modify(void *arg)
{
	struct list_node *n, *new, *remove;
	ck_spinlock_mcs_context_t *mcs, *mcs2;
	int i, id, ret;
	int freed = 0;

	id = (int)arg;

	i = 0;
	new = q_alloc(sizeof(struct list_node), 1);
	assert(new);
	new->data[0] = id;
	mcs = &(mcs_locks[id].l_context);
	mcs2 = &(mcs_locks_2[id].l_context);

	CK_SLIST_FOREACH(n, &ll_head, next) {
		/* let's do modification to the list */
		if (i == id*NODES_PERTHD) {
			freed++;
			/* nodes owned by the current thd. */

			/* The simple lock sequence here assumes no
			 * contention -- if there is, the trylock
			 * could fail, and we should roll back in that
			 * case (because the current node might be
			 * freed already). */

			/* take current node lock + next node. */
			ret = ck_spinlock_mcs_trylock(&n->l, mcs);
			assert(ret);

			remove = CK_SLIST_NEXT(n, next);
			assert(remove->data[0] == id);

			ret = ck_spinlock_mcs_trylock(&remove->l, mcs2);
			assert(ret);

			assert(n && remove);
			/* replace remove with new */
			new->next.sle_next = remove->next.sle_next;
			ck_pr_fence_store();
			n->next.sle_next = new;
			/* release the old node. */
			q_free(remove);
			ck_spinlock_mcs_unlock(&remove->l, mcs2);
			ck_spinlock_mcs_unlock(&n->l, mcs);
		}
		i++;
	}
	assert(freed == 1);

	return (void *)i;
}

#elif BENCH_OP == 1
struct parsec ps;
PS_PARSLAB_CREATE(bench, CACHE_LINE, PS_PAGE_SIZE)
void *
nil_call(void *arg)
{
	(void)arg;

	return 0;
}
#elif BENCH_OP == 3
ck_rwlock_t rwlock = CK_RWLOCK_INITIALIZER;
#elif BENCH_OP == 4
ck_spinlock_mcs_t mcslock = CK_SPINLOCK_MCS_INITIALIZER;
struct mcs_context {
	ck_spinlock_mcs_context_t lock_context;
	char pad[PAGE_SIZE - sizeof(ck_spinlock_mcs_context_t)];
} __attribute__((packed, aligned(PAGE_SIZE)));

struct mcs_context mcslocks[NUM_CPU] __attribute__((aligned(PAGE_SIZE)));
#elif BENCH_OP == 5
struct epoch_record {
	ck_epoch_record_t record;
	char pad[PAGE_SIZE - sizeof(ck_epoch_record_t)];
} __attribute__((packed, aligned(PAGE_SIZE)));

struct epoch_record epoch_records[NUM_CPU] __attribute__((aligned(PAGE_SIZE)));
ck_epoch_t global_epoch;
#elif BENCH_OP == 6
struct brlock_reader {
	ck_brlock_reader_t reader;
	char pad[PAGE_SIZE - sizeof(ck_brlock_reader_t)];
} __attribute__((packed, aligned(PAGE_SIZE)));

struct brlock_reader brlock_readers[NUM_CPU] __attribute__((aligned(PAGE_SIZE)));
ck_brlock_t brlock;
#endif

static int cmpfunc(const void * a, const void * b)
{
	return ( *(int*)b - *(int*)a );
}
static void Init(void)
{
#if BENCH_OP == 0
	ll_init();
#elif BENCH_OP == 1
	printf("parsec\n");
	ps_init(&ps);
	ps_mem_init_bench(&ps);
#elif BENCH_OP == 2
	rcu_init();
#elif BENCH_OP == 5
	ck_epoch_init(&global_epoch);
#elif BENCH_OP == 6
	ck_brlock_init(&brlock);
#endif
	return ;
}

static void Init_thd(void)
{
	thd_set_affinity(pthread_self(), thd_local_id);
#if BENCH_OP == 1
	ps_init_period(&ps, 1);
#elif BENCH_OP == 2
	rcu_register_thread();
	rcu_init();
#elif BENCH_OP == 5
	ck_epoch_register(&global_epoch, &(epoch_records[thd_local_id].record));
#elif BENCH_OP == 6
	ck_brlock_read_register(&brlock, &(brlock_readers[thd_local_id].reader));
#endif
	return ;
}

void bench_read(void) {
	int i, id, ret = 0;
	unsigned long long s1, e1, tot = 0, cost;
	
	(void)ret;
	id = thd_local_id;	

	for (i = 0 ; i < N_OPS; i++) {
		s1 = ps_tsc();
#if BENCH_OP == 0
		ps_enter(&ps);
		ret = ll_traverse();
		ps_exit(&ps);
		assert(ret == LL_LENGTH);
#elif BENCH_OP == 1
		ps_enter(&ps);
		ret = nil_call(NULL);
		ps_exit(&ps);
#elif BENCH_OP == 2
		rcu_read_lock();
		rcu_read_unlock();
#elif BENCH_OP == 3
		ck_rwlock_read_lock(&rwlock);
		ck_rwlock_read_unlock(&rwlock);
#elif BENCH_OP == 4
		ck_spinlock_mcs_lock(&mcslock, &(mcslocks[id].lock_context));
		ck_spinlock_mcs_unlock(&mcslock, &(mcslocks[id].lock_context));
#elif BENCH_OP == 5
		ck_epoch_begin(&global_epoch, &(epoch_records[id].record));
		ck_epoch_end(&global_epoch, &(epoch_records[id].record));
#elif BENCH_OP == 6
		ck_brlock_read_lock(&brlock, &(brlock_readers[id].reader));
		ck_brlock_read_unlock(&(brlock_readers[id].reader));
#endif
		e1 = ps_tsc();
		if (id == 1) {
		  cost = e1-s1;
		  p99_log_r[i] = cost;
		  tot += (unsigned long long)cost;
		}
	}
	if (id == 1) {
		qsort(p99_log_r, N_OPS, sizeof(unsigned long), cmpfunc);
		printf("thd %d read max %lu 99 %lu 99.9 %lu 99.99 %lu avg %llu\n", 
		       id, p99_log_r[0], p99_log_r[N_OPS/100], p99_log_r[N_OPS/1000], p99_log_r[N_OPS/10000], tot/N_OPS);
	}

	return;
}

void bench_update(void) {
	int i, id, ret = 0;
	unsigned long long s1, e1, cost, tot = 0;
	void *last_alloc;
	
	(void)ret;
	id = thd_local_id;

	for (i = 0 ; i < N_LOG; i++) {
#if BENCH_OP == 1
		last_alloc = ps_mem_alloc_bench();
		assert(last_alloc);
		ps_mem_free_bench(last_alloc);
#endif		
		s1 = ps_tsc();
#if BENCH_OP == 0
		ps_enter(&ps);
		ll_modify(id);
		ps_exit(&ps);
#elif BENCH_OP == 1
		ps_quiesce_bench();
#elif BENCH_OP == 2
		synchronize_rcu();
#elif BENCH_OP == 3
		ck_rwlock_write_lock(&rwlock);
		ck_rwlock_write_unlock(&rwlock);
#elif BENCH_OP == 4
		ck_spinlock_mcs_lock(&mcslock, &(mcslocks[id].lock_context));
		ck_spinlock_mcs_unlock(&mcslock, &(mcslocks[id].lock_context));
#elif BENCH_OP == 5
		ck_epoch_synchronize(&global_epoch, &(epoch_records[id].record));
#elif BENCH_OP == 6
		ck_brlock_write_lock(&brlock);
		ck_brlock_write_unlock(&brlock);
#endif
		e1 = ps_tsc();
		cost = e1-s1;
		tot += (unsigned long long)cost;
		p99_log[i] = cost;
	}
	qsort(p99_log, N_LOG, sizeof(unsigned long), cmpfunc);
	printf("thd %d update max %lu 99 %lu 99.9 %lu 99.99 %lu avg %llu\n", 
	       id, p99_log[0], p99_log[N_LOG/100], p99_log[N_LOG/1000], p99_log[N_LOG/10000], tot/N_LOG);
	return;
}

void * 
worker_read(void *arg)
{	
	thd_local_id = (int)arg;
	Init_thd();	

	meas_barrier(PS_NUMCORES);
	bench_read();
	meas_barrier(PS_NUMCORES);

	return 0;
}

void * 
worker_update(void *arg)
{	
	thd_local_id = (int)arg;
	Init_thd();	

	meas_barrier(PS_NUMCORES);
	bench_update();
	meas_barrier(PS_NUMCORES);

	return 0;
}

void set_smp_affinity()
{
	char cmd[64];
	/* everything done is the python script. */
	sprintf(cmd, "python set_smp_affinity.py %d %d", 40, getpid());
	system(cmd);
}

int main()
{
	int i, ret;
	pthread_t thds[NUM_CPU];

	set_smp_affinity();
	ret = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (ret) {
		printf("cannot lock memory %d... exit.\n", ret);
		exit(-1);
	}
	Init();
	thd_local_id = 0;
	thd_set_affinity(pthread_self(), 0);
	printf("%d cores\n", NUM_CPU);

	for (i = 1; i < NUM_CPU; i++) {
		ret = pthread_create(&thds[i], 0, worker_read, (void *)i);
		if (ret) exit(-1);
	}
	usleep(50000);

	worker_update((void *)0);

	for (i = 1; i < NUM_CPU; i++) {
		pthread_join(thds[i], (void *)&ret);
	}

	return 0;
}
