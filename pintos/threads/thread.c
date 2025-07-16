#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* 슬립 리스트 선언 */
static struct list sleep_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4		  /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	/* Init the globla thread context */
	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&sleep_list); // 초기화
	list_init(&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void)
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 PRIORITY를 가지고 NAME이라는 이름의 새 커널 스레드를 생성합니다. 
이 스레드는 AUX를 인자로 전달하며 FUNCTION을 실행하고, 준비 큐에 추가됩니다. 
새 스레드의 스레드 식별자를 반환하며, 생성에 실패하면 TID_ERROR를 반환합니다.
만약 thread_start()가 호출되었다면, 새 스레드는 thread_create()가 반환되기 전에 스케줄될 수 있습니다. 
심지어 thread_create()가 반환되기 전에 종료될 수도 있습니다. 
반대로, 원래 스레드는 새 스레드가 스케줄되기 전까지 얼마든지 실행될 수 있습니다. 
순서를 보장해야 한다면 세마포어 또는 다른 형태의 동기화를 사용하세요.
제공된 코드는 새 스레드의 'priority' 멤버를 PRIORITY로 설정하지만, 실제 우선순위 스케줄링은 구현되어 있지 않습니다. 
우선순위 스케줄링은 문제 1-3의 목표입니다. */
void thread_test_preemption(void)
{
	if (!list_empty(&ready_list) && thread_current()->priority 
        < list_entry(list_front(&ready_list), struct thread, elem)->priority)
        thread_yield();
}

tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	// 함수 인자가 유효한지 확인
	ASSERT(function != NULL);

	// 커널 힙에서 새로운 struct thread 할당 (1page)
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;		// 실패 시 에러 반환

	// 스레드 구조체 초기화
	init_thread(t, name, priority);

	// 고유한 스레드 ID 부여
	tid = t->tid = allocate_tid();

	// 이 스레드가 실행할 커널 함수 설정
	// 유저함수 function(aux)fmf kernel_thread 통해 호출
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;

	// 세그먼트 셀렉터 설정 (Ring 0 세그먼트)
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;

	// 인터럽트 활성화 상태로 설정
	t->tf.eflags = FLAG_IF;		// 인터럽트 enable

	// 준비된 상태로 변경 후 ready_list에 삽입
	thread_unblock(t);

	/* 
		현재 실행 중인 스레드와 새로 삽입된 스레드의 우선순위를 비교합니다. 
		새로 들어오는 스레드의 우선순위가 더 높으면 CPU를 양보합니다 
	*/
    thread_test_preemption();
	// 새로 생성된 스레드의 tid 반환
	return tid;
}

/* 
	현재 스레드를 재웁니다(sleep). thread_unblock()에 의해 깨어날 때까지 다시 스케줄되지 않습니다.
	이 함수는 인터럽트가 꺼진 상태에서 호출되어야 합니다. 
	일반적으로 synch.h에 있는 동기화 기본 요소를 사용하는 것이 더 나은 방법입니다.
*/
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}


// 우선순위 정렬
bool priority_more(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
	struct thread *t1 = list_entry(a, struct thread, elem);
	struct thread *t2 = list_entry(b, struct thread, elem);

	return t1->priority > t2->priority;
}

bool thread_compare_priority(struct list_elem *l, struct list_elem *s, void *aux)
{
	return list_entry(l, struct thread, elem)->priority > list_entry(s, struct thread, elem)->priority;
}

/* 세마비교 */
bool
sema_compare_priority (const struct list_elem *l, const struct list_elem *s, void *aux)
{
	struct semaphore_elem *l_sema = list_entry (l, struct semaphore_elem, elem);
	struct semaphore_elem *s_sema = list_entry(s, struct semaphore_elem, elem);

	struct list *waiter_i_sema = &(l_sema->semaphore.waiters);
	struct list *waiter_s_sema = &(s_sema->semaphore.waiters);

	return list_entry(list_begin(waiter_i_sema), struct thread, elem)->priority >list_entry(list_begin(waiter_s_sema),struct thread, elem)->priority;

}

