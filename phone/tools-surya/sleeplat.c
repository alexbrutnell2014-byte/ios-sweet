/*
 * sleeplat [N]: how long a short sleep really takes (the kernel's GPU fence wait sleeps 20 us at a time).
 * mach_wait_until(now + D) for D = 20 us, 100 us, 1 ms, N times each (default 300), on a normal thread and on a
 * time-constraint (realtime) thread like Core Animation's render thread: min / median / p90 / max overshoot in us.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static mach_timebase_info_data_t tb;
static int N = 300;

static uint64_t us2abs(double us) { return (uint64_t)(us * 1000.0 * tb.denom / tb.numer); }
static double abs2us(uint64_t a) { return (double)a * tb.numer / tb.denom / 1000.0; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void run(const char *who)
{
	const double ds[] = { 20, 100, 1000 };
	double *v = calloc(N, sizeof(double));
	for (int k = 0; k < 3; k++) {
		for (int i = 0; i < N; i++) {
			uint64_t t0 = mach_absolute_time(), dl = t0 + us2abs(ds[k]);
			mach_wait_until(dl);
			v[i] = abs2us(mach_absolute_time() - dl);
		}
		qsort(v, N, sizeof(double), cmp);
		printf("%-9s sleep %5.0f us: overshoot min %7.1f  median %7.1f  p90 %7.1f  max %7.1f us\n", who, ds[k], v[0], v[N / 2],
		       v[N * 9 / 10], v[N - 1]);
	}
	free(v);
}

static void *rt(void *arg)
{
	(void)arg;
	thread_time_constraint_policy_data_t p = { .period = (uint32_t)us2abs(16667), .computation = (uint32_t)us2abs(5000),
		.constraint = (uint32_t)us2abs(16667), .preemptible = 1 };
	kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
					     THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	run(kr == KERN_SUCCESS ? "realtime" : "rt-FAILED");
	return NULL;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		N = atoi(argv[1]);
	mach_timebase_info(&tb);
	run("normal");
	pthread_t t;
	pthread_create(&t, NULL, rt, NULL);
	pthread_join(t, NULL);
	return 0;
}
