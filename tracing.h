#include "timer.h"

#define TIDEWAYS_XHPROF_ROOT_SYMBOL "main()"
#define TIDEWAYS_XHPROF_CALLGRAPH_COUNTER_SIZE 1024
#define TIDEWAYS_XHPROF_CALLGRAPH_SLOTS 8192
#define TIDEWAYS_XHPROF_FLAGS_CPU 1
#define TIDEWAYS_XHPROF_FLAGS_MEMORY_MU 2
#define TIDEWAYS_XHPROF_FLAGS_MEMORY_PMU 4
#define TIDEWAYS_XHPROF_FLAGS_MEMORY 6
#define TIDEWAYS_XHPROF_FLAGS_MEMORY_ALLOC 16
#define TIDEWAYS_XHPROF_FLAGS_MEMORY_ALLOC_AS_MU (32|16)
#define TIDEWAYS_XHPROF_FLAGS_NO_BUILTINS 8

void tracing_callgraph_append_to_array(zval *return_value TSRMLS_DC);
void tracing_callgraph_get_parent_child_name(xhprof_callgraph_bucket *bucket, char *symbol, size_t symbol_len TSRMLS_DC);
zend_ulong tracing_callgraph_bucket_key(xhprof_frame_t *frame);
xhprof_callgraph_bucket *tracing_callgraph_bucket_find(xhprof_callgraph_bucket *bucket, xhprof_frame_t *current_frame, xhprof_frame_t *previous, zend_long key);
void tracing_callgraph_frame_free(xhprof_frame_tt *frame);
void tracing_begin(zend_long flags TSRMLS_DC);
void tracing_end(TSRMLS_D);
void tracing_enter_root_frame(TSRMLS_D);
void tracing_request_init(TSRMLS_D);
void tracing_request_shutdown();
void tracing_determine_clock_source();

#define TXRG(v) ZEND_MODULE_GLOBALS_ACCESSOR(tideways_xhprof, v)

#if defined(ZTS) && defined(COMPILE_DL_TIDEWAYS_XHPROF)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

static zend_always_inline void tracing_fast_free_frame(xhprof_frame_t *p TSRMLS_DC)
{
    if (p->function_name != NULL) {
        zend_string_release(p->function_name);
    }
    if (p->class_name != NULL) {
        zend_string_release(p->class_name);
    }

    /* we use/overload the previous_frame field in the structure to link entries in
     * the free list. */
    p->previous_frame = TXRG(frame_free_list);
    TXRG(frame_free_list) = p;
}

static zend_always_inline xhprof_frame_tt* tracing_fast_alloc_frame(TSRMLS_D)
{
    return (xhprof_frame_tt *)emalloc(sizeof(xhprof_frame_tt));
}

static zend_always_inline zend_string* tracing_get_class_name(zend_execute_data *data TSRMLS_DC)
{
    zend_function *curr_func;

    if (!data) {
        return NULL;
    }

    curr_func = data->func;

    if (curr_func->common.scope != NULL) {
        zend_string_addref(curr_func->common.scope->name);

        return curr_func->common.scope->name;
    }

    return NULL;
}

static zend_always_inline zend_string* tracing_get_function_name(zend_execute_data *data TSRMLS_DC)
{
    zend_function *curr_func;

    if (!data) {
        return NULL;
    }

    curr_func = data->func;

    if (!curr_func->common.function_name) {
        // This branch includes execution of eval and include/require(_once) calls
        // We assume it is not 1999 anymore and not much PHP code runs in the
        // body of a file and if it is, we are ok with adding it to the caller's wt.
        return NULL;
    }

    zend_string_addref(curr_func->common.function_name);

    return curr_func->common.function_name;
}

zend_always_inline static int tracing_enter_frame_callgraph(zend_string *root_symbol, zend_execute_data *execute_data TSRMLS_DC)
{
    zend_string *function_name = (root_symbol != NULL) ? zend_string_copy(root_symbol) : tracing_get_function_name(execute_data TSRMLS_CC);
    xhprof_frame_tt *current_frame;

    if (function_name == NULL) {
        return 0;
    }

    current_frame = tracing_fast_alloc_frame(TSRMLS_C);
    current_frame->class_name = (root_symbol == NULL) ? tracing_get_class_name(execute_data TSRMLS_CC) : NULL;
    current_frame->function_name = function_name;
    current_frame->previous_frame = TXRG(my_frame);
    current_frame->wt_start = time_milliseconds(TXRG(clock_source), TXRG(timebase_factor));
    current_frame->wt_end = current_frame->wt_start;

    TXRG(my_frame) = current_frame;
    return 1;
}

zend_always_inline static void tracing_exit_frame_callgraph(TSRMLS_D)
{
    xhprof_frame_tt *current_frame = TXRG(my_frame);
    current_frame->wt_end = time_milliseconds(TXRG(clock_source), TXRG(timebase_factor));
    zend_long duration = current_frame->wt_end - current_frame->wt_start;
    current_frame->duration = duration;
    TXRG(my_frame) = TXRG(my_frame)->previous_frame;
    if (TXRG(n_frame) < 10000 && current_frame->duration > 10) {
        TXRG(frame_list)[TXRG(n_frame)++] = current_frame;
    } else {
        ///Free the frame list
        tracing_callgraph_frame_free(current_frame);
    }
}
