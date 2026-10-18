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
 * @file cqueueowner.h
 * @brief INTERNAL ONLY. Sends by the owner of a queue whose sending is off.
 *
 * Once ccol_circq_disable_sending() or ccol_dynmq_disable_sending() runs,
 * every later send is refused. A module that owns a queue and turns sending
 * off to stop new work may still have to put one last control message behind
 * what the queue already accepted, a flush request that tells it when that
 * work is done for example, so these functions send whether sending is on or
 * off. Keep them for that last control message; anything else sent this way
 * would slip past the switch that the owner turned off on purpose. None of
 * this is public interface and the library never installs this header.
 */

#include <cthreadcomm.h>

/**
 * Sends msg to cq whether sending is on or off, and waits at most
 * timeout_us microseconds for room (0 doesn't wait). On success it owns
 * msg->data, the same way ccol_circq_send_zc() does.
 *
 * @return ccol_success, ccol_timed_out (or ccol_container_full when
 *         timeout_us is 0) when no room appeared in time,
 *         ccol_unexpected_failure when the wait itself failed, or
 *         ccol_invalid_args.
 */
ccol_retval_t ccol_circq_owner_send_zc(ccol_circular_queue *cq,
                                       c_message_t *msg, uint64_t timeout_us);

/**
 * Sends msg to dq whether sending is on or off. On success it owns
 * msg->data, the same way ccol_dynmq_send_zc() does.
 *
 * @return ccol_success, ccol_container_full when the queue holds
 *         ccol_max_elem_count messages, ccol_not_enough_memory, or
 *         ccol_invalid_args.
 */
ccol_retval_t ccol_dynmq_owner_send_zc(ccol_dynamic_queue *dq,
                                       c_message_t *msg);
