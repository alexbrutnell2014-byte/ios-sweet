/*
 * pstack PID [SAMPLES [INTERVAL_MS]]: a sampling profiler without Instruments. First the task's images
 * ("image ADDR PATH", from dyld's all-image infos, the shared cache's dylibs included), then SAMPLES samples
 * INTERVAL_MS apart (default 1, 0): for each thread, "s N t ID R|- NAME pc PC bt ADDR..." with R when the
 * thread was running (on a CPU or runnable), its name (pthread), and its frame-pointer backtrace.
 * From ios-flame's phone/tools/pstack.c (MIT): needs task_for_pid (signed with task_for_pid-allow).
 */
#include <mach/mach.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static task_t t;

static int rd(uint64_t a, void *out, size_t n)
{
	vm_size_t got = 0;
	return vm_read_overwrite(t, (vm_address_t)a, n, (vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n ? 0 : -1;
}

static void images(void)
{
	struct task_dyld_info di;
	mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
	struct dyld_all_image_infos ai;
	if (task_info(t, TASK_DYLD_INFO, (task_info_t)&di, &cnt) != KERN_SUCCESS || rd(di.all_image_info_addr, &ai, sizeof(ai)))
		return;
	printf("slide 0x%lx images %u\n", (unsigned long)ai.sharedCacheSlide, ai.infoArrayCount);
	for (uint32_t i = 0; i < ai.infoArrayCount && i < 4096; i++) {
		struct dyld_image_info ii;
		char path[512] = "";
		if (rd((uint64_t)(uintptr_t)ai.infoArray + i * sizeof(ii), &ii, sizeof(ii)))
			break;
		for (unsigned o = 0; o < sizeof(path) - 1; o += 64) {
			char chunk[64];
			if (rd((uint64_t)(uintptr_t)ii.imageFilePath + o, chunk, sizeof(chunk)))
				break;
			memcpy(path + o, chunk, sizeof(path) - 1 - o < 64 ? sizeof(path) - 1 - o : 64);
			if (memchr(chunk, 0, sizeof(chunk)))
				break;
		}
		path[sizeof(path) - 1] = 0;
		printf("image 0x%llx %s\n", (unsigned long long)(uintptr_t)ii.imageLoadAddress, path);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pstack PID [SAMPLES [INTERVAL_MS]]\n");
		return 2;
	}
	int samples = argc > 2 ? atoi(argv[2]) : 1, interval = argc > 3 ? atoi(argv[3]) : 0;
	kern_return_t kr = task_for_pid(mach_task_self(), atoi(argv[1]), &t);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid: %s\n", mach_error_string(kr));
		return 1;
	}
	images();
	for (int s = 0; s < samples; s++) {
		thread_act_array_t th;
		mach_msg_type_number_t nth;
		if (task_threads(t, &th, &nth) != KERN_SUCCESS)
			return 1;
		for (unsigned i = 0; i < nth; i++) {
			thread_identifier_info_data_t id;
			thread_extended_info_data_t ex;
			mach_msg_type_number_t c1 = THREAD_IDENTIFIER_INFO_COUNT, c2 = THREAD_EXTENDED_INFO_COUNT;
			uint64_t tid = i;
			if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &c1) == KERN_SUCCESS)
				tid = id.thread_id;
			int running = 0;
			char name[64] = "-";
			if (thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&ex, &c2) == KERN_SUCCESS) {
				running = ex.pth_run_state == TH_STATE_RUNNING;
				if (ex.pth_name[0]) {
					snprintf(name, sizeof(name), "%s", ex.pth_name);
					for (char *p = name; *p; p++)
						if (*p == ' ')
							*p = '_';
				}
			}
			arm_thread_state64_t st;
			mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
			if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
				uint64_t pc = arm_thread_state64_get_pc(st) & 0x0000000fffffffffULL;
				uint64_t fp = arm_thread_state64_get_fp(st);
				printf("s %d t %llx %c %s pc 0x%llx bt", s, (unsigned long long)tid, running ? 'R' : '-', name,
				       (unsigned long long)pc);
				for (int d = 0; d < 48 && fp; d++) {
					uint64_t fr[2];
					if (rd(fp, fr, sizeof(fr)) || !fr[1])
						break;
					printf(" 0x%llx", (unsigned long long)(fr[1] & 0x0000000fffffffffULL));
					fp = fr[0];
				}
				printf("\n");
			}
			mach_port_deallocate(mach_task_self(), th[i]);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(*th));
		if (interval)
			usleep(interval * 1000);
	}
	return 0;
}
