Enclave Thread State
-------------------------------

# State Machine
```
       NULL                 previous_state = state
        |                   if nesting_level == 0 -> state_before_exception = state;
        v            AEX    nesting_level++;                              AEX
   -> ENTERED -------------------> FIRST_LEVEL_EXCEPTION_HANDLING <------------------
  |    |   |set by user   ^            |                  |                           |
  |    |   v              | AEX        |                  v                           |
  |    |  RUNNING* -------             |        --> SECOND_LEVEL_EXCEPTION_HANDLING --
  |    |   |       return from illegal |       |    nesting_level--;
  |    v   v     instruction emulation |       |    if nesting_level == 0
  |    EXITED         nesting_level--; v        --- else                 |
  |    |                         [previous_state]                        v
  |    v                                                       [state_before_execution]
   ----
```

Details for each state are as follows.

- `NULL`
   The initial state of a thread, which can only transit to the `ENTERED` state

- `ENTERED`
  The state indicates that the thread has entered the enclave via a non-exception handling
  path. When entering the state, the OE runtime also clears the previous state saved
  in thread data (`td`), which ensures the future exiting flow updates the state correctly
  (see `EXITED` for more detail). Note that a special case that the non-exception handling
  path does not update the state is when the thread is in
  `SECOND_LEVEL_EXCEPTION_HANDLING`. This case indicates that the entering flow is
  triggered by an OCALL made by the exception handler code. Such logic is represented as
  follows.

  ```c
  if (td->state != SECOND_LEVEL_EXCEPTION_HANDLING)
  {
      td->previous_state = NULL;
      td->state = ENTERED;
  }
  ```

  The user can optionally update the state to `RUNNING_BLOCKING` or
  `RUNNING_NONBLOCKING` (no enforcement yet). When an AEX occurs caused by
  non-interrupt requests, the state turns into `FIRST_LEVEL_EXCEPTION_HANDLING`.

- `RUNNING_BLOCKING`
  The state indicates that the thread is running in the blocking mode, which does not
  accept an interrupt request from the host. When an AEX occurs caused
  by non-interrupt requests, the state turns into `FIRST_LEVEL_EXCEPTION_HANDLING`.

- `RUNNING_NONBLOCKING`
  The state indicates that the thread is running in the non-blocking mode, which is
  the only state that accepts an interrupt request from the host if the `is_interrupted`
  flag on the `td` is not set. When AEX occurs caused by both interrupt or non-interrupt
  requests, the state turns into `FIRST_LEVEL_EXCEPTION_HANDLING`.

- `FIRST_LEVEL_EXCEPTION_HANDLING`
  The state represents that the thread is executing the first-level exception handler.
  The state can be transited from any state except for `EXITED` and `NULL`.
  When entering the state, the OE runtime additionally stores the previous state in `td`.
  If the runtime executes the illegal instruction emulation flow,
  the state will turn into the value of the previous state and the previous state becomes
  `FIRST_LEVEL_EXCEPTION_HANDLING`. If the runtime executes the normal exception
  handling flow, the state will transit to `SECOND_LEVEL_EXCEPTION_HANDLING`.
  When the `exception_nesting_level` is zero, the runtime also stores the previous state
  in the `state_before_exception` field of the `td`. This allows the runtime to restore
  the state after the (non-nested) exception is done. The runtime then updates the
  `exception_nesting_level` accordingly, which reflects the level of nesting exceptions.
  The runtime uses the nesting level to determine whether to clear the `is_interrupted` flag
  after the exception handling. The above logic is as follows.

  ```c
  td->previous_state = td->state;
  if (td->exception_nesting_level == 0)
      td->state_before_exception = td->state;
  td->state = FIRST_LEVEL_EXCEPTION_HANDLING;

  td->exception_nesting_level++;
  ...
  if (illegal_instruction_emulation)
  {
      td->exception_nesting_level--;

      td->state = td->previous_state;
      td->previous_state = FIRST_LEVEL_EXCEPTION_HANDLING;
  }
  else
      td->state = SECOND_LEVEL_EXCEPTION_HANDLING;
  ```

  If the exception handling flow is triggered by an interrupt request from the host,
  the OE runtime performs the following logic to determine whether to accept the
  request. The `is_interrupted` flag is only set if the request is accepted, which
  indicates the enclave is handling the interrupt. The runtime will clear the flag after
  the exception handling based on the exception nesting level (see
  `SECOND_LEVEL_EXCEPTION_HANDLING` for more detail).

  ```c
  if (interrupt_request)
  {
      if (td->state != RUNNING_NONBLOCKING)
          return;

      if (td->is_interrupted == 1)
          return;

      // Set the flag
      td->is_interrupted = 1;
  }

  // Update td states
  ```

- `SECOND_LEVEL_EXCEPTION_HANDLING`
  The state represents that the thread is executing the second-level exception
  handler. The state can only be entered from `FIRST_LEVEL_EXCEPTION_HANDLING`.
  The state could transit back to `FIRST_LEVEL_EXCEPTION_HANDLING` if an AEX
  occurs. If the nesting level is zero, the state will be restored to
  `state_before_exception` saved in `td` before resuming the execution.
  The OE runtime will also clear the `is_interrupted` flag if is it set and the
  nesting level is zero, which indicates the interrupt has been served;
  i.e., the runtime will not clear the flag if it is handling a nested exception during
  the interrupt handling. After that, the runtime will always clear the `previous_state`
  which ensures the future exiting flow updates the state correctly
  (see `EXITED` state for more detail).

  The above logic is as follows.

  ```c
  // After exception handlers

  td->exception_nesting_level--;
  if (td->exception_nesting_level == 0)
  {
      td->is_interrupted == 1)
      td->is_interrupted = 0;

      td->state = td->state_before_exception;
      td->state_before_exception = NULL;
  }
  td->previous_state = NULL;
  ```

- `EXITED`
  The state indicates that the thread has left the enclave, which occurs when
  the thread performs the exiting flow. Examples include when the thread makes
  an OCALL or finishes an ECALL when running in `ENTERED`, `RUNNING_BLOCKING`,
  or `RUNNING_NONBLOCKING`. Note that the following special cases of existing
  flow do not cause the state transition.
  - The thread exits from an exception entry (if the `previous_state` equals
    `FIRST_LEVEL_EXCEPTION_HANDLING` or `state` equals `SECOND_LEVEL_EXCEPTION_HANDLING`).

  - The thread makes an OCALL while running in the`SECOND_LEVEL_EXCEPTION_HANDLING`.

  The logic is as follows.
  ```c
  if (td->state != SECOND_LEVEL_EXCEPTION_HANDLING &&
      td->previous != FIRST_LEVEL_EXCEPTION_HANDLING)
    td->state = EXITED;
  ```

Authors
-------

- Ming-Wei Shih <mishih@microsoft.com>
