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

// Internal only: Forced fork-handler registration of the modules that other
// modules depend on. Prepare handlers run in the reverse of their registration
// order, and every module registers lazily on first use. A module that holds
// another module's locks inside its own (chttpserver and chttpclient do this
// with cthreadcomm, cthreadpool and clogger) must make those dependencies
// register first, or the two sets of handler locks can nest in opposite orders
// in two forks and deadlock. Call all three before your own ccol__at_fork().
// The defining files include this header too, so a changed signature fails
// the build.

// Makes cthreadcomm (ceventloop and the queues) register its fork handlers now,
// unless it already has. chttpserver and chttpclient call it before their own
// registration, so cthreadcomm's prepare handler runs after theirs.
void cthreadcomm__ensure_atfork_registered_before_caller(void);

// The same for cthreadpool, whose pools chttpserver and chttpclient lock inside
// their own handlers.
void cthrpool__ensure_atfork_registered_before_caller(void);

// The same for clogger, whose loggers chttpserver and chttpclient lock inside
// their own handlers.
void clog__ensure_atfork_registered_before_caller(void);
