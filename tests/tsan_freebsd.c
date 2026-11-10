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

// Linked into every ThreadSanitizer test binary on FreeBSD only (platform.mk,
// CCOL_TSAN_EXTRA_SRC). TSAN_OPTIONS and a suppressions file add to these.
const char *__tsan_default_options(void);
const char *__tsan_default_suppressions(void);

// Gives the default options that the ThreadSanitizer runtime asks for at
// start-up. intercept_tls_get_addr=0 is needed because with the interception
// on, a thread that starts while another forks deadlocks between the loader
// lock and a lock of the runtime's allocator.
const char *__tsan_default_options(void) { return "intercept_tls_get_addr=0"; }

// Gives the default suppressions that the runtime asks for at start-up. libthr
// allocates mutexes and thread structures from a private allocator whose lock
// the runtime can't see, so reuse of its blocks looks like a race.
const char *__tsan_default_suppressions(void) { return "race:__thr_calloc\n"; }
