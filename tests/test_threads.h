/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// The number of threads of the test process: /proc/self/task on Linux,
// kern.proc on the BSDs, task_threads() on macOS. Under qemu-user the count
// includes the emulator's threads, so compare with a baseline, never with a
// constant.
#ifndef CCOL_TESTS_TEST_THREADS_H
#define CCOL_TESTS_TEST_THREADS_H

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#endif
#ifdef __OpenBSD__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#ifdef __OpenBSD__
// Reads the kern.proc records of this process into *out: One for the process
// (p_tid -1) and one per thread. test_thread_count() uses it on OpenBSD. It
// gives the record count or -1; the caller frees *out. The buffer has room for
// 16 threads that start between the two sysctl calls.
static inline int test_openbsd_proc_records(struct kinfo_proc **out) {
  int mib[6] = {CTL_KERN,
                KERN_PROC,
                KERN_PROC_PID | KERN_PROC_SHOW_THREADS,
                (int)getpid(),
                (int)sizeof(struct kinfo_proc),
                0};
  size_t len = 0;
  *out = NULL;
  if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0) return -1;
  len += 16 * sizeof(struct kinfo_proc);
  struct kinfo_proc *kp = malloc(len);
  if (!kp) return -1;
  mib[5] = (int)(len / sizeof(struct kinfo_proc));
  if (sysctl(mib, 6, kp, &len, NULL, 0) != 0) {
    free(kp);
    return -1;
  }
  *out = kp;
  return (int)(len / sizeof(struct kinfo_proc));
}
#endif

// Gives the number of threads of the process, or -1 when the system doesn't
// report it. Tests use it to see that a teardown joined every thread.
static inline int test_thread_count(void) {
#ifdef __FreeBSD__
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD,
                (int)getpid()};
  size_t len = 0;
  if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0) return -1;
  len += 16 * sizeof(struct kinfo_proc);
  struct kinfo_proc *kp = malloc(len);
  if (!kp) return -1;
  int n = -1;
  if (sysctl(mib, 4, kp, &len, NULL, 0) == 0)
    n = (int)(len / sizeof(struct kinfo_proc));
  free(kp);
  return n;
#elif defined(__APPLE__)
  thread_act_array_t list;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return -1;
  for (mach_msg_type_number_t i = 0; i < count; i++)
    mach_port_deallocate(mach_task_self(), list[i]);
  vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(list[0]));
  return (int)count;
#elif defined(__OpenBSD__)
  struct kinfo_proc *kp;
  int n = test_openbsd_proc_records(&kp);
  if (n < 0) return -1;
  int threads = 0;
  for (int i = 0; i < n; i++)
    if (kp[i].p_tid != -1) threads++;
  free(kp);
  return threads;
#else
  DIR *d = opendir("/proc/self/task");
  if (!d) return -1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL)
    if (e->d_name[0] != '.') n++;
  closedir(d);
  return n;
#endif
}

#endif
