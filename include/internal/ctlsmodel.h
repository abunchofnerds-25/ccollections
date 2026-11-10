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

// The thread-local storage model of the per-thread fast paths (the cmempool
// thread cache, the cpintable pin stripe, the chttpserver date cache and the
// clogger pipe flag). initial-exec makes each access one load, but it marks
// the whole library as needing static TLS, and a dlopen() must then fit our
// TLS block into the small reserve the loader keeps (glibc has room; FreeBSD
// needs LD_STATIC_TLS_EXTRA). The general model never fails a dlopen() but
// makes the thread-safe cmempool path slower than malloc.
//
// CMEMPOOL_DYNAMIC_TLS 1 picks the general model and 0 picks initial-exec.
// When it isn't set we use initial-exec with glibc and on FreeBSD, and the
// general model everywhere else. Include this file after a C library header,
// because that header defines __GLIBC__.

#ifndef CCOL_INTERNAL_CTLSMODEL_H
#define CCOL_INTERNAL_CTLSMODEL_H

#ifndef CMEMPOOL_DYNAMIC_TLS
#if defined(__GLIBC__) || defined(__FreeBSD__)
#define CMEMPOOL_DYNAMIC_TLS 0
#else
#define CMEMPOOL_DYNAMIC_TLS 1
#endif
#endif

// macOS frees the __thread storage of an exiting thread before the pthread
// key destructors run, and OpenBSD's emulated TLS is gone after one round of
// destructors, so a destructor that reads a __thread variable there sees a
// zeroed copy. Under CCOL__EMULATE_DARWIN_TLS every module that drains
// per-thread state from a key destructor keeps that state in a heap block whose
// pointer is the key's value. It turns on by itself on macOS and OpenBSD; the
// Linux and FreeBSD suites set it to test that form.
#if (defined(__APPLE__) || defined(__OpenBSD__)) && \
    !defined(CCOL__EMULATE_DARWIN_TLS)
#define CCOL__EMULATE_DARWIN_TLS 1
#endif

#endif /* CCOL_INTERNAL_CTLSMODEL_H */
