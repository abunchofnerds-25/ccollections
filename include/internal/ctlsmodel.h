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

/* The thread-local storage model of the library's thread-local fast paths
 * (the per-thread cache of cmempool, the pin stripe of cpintable, the date
 * cache of chttpserver and the pipe flag of clogger).
 *
 * initial-exec resolves a thread-local at load time, so an access is one
 * load relative to the thread pointer. It also marks the whole library as
 * needing static thread-local storage, so a dlopen() of it must fit the
 * library's thread-local block into the reserve that the loader keeps for
 * late loads. glibc keeps about 1.6 KiB there, which this library fits.
 * FreeBSD keeps 128 bytes unless the environment of the program sets
 * LD_STATIC_TLS_EXTRA, and this library needs 384 or more there. The general
 * model is resolved through __tls_get_addr on each access, and a dlopen()
 * never fails for it; it costs the thread-safe path of cmempool a third to
 * two thirds of its throughput (more on FreeBSD), which makes the pool
 * slower than malloc. TLS descriptors (-mtls-dialect=gnu2) recover about a
 * third of that on Linux, and FreeBSD's loader on x86-64 refuses them.
 *
 * CCOL_MEMPOOL_DYNAMIC_TLS selects the general model when it is 1 and
 * initial-exec when it is 0. A build that does not set it gets initial-exec
 * with glibc and on FreeBSD, and the general model everywhere else. This file
 * must be included after a header of the C library, which is what defines
 * __GLIBC__. */

#ifndef CCOL_INTERNAL_CTLSMODEL_H
#define CCOL_INTERNAL_CTLSMODEL_H

#ifndef CCOL_MEMPOOL_DYNAMIC_TLS
#if defined(__GLIBC__) || defined(__FreeBSD__)
#define CCOL_MEMPOOL_DYNAMIC_TLS 0
#else
#define CCOL_MEMPOOL_DYNAMIC_TLS 1
#endif
#endif

/* macOS releases the thread-local storage (__thread) of an exiting thread
 * before it runs the destructors of pthread keys. A destructor that reads a
 * __thread variable there reads a new, zeroed copy, so state that it must
 * drain is lost. Under _CCOL_EMULATE_DARWIN_TLS, every module whose key
 * destructor drains per-thread state keeps that state in a heap block whose
 * pointer is the value of the key, and the destructor drains the block that
 * it receives as its argument. Elsewhere the state stays in __thread
 * variables. The switch is on by itself on macOS; the test suites of Linux
 * and FreeBSD set it to run the macOS form. */
#if defined(__APPLE__) && !defined(_CCOL_EMULATE_DARWIN_TLS)
#define _CCOL_EMULATE_DARWIN_TLS 1
#endif

#endif /* CCOL_INTERNAL_CTLSMODEL_H */
