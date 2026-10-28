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

#pragma once

/**
 * @file clogderive.h
 * @brief INTERNAL ONLY. A derive of a clog handle that never ends the
 * process.
 *
 * clog_derive() takes a stale or closed parent handle as a programming
 * error and calls _ccol_fatal_err(). A module that gets a logger handle
 * from its own caller, and has to report a bad handle as an ordinary
 * error, uses this function instead. Checking the handle first and then
 * calling clog_derive() would race with a concurrent clog_close(), so this
 * function resolves and pins the parent and derives under that pin, which
 * gives one answer for one moment. None of this is public: the library
 * never installs this header and doesn't export the function.
 */

#include <clogger.h>
#include <common.h>

/**
 * Derives a new logger from parent the same way clog_derive() does, but
 * reports a failure through *rv instead of ending the process.
 *
 * On success it gives the new handle and sets *rv to ccol_success. It
 * gives CLOG_NULL with *rv set to ccol_invalid_args when parent is
 * CLOG_NULL, stale or closed, and CLOG_NULL with *rv set to
 * ccol_not_enough_memory when the new logger or its handle couldn't be
 * allocated. rv must not be NULL.
 */
clog _clog_derive_checked(clog parent, ccol_retval_t *rv);
