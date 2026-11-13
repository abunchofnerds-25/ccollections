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

// Internal: A derive of a clog handle that never ends the process. Not
// installed and not exported.
#include <clogger.h>
#include <common.h>

// Derives a logger from parent like clog_derive(), but reports a failure
// through *rv instead of ending the process. A module that gets a logger handle
// from its own caller (chttpserver, chttpclient) needs this to treat a bad
// handle as an ordinary error; checking first and then calling clog_derive()
// would race a concurrent clog_close(), so this resolves, pins and derives in
// one step. Gives the new handle with *rv = ccol_success, or CLOG_NULL with *rv
// = ccol_invalid_args (parent CLOG_NULL, stale or closed) or
// ccol_not_enough_memory. rv must not be NULL.
clog clog__derive_checked(clog parent, ccol_retval *rv);
