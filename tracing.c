#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/html.h"
#include "php_tideways_xhprof.h"

extern ZEND_DECLARE_MODULE_GLOBALS(tideways_xhprof);

#include "tracing.h"

static const char digits[] = "0123456789abcdef";

static void *(*_zend_malloc) (size_t);
static void (*_zend_free) (void *);
static void *(*_zend_realloc) (void *, size_t);

void *tideways_malloc (size_t size);
void tideways_free (void *ptr);
void *tideways_realloc (void *ptr, size_t size);

/**
 * Free any items in the free list.
 */
static zend_always_inline void tracing_free_the_free_list(TSRMLS_D)
{
    xhprof_frame_t *frame = TXRG(frame_free_list);
    xhprof_frame_t *current;

    while (frame) {
        current = frame;
        frame = frame->previous_frame;
        efree(current);
    }
}

void tracing_enter_root_frame(TSRMLS_D)
{
    TXRG(start_time) = time_milliseconds(TXRG(clock_source), TXRG(timebase_factor));
    TXRG(start_timestamp) = current_timestamp();
    TXRG(enabled) = 1;
    TXRG(root) = zend_string_init(TIDEWAYS_XHPROF_ROOT_SYMBOL, sizeof(TIDEWAYS_XHPROF_ROOT_SYMBOL)-1, 0);
    TXRG(n_frame) = 0;
    tracing_enter_frame_callgraph(TXRG(root), NULL TSRMLS_CC);
}

void tracing_end(TSRMLS_D)
{
    if (TXRG(enabled) == 1) {
        if (TXRG(root)) {
            zend_string_release(TXRG(root));
        }
        while (TXRG(callgraph_frames)) {
            tracing_exit_frame_callgraph(TSRMLS_C);
        }

        TXRG(enabled) = 0;
        TXRG(callgraph_frames) = NULL;
    }
}

void tracing_callgraph_frame_free(xhprof_frame_tt *frame)
{
    if (frame->function_name) {
        zend_string_release(frame->function_name);
    }

    if (frame->class_name) {
        zend_string_release(frame->class_name);
    }

    efree(frame);
}
xhprof_callgraph_bucket *tracing_callgraph_bucket_find(xhprof_callgraph_bucket *bucket, xhprof_frame_t *current_frame, xhprof_frame_t *previous, zend_long key)
{
    while (bucket) {
        if (bucket->key == key &&
            bucket->child_recurse_level == current_frame->recurse_level &&
            bucket->child_class == current_frame->class_name &&
            zend_string_equals(bucket->child_function, current_frame->function_name)) {

            if (previous == NULL && bucket->parent_class == NULL && bucket->parent_function == NULL ) {
                // special handling for the root
                return bucket;
            } else if (previous &&
                       previous->recurse_level == bucket->parent_recurse_level &&
                       previous->class_name == bucket->parent_class &&
                       zend_string_equals(previous->function_name, bucket->parent_function)) {
                // parent matches as well
                return bucket;
            }
        }

        bucket = bucket->next;
    }

    return NULL;
}

zend_always_inline static zend_ulong hash_data(zend_ulong hash, char *data, size_t size)
{
    size_t i;

    for (i = 0; i < size; ++i) {
        hash = hash * 33 + data[i];
    }

    return hash;
}

zend_always_inline static zend_ulong hash_int(zend_ulong hash, int data)
{
    return hash_data(hash, (char*) &data, sizeof(data));
}

zend_ulong tracing_callgraph_bucket_key(xhprof_frame_t *frame)
{
    zend_ulong hash = 5381;
    xhprof_frame_t *previous = frame->previous_frame;

    if (previous) {
        if (previous->class_name) {
            hash = hash_int(hash, ZSTR_HASH(previous->class_name));
        }

        if (previous->function_name) {
            hash = hash_int(hash, ZSTR_HASH(previous->function_name));
        }
        hash += previous->recurse_level;
    }

    if (frame->class_name) {
        hash = hash_int(hash, ZSTR_HASH(frame->class_name));
    }

    if (frame->function_name) {
        hash = hash_int(hash, ZSTR_HASH(frame->function_name));
    }

    hash += frame->recurse_level;

    return hash;
}

