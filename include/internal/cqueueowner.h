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

// Internal only. Sends by the owner of a queue whose sending is off. After
// ccircq_disable_sending() or cdynmq_disable_sending() every send is refused,
// but an owner that turned sending off to stop new work may have to put one
// last control message behind what the queue already accepted (a flush request,
// for example). These functions send whether sending is on or off. Use them for
// that last control message only, since anything else sent this way slips past
// the switch the owner turned off on purpose. Never installed.

#include <cthreadcomm.h>

// Sends msg to cq whether sending is on or off, for the owner's last control
// message after it turned sends off. It waits at most timeout_us microseconds
// for room (0 doesn't wait). On success the queue owns msg->data, as with
// ccircq_send(). It gives ccol_success, ccol_timed_out when no room appeared in
// time (ccol_container_full when timeout_us is 0), ccol_unexpected_failure when
// the wait itself failed, or ccol_invalid_args.
ccol_retval ccircq_owner_send(ccircq *cq, cmessage *msg, uint64_t timeout_us);

// Sends msg to dq whether sending is on or off, for the owner's last control
// message after it turned sends off. On success the queue owns msg->data, as
// with cdynmq_send(). It gives ccol_success, ccol_container_full at
// ccol_max_elem_count messages, ccol_not_enough_memory, or ccol_invalid_args.
ccol_retval cdynmq_owner_send(cdynmq *dq, cmessage *msg);
