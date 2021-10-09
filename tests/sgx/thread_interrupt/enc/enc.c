// Copyright (c) Open Enclave SDK contributors.
// Licensed under the MIT License.

#include <openenclave/corelibc/string.h>
#include <openenclave/enclave.h>
#include <openenclave/internal/print.h>
#include <openenclave/internal/raise.h>
#include <openenclave/internal/sgx/td.h>
#include <openenclave/internal/tests.h>
#include "thread_interrupt_t.h"

#include <stdio.h>

typedef struct _thread_info_nonblocking_t
{
    int tid;
    oe_sgx_td_t* td;
    int lock;
} thread_info_t;

static thread_info_t _thread_info_nonblocking;
static thread_info_t _thread_info_blocking;
static volatile int _handler_entered;

uint64_t thread_interrupt_handler(oe_exception_record_t* exception_record)
{
    int self_tid = 0;

    _handler_entered = 1;

    OE_TEST(exception_record->code == OE_EXCEPTION_UNKNOWN);

    host_get_tid(&self_tid);
    OE_TEST(_thread_info_nonblocking.tid == self_tid);

    printf("(tid=%d) thread is interrupted...\n", self_tid);

    OE_TEST(
        _thread_info_nonblocking.td->state ==
        OE_TD_STATE_SECOND_LEVEL_EXCEPTION_HANDLING);

    // The handler is responsible for updating the state
    _thread_info_nonblocking.td->state = OE_TD_STATE_RUNNING_NONBLOCKING;
    _thread_info_nonblocking.lock = 0;

    return OE_EXCEPTION_CONTINUE_EXECUTION;
}

void enc_run_thread_nonblocking(int tid)
{
    oe_result_t result = OE_OK;
    int self_tid = 0;
    host_get_tid(&self_tid);

    OE_TEST(tid == self_tid);
    printf("(tid=%d) non-blocking thread is running...\n", self_tid);

    OE_CHECK(
        oe_add_vectored_exception_handler(false, thread_interrupt_handler));

    _thread_info_nonblocking.tid = tid;
    _thread_info_nonblocking.td = oe_sgx_get_td();
    // Validate the default state
    OE_TEST(_thread_info_nonblocking.td->state == OE_TD_STATE_ENTERED);
    // Change the state to RUNNING_NONBLOCKING so the thread
    // can serve an interrupt request
    _thread_info_nonblocking.td->state = OE_TD_STATE_RUNNING_NONBLOCKING;
    // Ensure the order of setting the lock
    asm volatile("" ::: "memory");
    _thread_info_nonblocking.lock = 1;
    while (_thread_info_nonblocking.lock)
    {
        asm volatile("pause" ::: "memory");
    }

    OE_TEST(
        _thread_info_nonblocking.td->state == OE_TD_STATE_RUNNING_NONBLOCKING);
    printf("(tid=%d) non-blocking thread is exiting...\n", self_tid);
done:
    return;
}

void enc_run_thread_blocking(int tid)
{
    oe_result_t result = OE_OK;
    int self_tid = 0;
    host_get_tid(&self_tid);

    OE_TEST(tid == self_tid);
    printf("(tid=%d) blocking thread is running...\n", self_tid);

    OE_CHECK(
        oe_add_vectored_exception_handler(false, thread_interrupt_handler));

    _thread_info_blocking.tid = tid;
    _thread_info_blocking.td = oe_sgx_get_td();
    // Validate the default state
    OE_TEST(_thread_info_blocking.td->state == OE_TD_STATE_ENTERED);
    // Change the state to RUNNING_NONBLOCKING so the thread
    // can serve an interrupt request
    _thread_info_blocking.td->state = OE_TD_STATE_RUNNING_BLOCKING;
    // Ensure the order of setting the lock
    asm volatile("" ::: "memory");
    _thread_info_blocking.lock = 1;
    while (_thread_info_blocking.lock)
    {
        asm volatile("pause" ::: "memory");
    }

    printf("(tid=%d) blocking thread is exiting...\n", self_tid);
done:
    return;
}

void enc_thread_interrupt_nonblocking(void)
{
    oe_result_t result;
    int tid = 0;

    host_get_tid(&tid);
    OE_TEST(tid != 0);

    /* Test interrupting a non-blocking thread */
    printf("(tid=%d) Create a non-blocking thread...\n", tid);
    result = host_create_thread(0 /* blocking */);
    if (result != OE_OK)
        return;

    while (!_thread_info_nonblocking.lock)
    {
        asm volatile("pause" ::: "memory");
    }

    OE_TEST(_thread_info_nonblocking.tid != 0);
    host_sleep_msec(30);

    printf(
        "(tid=%d) Sending interrupt to (td=0x%lx, tid=%d)...\n",
        tid,
        (uint64_t)_thread_info_nonblocking.td,
        _thread_info_nonblocking.tid);
    host_send_interrupt(_thread_info_nonblocking.tid);

    host_join_thread();
}

void enc_thread_interrupt_blocking(void)
{
    oe_result_t result;
    int tid = 0;
    int retry = 0;

    host_get_tid(&tid);
    OE_TEST(tid != 0);

    /* Test interrupting a blocking thread */
    printf("(tid=%d) Create a blocking thread...\n", tid);
    result = host_create_thread(1 /* blocking */);
    if (result != OE_OK)
        return;

    while (!_thread_info_blocking.lock)
    {
        asm volatile("pause" ::: "memory");
    }

    OE_TEST(_thread_info_blocking.tid != 0);
    host_sleep_msec(30);

    _handler_entered = 0;
    while (!_handler_entered)
    {
        printf(
            "(tid=%d) Sending interrupt to (td=0x%lx, tid=%d)...%d\n",
            tid,
            (uint64_t)_thread_info_blocking.td,
            _thread_info_blocking.tid,
            ++retry);
        host_send_interrupt(_thread_info_blocking.tid);

        if (retry == 10)
        {
            printf(
                "Unable to interrrupt (tid=%d), aborting\n",
                _thread_info_blocking.tid);
            oe_abort();
        }
        host_sleep_msec(30);
    }

    host_join_thread();
}

OE_SET_ENCLAVE_SGX(
    1,    /* ProductID */
    1,    /* SecurityVersion */
    true, /* Debug */
    1024, /* NumHeapPages */
    1024, /* NumStackPages */
    2);   /* NumTCS */