/* 
	차단(blocked)된 스레드 T를 실행 준비(ready-to-run) 상태로 전환합니다. 
	만약 T가 차단된 상태가 아니라면 오류입니다. (현재 실행 중인 스레드를 준비 상태로 만들려면 thread_yield()를 사용하세요.)
	이 함수는 현재 실행 중인 스레드를 선점(preempt)하지 않습니다. 
	이는 중요할 수 있습니다. 호출자가 직접 인터럽트를 비활성화했다면, 스레드를 원자적으로 차단 해제하고 다른 데이터를 업데이트할 수 있을 것이라고 예상할 수 있기 때문입니다.
*/
void thread_unblock(struct thread *t)
{
	
	enum intr_level old_level;

	ASSERT(is_thread(t));

	// 인터럽트 플래그 저장 후 인터럽트 비활성화(임계영역 진입)
	old_level = intr_disable();

	// 스레드 상태가 BLOCKED인지 확인(다른 상태면 오류)
	ASSERT(t->status == THREAD_BLOCKED);

	// 준비된 리스트의 맨 뒤에 스레드 삽입
	//list_push_back(&ready_list, &t->elem);

	list_insert_ordered(&ready_list, &t->elem, thread_compare_priority, 0);

	// 스레드 상태를 READY로 변경
	t->status = THREAD_READY;

	// 인터럽트 상태 복원(임계 영역 탈출)
	intr_set_level(old_level);
}


/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/*
	CPU를 양보합니다.
	현재 스레드는 재워지지 않으며, 스케줄러의 재량에 따라 즉시 다시 스케줄될 수 있습니다.
*/
void thread_yield(void)
{
	// 현재 실행 중인 스레드 
	struct thread *curr = thread_current();
	enum intr_level old_level;

	// 인터럽트 핸들러 내부에서는 호출 금지
	ASSERT(!intr_context());

	// 인터럽트 비활성화 (임계영역 진입)
	old_level = intr_disable();

	// idle_thread(유효 스레드)는 ready_list에 들어가지 않음
	// 현재 스레드를 ready_list에 추가
	if (curr != idle_thread) 
		list_insert_ordered(&ready_list, &curr->elem, thread_compare_priority, 0);

	// 스레드 상태를 READY로 바꾸고 스케줄러 호출
	do_schedule(THREAD_READY);

	// 인터럽트 상태 복원(임계 영역 종료)
	intr_set_level(old_level);
}

/* 비교 함수 */
bool check_struct(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
    /* a와 b가 각각 가리키는 thread 구조체를 얻는다 */
    struct thread *thread_a = list_entry(a, struct thread, elem);
    struct thread *thread_b = list_entry(b, struct thread, elem);
    
    /* wakeup_tick이 더 작은 스레드를 우선 순위로 하여 정렬한다 */
    return thread_a->wakeup_tick < thread_b->wakeup_tick;
}

/* 구현해야 할 thread_sleep 함수 */
void thread_sleep(int64_t ticks)
{
	/* 현재 스레드가 유휴 스레드가 아닌 경우, = idle 스레드
	호출자 스레드의 상태를 BLOCKED로 변경합니다,
	로컬 틱을 저장하여 깨웁니다,
	필요한 경우 글로벌 틱을 업데이트합니다,
	그리고 schedule() 을 호출합니다.*/
	/* 스레드 목록을 조작할 때는 인터럽트를 비활성화하세요! */
	enum intr_level old_level; // 인터럽트 비활성화

	struct thread *curr; // 현재 스레드
	old_level = intr_disable();
	curr = thread_current();

	curr->wakeup_tick = ticks; // 매개변수로 받은 시간

	//list_push_back(&sleep_list, &curr->elem); // 맨 뒤에 삽입
	list_insert_ordered (&sleep_list, &curr->elem, check_struct, NULL);

	thread_block();
	
	intr_set_level(old_level); // 인터럽트 복원
}

void thread_wakeup(int64_t current_ticks)
{
	struct list_elem *e = list_begin(&sleep_list);
	while (e != list_end(&sleep_list))
	{
		struct thread *t = list_entry(e, struct thread, elem);
		if (t->wakeup_tick <= current_ticks)
		{
			e = list_remove(e);
			thread_unblock(t);
		}
		else{
			e = list_next(e);
		}
	}
}

