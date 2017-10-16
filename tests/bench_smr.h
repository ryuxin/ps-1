#ifndef  BENCH_SMR_H
#define  BENCH_SMR_H

#include <ck_brlock.h>
#include <ck_epoch.h>
#include <ck_pr.h>
#include <ck_spinlock.h>
#include <ck_rwlock.h>
#include <ck_pflock.h>

#include <ps_smr.h>
#include <ps_plat.h>

#include <urcu.h>
#define CACHE_LINE PS_CACHE_LINE
#define PAGE_SIZE  PS_PAGE_SIZE
#define NUM_CPU    PS_NUMCORES
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE)))

// 1 -> parsec, 2-> urcu, 3->phase-fair-rwlock, 4->mcslock, 5->ticketlock, 6->epoch, 7->brlock
#define BENCH_OP 4

#if BENCH_OP == 1
struct parsec ps;
PS_PARSLAB_CREATE(bench, CACHE_LINE, PS_PAGE_SIZE)
#elif BENCH_OP == 3
ck_pflock_t pfrwlock = CK_PFLOCK_INITIALIZER;
#elif BENCH_OP == 4
ck_spinlock_mcs_t mcslock = CK_SPINLOCK_MCS_INITIALIZER;
struct mcs_context {
	ck_spinlock_mcs_context_t lock_context;
	char pad[PAGE_SIZE - sizeof(ck_spinlock_mcs_context_t)];
} __attribute__((packed, aligned(PAGE_SIZE)));
struct mcs_context mcslocks[NUM_CPU] __attribute__((aligned(PAGE_SIZE)));
#elif BENCH_OP == 5
ck_spinlock_ticket_t ticketlock = CK_SPINLOCK_TICKET_INITIALIZER;
#elif BENCH_OP == 6
ck_epoch_t global_epoch;
struct epoch_record {
	ck_epoch_record_t record;
	char pad[PAGE_SIZE - sizeof(ck_epoch_record_t)];
} __attribute__((packed, aligned(PAGE_SIZE)));
struct epoch_record epoch_records[NUM_CPU] __attribute__((aligned(PAGE_SIZE)));
#elif BENCH_OP == 7
ck_brlock_t brlock;
struct brlock_reader {
	ck_brlock_reader_t reader;
	char pad[PAGE_SIZE - sizeof(ck_brlock_reader_t)];
} __attribute__((packed, aligned(PAGE_SIZE)));
struct brlock_reader brlock_readers[NUM_CPU] __attribute__((aligned(PAGE_SIZE)));
#endif

static void Init(void)
{
#if BENCH_OP == 1
	printf("parsec\n");
	ps_init(&ps);
	ps_mem_init_bench(&ps);
#elif BENCH_OP == 2
	printf("u-rcu\n");
	rcu_init();
#elif BENCH_OP == 3
	printf("phase fair rwlock\n");
	ck_pflock_init(&pfrwlock);
#elif BENCH_OP == 4
	printf("mcs lock\n");
	ck_spinlock_mcs_init(&mcslock);
#elif BENCH_OP == 5
	printf("ticket lock\n");
	ck_spinlock_ticket_init(&ticketlock);
#elif BENCH_OP == 6
	printf("ck epoch\n");
	ck_epoch_init(&global_epoch);
#elif BENCH_OP == 7
	printf("brlock\n");
	ck_brlock_init(&brlock);
#endif
	return ;
}

#endif /* BENCH_SMR_H */
