#ifndef GO4_RECOVERY_H
#define GO4_RECOVERY_H

/* Call before initializing application RAM, with IRQs masked and MSP set. */
void ws9652a_recovery_startup(void);
/* Privileged thread context: the caller drains the console and stops peers.
 * Uses the otherwise idle debug data register as a one-shot reset mailbox;
 * no SRAM is retained. Returns an errno before teardown if unavailable.
 * On success, holds peers with the bounded sequence and resets into SBL. */
int recovery_request_immediate(void);

#endif
