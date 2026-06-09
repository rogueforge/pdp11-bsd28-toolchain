/ minimal __cleanup -- stdio flush hook called by exit().
/ No buffered stdio yet, so this is a no-op.  (Replaced once stdio lands.)

.globl	__cleanup
__cleanup:
	rts	pc
