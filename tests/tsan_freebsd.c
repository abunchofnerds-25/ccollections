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

/* Linked into every ThreadSanitizer test binary on FreeBSD, and into none
 * elsewhere (platform.mk, CCOL_TSAN_EXTRA_SRC). The runtime asks for these
 * two hooks at start-up; TSAN_OPTIONS and a suppressions file still apply on
 * top of them.
 *
 * intercept_tls_get_addr=0: with the interception on, a thread that starts
 * while another thread forks deadlocks inside the runtime, between the lock
 * of the dynamic loader and a lock of the runtime's allocator. A program of a
 * few lines that only forks and creates threads hangs every run with it, and
 * passes with this option once one thread has exited.
 *
 * race:__thr_calloc: libthr allocates its mutexes and thread structures from
 * a private allocator whose lock the runtime cannot see, so the reuse of one
 * of its blocks by a second thread reads as a race in libthr itself. A
 * program that only creates mutexes and threads in two threads reports it
 * every run. */

const char *__tsan_default_options(void);
const char *__tsan_default_suppressions(void);

const char *__tsan_default_options(void) { return "intercept_tls_get_addr=0"; }

const char *__tsan_default_suppressions(void) { return "race:__thr_calloc\n"; }
