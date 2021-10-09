// Copyright (c) Open Enclave SDK contributors.
// Licensed under the MIT License.

#include <openenclave/corelibc/string.h>
#include <openenclave/enclave.h>
#include <openenclave/internal/print.h>
#include <openenclave/internal/raise.h>
#include <openenclave/internal/sgx/td.h>
#include <openenclave/internal/tests.h>
#include "td_state_t.h"

#include <stdio.h>

#define OE_EXPECT(a, b)                                              \
    do                                                               \
    {                                                                \
        uint64_t value = (uint64_t)(a);                              \
        uint64_t expected = (uint64_t)(b);                           \
        if (value != expected)                                       \
        {                                                            \
            printf(                                                  \
                "Test failed: %s(%u): %s expected: %lu, got: %lu\n", \
                __FILE__,                                            \
                __LINE__,                                            \
                __FUNCTION__,                                        \
                expected,                                            \
                value);                                              \
            oe_abort();                                              \
        }                                                            \
    } while (0)

typedef struct _thread_info_nonblocking_t
{
    int tid;
    oe_sgx_td_t* td;
} thread_info_t;

static thread_info_t _thread_info;
static volatile int _handler_done;
static volatile int* _host_lock_state;

static void cpuid(
    unsigned int leaf,
    unsigned int subleaf,
    unsigned int* eax,
    unsigned int* ebx,
    unsigned int* ecx,
    unsigned int* edx)
{
    asm volatile("cpuid"
                 // CPU id instruction returns values in the following registers
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 // __leaf is passed in eax (0) and __subleaf in ecx (2)
                 : "0"(leaf), "2"(subleaf));
}

// This function will generate the divide by zero function.
// The handler will catch this exception and fix it, and continue execute.
// It will return 0 if success.
static int divide_by_zero_exception_function(void)
{
    // Making ret, f and d volatile to prevent optimization
    volatile int ret = 1;
    volatile float f = 0;
    volatile double d = 0;

    f = 0.31f;
    d = 0.32;

    // Using inline assembly for idiv to prevent it being optimized out
    // completely. Specify edi as the used register to ensure that 32-bit
    // division is done. 64-bit division generates a 3 byte instruction rather
    // than 2 bytes.
    register int edi __asm__("edi") = 0;
    asm volatile("idiv %1"
                 : "=a"(ret)
                 : "r"(edi) // Divisor of 0 is hard-coded
                 : "%1",
                   "cc"); // cc indicates that flags will be clobbered by ASM

    // Check if the float registers are recovered correctly after the exception
    // is handled.
    if (f < 0.309 || f > 0.321 || d < 0.319 || d > 0.321)
    {
        return -1;
    }

    return 0;
}

static uint64_t td_state_handler(oe_exception_record_t* exception_record)
{
    if (exception_record->code == OE_EXCEPTION_UNKNOWN)
    {
        int self_tid = 0;

        if (_handler_done)
        {
            printf("Unexpected interrupt...\n");
            return OE_EXCEPTION_ABORT_EXECUTION;
        }

        // Expect the state to be OE_TD_STATE_SECOND_LEVEL_EXCEPTION_HANDLING
        OE_EXPECT(
            _thread_info.td->state,
            OE_TD_STATE_SECOND_LEVEL_EXCEPTION_HANDLING);

        // Expect is_interrupted flag is set
        OE_TEST(_thread_info.td->is_interrupted == 1);

        OE_TEST(exception_record->code == OE_EXCEPTION_UNKNOWN);

        host_get_tid(&self_tid);
        OE_TEST(_thread_info.tid == self_tid);

        printf("(tid=%d) thread is interrupted...\n", self_tid);

        // Expect the state to be persisted after ocall(s)
        OE_EXPECT(
            _thread_info.td->state,
            OE_TD_STATE_SECOND_LEVEL_EXCEPTION_HANDLING);

        _thread_info.td->state = OE_TD_STATE_RUNNING_BLOCKING;
        {
            uint32_t a, b, c, d;
            cpuid(1, 0, &a, &b, &c, &d);
        }
        // Expect the state to be persisted after an illegal instruction
        // emulation
        OE_EXPECT(_thread_info.td->state, OE_TD_STATE_RUNNING_BLOCKING);

        // Expect is_interrupted flag is persisted
        OE_TEST(_thread_info.td->is_interrupted == 1);

        divide_by_zero_exception_function();

        // Expect the state to be
        // OE_TD_STATE_SECOND_LEVEL_EXCEPTION_HANDLING after a nested exception
        OE_EXPECT(
            _thread_info.td->state,
            OE_TD_STATE_SECOND_LEVEL_EXCEPTION_HANDLING);

        // Expect is_interrupted flag is persisted after a nested exception
        OE_TEST(_thread_info.td->is_interrupted == 1);

        __atomic_store_n(_host_lock_state, 2, __ATOMIC_RELEASE);

        _handler_done = 1;

        return OE_EXCEPTION_CONTINUE_EXECUTION;
    }
    else if (exception_record->code == OE_EXCEPTION_DIVIDE_BY_ZERO)
    {
        int self_tid = 0;
        OE_EXPECT(
            _thread_info.td->state,
            OE_TD_STATE_SECOND_LEVEL_EXCEPTION_HANDLING);
        host_get_tid(&self_tid);
        OE_TEST(_thread_info.tid == self_tid);

        // Skip the idiv instruction - 2 is tied to the size of the idiv
        // instruction and can change with a different compiler/build.
        // Minimizing this with the use of the inline assembly for integer
        // division
        exception_record->context->rip += 2;
        return OE_EXCEPTION_CONTINUE_EXECUTION;
    }
    else
    {
        return OE_EXCEPTION_ABORT_EXECUTION;
    }
}