void tracing_callgraph_get_parent_child_name(xhprof_callgraph_bucket *bucket, char *symbol, size_t symbol_len TSRMLS_DC)
{
    if (bucket->parent_class) {
        if (bucket->parent_recurse_level > 0) {
            snprintf(symbol, symbol_len, "%s::%s@%d==>", ZSTR_VAL(bucket->parent_class), ZSTR_VAL(bucket->parent_function), bucket->parent_recurse_level);
        } else {
            snprintf(symbol, symbol_len, "%s::%s==>", ZSTR_VAL(bucket->parent_class), ZSTR_VAL(bucket->parent_function));
        }
    } else if (bucket->parent_function) {
        if (bucket->parent_recurse_level > 0) {
            snprintf(symbol, symbol_len, "%s@%d==>", ZSTR_VAL(bucket->parent_function), bucket->parent_recurse_level);
        } else {
            snprintf(symbol, symbol_len, "%s==>", ZSTR_VAL(bucket->parent_function));
        }
    } else {
        snprintf(symbol, symbol_len, "");
    }

    if (bucket->child_class) {
        if (bucket->child_recurse_level > 0) {
            snprintf(symbol, symbol_len, "%s%s::%s@%d", symbol, ZSTR_VAL(bucket->child_class), ZSTR_VAL(bucket->child_function), bucket->child_recurse_level);
        } else {
            snprintf(symbol, symbol_len, "%s%s::%s", symbol, ZSTR_VAL(bucket->child_class), ZSTR_VAL(bucket->child_function));
        }
    } else if (bucket->child_function) {
        if (bucket->child_recurse_level > 0) {
            snprintf(symbol, symbol_len, "%s%s@%d", symbol, ZSTR_VAL(bucket->child_function), bucket->child_recurse_level);
        } else {
            snprintf(symbol, symbol_len, "%s%s", symbol, ZSTR_VAL(bucket->child_function));
        }
    }
}

void tracing_callgraph_get_function_name(xhprof_frame_tt *frame, char *symbol, size_t symbol_len TSRMLS_DC)
{
    if (frame->class_name) {
        snprintf(symbol, symbol_len, "%s::%s()", ZSTR_VAL(frame->class_name), ZSTR_VAL(frame->function_name));
    } else {
        snprintf(symbol, symbol_len, "%s::%s()", "closure", ZSTR_VAL(frame->function_name));
    }
}

void tracing_callgraph_append_to_array(zval *return_value TSRMLS_DC)
{
    int i = 0;
    xhprof_callgraph_bucket *bucket;
    char symbol[512] = "";
    zval stats_zv, *stats = &stats_zv;

    xhprof_frame_tt *frame = TXRG(my_frame);

    for (int i=0; i<TXRG(n_frame); ++i) {
        frame = TXRG(frame_list)[i];
        tracing_callgraph_get_function_name(frame, symbol, sizeof(symbol) TSRMLS_CC);
        array_init(stats);
        add_assoc_string(stats, "function_name", symbol);
        add_assoc_long(stats, "wt_start", frame->wt_start);
        add_assoc_long(stats, "wt_end", frame->wt_end);
        add_assoc_long(stats, "duration", frame->duration);
        add_index_zval(return_value, i, stats);
    }
}

void tracing_begin(zend_long flags TSRMLS_DC)
{
    int i;

    TXRG(flags) = flags;
    TXRG(callgraph_frames) = NULL;
    TXRG(n_frame) = 0;
    for (i = 0; i < TXRG(n_frame); ++i) {
        TXRG(frame_list)[i] = NULL;
    }

    for (i = 0; i < TIDEWAYS_XHPROF_CALLGRAPH_SLOTS; i++) {
        TXRG(callgraph_buckets)[i] = NULL;
    }

    for (i = 0; i < TIDEWAYS_XHPROF_CALLGRAPH_COUNTER_SIZE; i++) {
        TXRG(function_hash_counters)[i] = 0;
    }

    if (flags & TIDEWAYS_XHPROF_FLAGS_MEMORY_ALLOC) {
        zend_mm_heap *heap = zend_mm_get_heap();
        zend_mm_get_custom_handlers (heap, &_zend_malloc, &_zend_free, &_zend_realloc);
        zend_mm_set_custom_handlers (heap, &tideways_malloc, &tideways_free, &tideways_realloc);
    }
    TXRG(my_frame) = NULL;
}

void tracing_request_init(TSRMLS_D)
{
    TXRG(timebase_factor) = get_timebase_factor(TXRG(clock_source));
    TXRG(enabled) = 0;
    TXRG(flags) = 0;
    TXRG(frame_free_list) = NULL;

    TXRG(num_alloc) = 0;
    TXRG(num_free) = 0;
    TXRG(amount_alloc) = 0;
    TXRG(n_frame) = 0;
}

void tracing_request_shutdown()
{
    tracing_free_the_free_list(TSRMLS_C);
}

void *tideways_malloc (size_t size)
{
    TXRG(num_alloc) += 1;
    TXRG(amount_alloc) += size;

    if (_zend_malloc) {
        return _zend_malloc(size);
    }

    zend_mm_heap *heap = zend_mm_get_heap();
    return zend_mm_alloc(heap, size);
}

void tideways_free (void *ptr)
{
    TXRG(num_free) += 1;

    if (_zend_free) {
        return _zend_free(ptr);
    }

    zend_mm_heap *heap = zend_mm_get_heap();
    return zend_mm_free(heap, ptr);
}

void *tideways_realloc (void *ptr, size_t size)
{
    TXRG(num_alloc) += 1;
    TXRG(num_free) += 1;
    TXRG(amount_alloc) += size;

    if (_zend_realloc) {
        return _zend_realloc(ptr, size);
    }

    zend_mm_heap *heap = zend_mm_get_heap();
    return zend_mm_realloc(heap, ptr, size);
}
