.globl	_printf
.text
_printf:
~~printf:
jsr	r5,csv
~fmt=4
~args=6
jbr	L1
L2:mov	$12+__iob,(sp)
mov	r5,-(sp)
add	$6,(sp)
mov	4(r5),-(sp)
jsr	pc,*$__doprnt
cmp	(sp)+,(sp)+
bit	$40,20+__iob
jeq	L10000
mov	$177777,r0
jbr	L10001
L10000:clr	r0
L10001:jbr	L3
L3:jmp	cret
L1:jbr	L2
.globl
.data
