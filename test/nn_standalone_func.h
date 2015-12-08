
#ifndef NN_TEST_SHARED
#define NN_TEST_SHARED

/* Define some functions that are provided by nanomsg */
#define nn_alloc(sz,nop)	malloc(sz)
#define alloc_assert(expr)	assert( expr != NULL )

#endif /* NN_TEST_SHARED */