/* Minimal static bump allocator for the minimal libc (no sbrk syscall in the
 * simulator).  free() is a no-op.  Sufficient for small allocations; the
 * authentic gen/malloc.c needs more c1 work before it compiles. */
char _arena[16384];
char *_aptr _arena;
char *malloc(n) {
	char *p;
	n = (n + 1) & ~1;
	if (_aptr + n > _arena + 16384)
		return 0;
	p = _aptr;
	_aptr =+ n;
	return p;
}
free(p) char *p; { }