int get_effective_priority(struct thread *t){
	// 1. dontaions가 비었을 경우
	if(list_empty(&t->donations))
		return t->priority;
		
	// 2. 변수 새로이 설정
	int max_priority = t->priority;
	
	// 3. dotaions 순회하며 max_priority 업데이트
	struct list_elem *e;
	for (e = list_begin (&t->donations); e != list_end (&t->donations); e = list_next (e)){
		struct thread *donor = list_entry(e, struct thread, donations_elem);

		int donor_effective_priority = get_effective_priority(donor);

		if(max_priority < donor_effective_priority){
			max_priority = donor_effective_priority;
		}
	}
	return max_priority;
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정 */
void thread_set_priority(int new_priority)
{
	// 현재 스레드의 priority를 매개 변수 new_priority로 설정
	thread_current()->priority = new_priority;

	// /*
	// 	현재 스레드보다 더 높은 우선 순위가 ready_list에 존재하면 양보
	// */
	// enum intr_level old_level = intr_disable();
	// if(!list_empty(&ready_list)){
	// 	struct thread *t = list_entry(list_front(&ready_list), struct thread, elem);

	// 	if(get_effective_priority(thread_current()) < get_effective_priority(t)){
	// 		thread_yield();
	// 	}
	// }
	// intr_set_level(old_level);
	refresh_priority();
	thread_test_preemption();
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	/* 도네이션 초기화 */
	t->init_priority = priority;
	t->waiting_lock = NULL;
	list_init(&t->donations);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"
		"movq 0(%%rsp),%%r15\n"
		"movq 8(%%rsp),%%r14\n"
		"movq 16(%%rsp),%%r13\n"
		"movq 24(%%rsp),%%r12\n"
		"movq 32(%%rsp),%%r11\n"
		"movq 40(%%rsp),%%r10\n"
		"movq 48(%%rsp),%%r9\n"
		"movq 56(%%rsp),%%r8\n"
		"movq 64(%%rsp),%%rsi\n"
		"movq 72(%%rsp),%%rdi\n"
		"movq 80(%%rsp),%%rbp\n"
		"movq 88(%%rsp),%%rdx\n"
		"movq 96(%%rsp),%%rcx\n"
		"movq 104(%%rsp),%%rbx\n"
		"movq 112(%%rsp),%%rax\n"
		"addq $120,%%rsp\n"
		"movw 8(%%rsp),%%ds\n"
		"movw (%%rsp),%%es\n"
		"addq $32, %%rsp\n"
		"iretq"
		: : "g"((uint64_t)tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile(
		/* Store registers that will be used. */
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
		/* Fetch input once */
		"movq %0, %%rax\n"
		"movq %1, %%rcx\n"
		"movq %%r15, 0(%%rax)\n"
		"movq %%r14, 8(%%rax)\n"
		"movq %%r13, 16(%%rax)\n"
		"movq %%r12, 24(%%rax)\n"
		"movq %%r11, 32(%%rax)\n"
		"movq %%r10, 40(%%rax)\n"
		"movq %%r9, 48(%%rax)\n"
		"movq %%r8, 56(%%rax)\n"
		"movq %%rsi, 64(%%rax)\n"
		"movq %%rdi, 72(%%rax)\n"
		"movq %%rbp, 80(%%rax)\n"
		"movq %%rdx, 88(%%rax)\n"
		"pop %%rbx\n" // Saved rcx
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n" // Saved rbx
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n" // Saved rax
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n" // read the current rip.
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n" // rip
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		: : "g"(tf_cur), "g"(tf) : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

static void
schedule(void)
{
	struct thread *curr = running_thread();
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}

void donation_priority(void)
{
	int depth;
	struct thread *cur = thread_current();

	for(depth = 0; depth < 8; depth ++) /* 최대 깊이는 8이여야함 */
	{
		if(!cur->waiting_lock) break;
		struct thread *holder = cur->waiting_lock->holder;
		holder->priority = cur->priority;
		cur = holder;
	}
}

/* 스레드 정렬 함수 */
bool thread_compare_donate_priority(const struct list_elem *l, const struct list_elem *s, void *aux)
{
	return list_entry(l, struct thread, donations_elem)->priority > list_entry(s, struct thread, donations_elem)->priority;
}

/* 도네이션 리스트에서 스레드 들을 지우는 함수 */
void remove_with_lock(struct  lock *lock)
{
	struct list_elem *e;
	struct thread *cur = thread_current();

	for(e=list_begin(&cur->donations); e != list_end(&cur->donations); e=list_next(e))
	{
		struct thread *t = list_entry(e,struct thread, donations_elem);
		if(t->waiting_lock == lock){
			list_remove(&t->donations_elem);
		}
	}
}

/* priority 재설정 함수 */
void refresh_priority(void)
{
	/* 현재 실행중인 스레드를 불러 온다. */
	struct thread *cur = thread_current();

	/*현재 스레드의 우선순위를 init_priority에 저장한다. */
	cur->priority = cur->init_priority;
	/* 현재스레드의 도네이션이 비어있지 않았을 경우 */
	if(!list_empty(&cur->donations)) {
		/* 현재스레드의 도네이션 리스트를 우선순위 순서로 맞춘다. */
		list_sort (&cur->donations, thread_compare_donate_priority, 0);
		/* 우선순위가 높은 쓰레드 정보를 프론트에 넣는다. */
		struct thread *front = list_entry(list_front(&cur->donations), struct thread, donations_elem);
		/* 프론트 쓰레드가 현재 스레드보다 우선순위가 높을경우 */
		if (front->priority > cur->priority){
			/* 현재스레드는 프론트 스레드가 된다. */
			cur->priority = front->priority;
		}
	}
}
