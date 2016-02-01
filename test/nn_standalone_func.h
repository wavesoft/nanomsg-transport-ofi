
#ifndef NN_TEST_SHARED
#define NN_TEST_SHARED

/* Define some functions that are provided by nanomsg */
#define nn_alloc(sz,nop)	malloc(sz)
#define nn_free(p)			free(p)
#define alloc_assert(expr)	assert( expr != NULL )
#define nn_fast(x) 			(x)
#define nn_slow(x) 			(x)

#endif /* NN_TEST_SHARED */