void enc_run_thread(int tid)
{
    oe_result_t result = OE_OK;
    int self_tid = 0;

    _thread_info.td = oe_sgx_get_td();
    // Expect the state to be ENTERED upon entering
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_ENTERED);

    // Expect is_interrupted flag is not set
    OE_TEST(_thread_info.td->is_interrupted == 0);

    _thread_info.td->state = OE_TD_STATE_RUNNING_BLOCKING;
    host_get_tid(&self_tid);

    // Expect the state to be ENTERED after an ocall
    // A sophisticated application is responsible to
    // update the state after the ocall returns
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_ENTERED);

    OE_TEST(tid == self_tid);
    printf("(tid=%d) thread is running...\n", self_tid);
    _thread_info.tid = tid;

    OE_CHECK(oe_add_vectored_exception_handler(false, td_state_handler));

    // Change the state to RUNNING_NONBLOCKING so the thread
    // can serve an interrupt request
    _thread_info.td->state = OE_TD_STATE_RUNNING_NONBLOCKING;
    // Ensure the order of setting the lock
    asm volatile("" ::: "memory");
    __atomic_store_n(_host_lock_state, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(_host_lock_state, __ATOMIC_ACQUIRE) == 1)
    {
        asm volatile("pause" ::: "memory");
    }

    // Expect the state to be persisted after an interrupt
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_RUNNING_NONBLOCKING);

    // Expect is_interrupted flag is cleared
    OE_TEST(_thread_info.td->is_interrupted == 0);

    printf("(tid=%d) interrupt is handled...\n", self_tid);

    __atomic_store_n(_host_lock_state, 3, __ATOMIC_RELEASE);
    host_spin();
    while (__atomic_load_n(_host_lock_state, __ATOMIC_ACQUIRE) != 5)
    {
        asm volatile("pause" ::: "memory");
    }

    // Expect the state to be ENTERED after an OCALL
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_ENTERED);

    _thread_info.td->state = OE_TD_STATE_RUNNING_BLOCKING;
    {
        uint32_t a, b, c, d;
        cpuid(1, 0, &a, &b, &c, &d);
    }
    // Expect the state to be persisted after an illegal instruction
    // emulation
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_RUNNING_BLOCKING);

    divide_by_zero_exception_function();

    // Expect the state to be persisted after an exception.
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_RUNNING_BLOCKING);

    printf("(tid=%d) thread is exiting...\n", self_tid);
done:
    return;
}

void enc_td_state(uint64_t lock_state)
{
    oe_result_t result;
    int tid = 0;

    host_get_tid(&tid);
    OE_TEST(tid != 0);

    /* Set up the lock_state points to the host*/
    _host_lock_state = (int*)lock_state;

    printf("(tid=%d) Create a thread...\n", tid);
    result = host_create_thread();
    if (result != OE_OK)
        return;

    while (__atomic_load_n(_host_lock_state, __ATOMIC_ACQUIRE) == 0)
    {
        asm volatile("pause" ::: "memory");
    }

    OE_TEST(_thread_info.tid != 0);
    host_sleep_msec(30);

    printf(
        "(tid=%d) Sending interrupt to (td=0x%lx, tid=%d) inside the "
        "enclave...\n",
        tid,
        (uint64_t)_thread_info.td,
        _thread_info.tid);
    host_send_interrupt(_thread_info.tid);

    while (__atomic_load_n(_host_lock_state, __ATOMIC_ACQUIRE) != 4)
    {
        asm volatile("pause" ::: "memory");
    }

    // Expect the target td's state to be EXITED while
    // running in the host context
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_EXITED);
    host_sleep_msec(30);

    printf(
        "(tid=%d) Sending interrupt to (td=0x%lx, tid=%d) on the "
        "host...\n",
        tid,
        (uint64_t)_thread_info.td,
        _thread_info.tid);
    host_send_interrupt(_thread_info.tid);

    host_join_thread();

    // Expect the target td's state to be EXITED
    OE_EXPECT(_thread_info.td->state, OE_TD_STATE_EXITED);
}

OE_SET_ENCLAVE_SGX(
    1,    /* ProductID */
    1,    /* SecurityVersion */
    true, /* Debug */
    1024, /* NumHeapPages */
    1024, /* NumStackPages */
    2);   /* NumTCS */
