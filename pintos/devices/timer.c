#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* OS가 부팅된 이후 경과한 타이머 틱 수입니다. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
extern struct list *get_sleep_list(void);
/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* OS가 부팅된 이후 경과한 타이머 틱 수를 반환한다 */
int64_t
timer_ticks (void) {
	/* 현재 인터럽트 상태를 old_level에 저장한 뒤, 인터럽트를 비활성화한다 */
	enum intr_level old_level = intr_disable ();
	 /* 전역 변수 ticks 값을 t에 저장한다 (critical section 보호) */
	int64_t t = ticks;
	  /* 이전 인터럽트 상태로 복원한다 */
	intr_set_level (old_level);
	/* 컴파일러의 최적화로 인한 순서 변경을 방지한다 */
	barrier ();
	/* 경과한 틱 수 t를 반환한다 */
	return t;
}

/* timer_ticks()가 한 번 반환했던 값을 기반으로, THEN 이후 경과한 타이머 틱 수를 반환한다. */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}


// void timer_sleep(int64_t ticks) {
//     int64_t start = timer_ticks();

//     ASSERT(intr_get_level() == INTR_ON);
//     enum intr_level old_level = intr_disable();  // 인터럽트 끄기

//     if (timer_elapsed(start) < ticks) {
//         thread_sleep(start + ticks); // 인터럽트 꺼진 상태에서 sleep
//     }

//     intr_set_level(old_level);  // 인터럽트 이전 상태로 복구
// }

void
timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();

	ASSERT (intr_get_level () == INTR_ON);
	// while (timer_elapsed (start) < ticks)
	// 	thread_yield ();

	if(timer_elapsed(start) < ticks)
		thread_sleep(start + ticks);
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}




/* 타이머 인터럽트가 발생했을 때 실행되는 핸들러 함수이다. */
// static void
// timer_interrupt (struct intr_frame *args UNUSED) {
//     enum intr_level old_level = intr_disable();  // 인터럽트 비활성화
// 	ticks++;  /* 전체 시스템 tick 수 증가 */
// 	// printf("tick: %lld\n", ticks);
// 	thread_tick (); /* 실행 중인 프로세스의 cpu 사용량 업데이트*/
//     /* 현재시간 불러오는 타이머틱 */
// 	int64_t now =ticks;

// 	/* 슬립 리스트 불러오기 */
// 	struct list *sleep_list = get_sleep_list();
// 	/* 슬립리스트 안에 원소가 비지 않을때까지 반복한다. */
// 	while(!list_empty(sleep_list)){
// 		/* 리스트의 맨 앞 스레드 추출 (wake-up tick이 가장 이른 스레드) */
// 		struct thread *t = list_entry(list_front(sleep_list), struct thread, elem);
// 		/* 아직 깨울 시간이 아니라면 더 이상 검사할 필요 없음 (정렬되어 있기 때문) */
// 		if(t->wakeup_tick > now) break;
// 		/* 깨어날 시간이 된 스레드 → 리스트에서 제거하고 unblock */
// 		list_pop_front(sleep_list);
// 		/* unblock 처리 하고 CPU 제어권을 넘긴다. */
// 		thread_unblock(t);
// 	}
// 	intr_set_level(old_level);
// }
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;		// 전역 tick 수 증가 (현재까지 지난 타이머 틱 수)
	thread_tick();		// 현재 실행 중인 쓰레드의 시간 관련 처리

	// sleep_list를 순회하며 깰 애들 찾기
	while(!list_empty(get_sleep_list())){
		struct list_elem *e  = list_front (get_sleep_list());
		struct thread *t = list_entry(e, struct thread, elem);

		if(t->wakeup_tick <= ticks){
			// wakeup 해야함
			list_pop_front(get_sleep_list());
			thread_unblock(t); 
		} else {
			break;
		}
	}
	
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
