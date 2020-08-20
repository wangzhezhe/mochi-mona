/*
 * (C) 2020 The University of Chicago
 * 
 * See COPYRIGHT in top-level directory.
 */
#include "mona.h"

typedef struct mona_instance {
    na_class_t*   na_class;
    na_context_t* na_context;
    ABT_pool      progress_pool;
    ABT_xstream   progress_xstream;
    ABT_thread    progress_thread;
    na_bool_t     owns_progress_pool;
    na_bool_t     owns_progress_xstream;
    na_bool_t     owns_na_class_and_context;
    na_bool_t     finalize_flag;
} mona_instance;

typedef struct mona_request {
  ABT_eventual eventual;
} mona_request;

static void mona_progress_loop(void* uarg) {
    mona_instance_t mona = (mona_instance_t)uarg;
    na_return_t trigger_ret, na_ret;
    unsigned int actual_count = 0;
    size_t size;

    while(!mona->finalize_flag) {

        do {
            trigger_ret = NA_Trigger(mona->na_context, 0, 1, NULL, &actual_count);
        } while ((trigger_ret == NA_SUCCESS) && actual_count && !mona->finalize_flag);
        
        ABT_pool_get_size(mona->progress_pool, &size);
        if(size)
            ABT_thread_yield();

        // TODO put a high timeout value to avoid busy-spinning
        // if there is no other ULT in the pool that could run
        na_ret = NA_Progress(mona->na_class, mona->na_context, 0);
        if (na_ret != NA_SUCCESS && na_ret != NA_TIMEOUT) {
            fprintf(stderr, "WARNING: unexpected return value from NA_Progress (%d)\n", na_ret);
        }
    }
}

mona_instance_t mona_init(
        const char *info_string,
        na_bool_t listen,
        const struct na_init_info *na_init_info)
{
    return mona_init_thread(
            info_string,
            listen,
            na_init_info,
            NA_FALSE);
}

mona_instance_t mona_init_thread(
        const char *info_string,
        na_bool_t listen,
        const struct na_init_info *na_init_info,
        na_bool_t use_progress_es)
{
    int ret;
    ABT_xstream xstream = ABT_XSTREAM_NULL;
    ABT_pool progress_pool = ABT_POOL_NULL;
    mona_instance_t mona = MONA_INSTANCE_NULL;

    if(use_progress_es == NA_TRUE) {

        ret = ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPSC, ABT_FALSE, &progress_pool);
        if(ret != ABT_SUCCESS) goto error;

        ret = ABT_xstream_create_basic(ABT_SCHED_DEFAULT, 1, &progress_pool, ABT_SCHED_CONFIG_NULL, &xstream);
        if(ret != ABT_SUCCESS) goto error;

    } else {

        ret = ABT_xstream_self(&xstream);
        if(ret != ABT_SUCCESS) goto error;

        ret = ABT_xstream_get_main_pools(xstream, 1, &progress_pool);
        if(ret != ABT_SUCCESS) goto error;
    }

    mona = mona_init_pool(
            info_string,
            listen,
            na_init_info,
            progress_pool);
    if(!mona) goto error;

    if(use_progress_es == NA_TRUE) {
        mona->owns_progress_pool = NA_TRUE;
        mona->owns_progress_xstream = NA_TRUE;
    }

    mona->progress_xstream = xstream;

finish:
    return mona;

error:
    if(progress_pool != ABT_POOL_NULL && use_progress_es == NA_TRUE)
        ABT_pool_free(&progress_pool);
    if(xstream != ABT_XSTREAM_NULL && use_progress_es == NA_TRUE)
        ABT_xstream_free(&xstream);
    mona = MONA_INSTANCE_NULL;
    goto finish;
}

mona_instance_t mona_init_pool(
        const char *info_string,
        na_bool_t listen,
        const struct na_init_info *na_init_info,
        ABT_pool progress_pool)
{
    na_class_t* na_class = NULL;
    na_context_t* na_context = NULL;
    mona_instance_t mona = MONA_INSTANCE_NULL;

    na_class = NA_Initialize_opt(info_string, listen, na_init_info);
    if(!na_class) goto error;

    na_context = NA_Context_create(na_class);
    if(!na_context) goto error;

    mona = mona_init_na_pool(na_class, na_context, progress_pool);
    if(!mona) goto error;

    mona->owns_na_class_and_context = NA_TRUE;

finish:
    return mona;

error:
    if(na_context) NA_Context_destroy(na_class, na_context);
    if(na_class) NA_Finalize(na_class);
    mona = MONA_INSTANCE_NULL;
    goto finish;
}

mona_instance_t mona_init_na_pool(
        na_class_t *na_class,
        na_context_t *na_context,
        ABT_pool progress_pool)
{
    int ret;
    mona_instance_t mona = (mona_instance_t)calloc(1, sizeof(*mona));
    if(!mona) return MONA_INSTANCE_NULL;
    mona->na_class = na_class;
    mona->na_context = na_context;
    mona->progress_pool = progress_pool;
    mona->progress_xstream = ABT_XSTREAM_NULL;
    mona->progress_thread = ABT_THREAD_NULL;

    ret = ABT_thread_create(mona->progress_pool, mona_progress_loop, 
            (void*)mona, ABT_THREAD_ATTR_NULL, &(mona->progress_thread));
    if(ret != ABT_SUCCESS) goto error;

finish:
    return mona;

error:
    free(mona);
    mona = MONA_INSTANCE_NULL;
    goto finish;
}

na_return_t mona_finalize(mona_instance_t mona)
{
    mona->finalize_flag = NA_TRUE;
    ABT_thread_join(mona->progress_thread);

    if(mona->owns_progress_xstream) {
        ABT_xstream_join(mona->progress_xstream);
        ABT_xstream_free(&(mona->progress_xstream));
    }
    if(mona->owns_progress_pool)
        ABT_pool_free(&(mona->progress_pool));
    if(mona->owns_na_class_and_context) {
        NA_Context_destroy(
                mona->na_class,
                mona->na_context);
        NA_Finalize(mona->na_class);
    }
    free(mona);

    return NA_SUCCESS;
}

const char* mona_get_class_name(mona_instance_t mona)
{
    return NA_Get_class_name(mona->na_class);
}

const char* mona_get_class_protocol(mona_instance_t mona)
{
    return NA_Get_class_protocol(mona->na_class);
}

na_bool_t mona_is_listening(mona_instance_t mona)
{
    return NA_Is_listening(mona->na_class);
}

na_return_t mona_addr_lookup(
        mona_instance_t mona,
        const char *name,
        na_addr_t *addr)
{
    return NA_Addr_lookup(mona->na_class, name, addr);
}

na_return_t mona_addr_free(
        mona_instance_t mona,
        na_addr_t addr)
{
    return NA_Addr_free(mona->na_class, addr);
}

na_return_t mona_addr_set_remove(
        mona_instance_t mona,
        na_addr_t addr)
{
    return NA_Addr_set_remove(mona->na_class, addr);
}

na_return_t mona_addr_self(
        mona_instance_t mona,
        na_addr_t* addr)
{
    return NA_Addr_self(mona->na_class, addr);
}

na_return_t mona_addr_dup(
        mona_instance_t mona,
        na_addr_t addr,
        na_addr_t* dup_addr)
{
    return NA_Addr_dup(mona->na_class, addr, dup_addr);
}

na_bool_t mona_addr_cmp(
        mona_instance_t mona,
        na_addr_t addr1,
        na_addr_t addr2)
{
    return NA_Addr_cmp(mona->na_class, addr1, addr2);
}

na_bool_t mona_addr_is_self(
        mona_instance_t mona,
        na_addr_t addr)
{
    return NA_Addr_is_self(mona->na_class, addr);
}

na_return_t mona_addr_to_string(
        mona_instance_t mona,
        char *buf,
        na_size_t *buf_size,
        na_addr_t addr)
{
    return NA_Addr_to_string(mona->na_class, buf, buf_size, addr);
}

na_size_t mona_addr_get_serialize_size(
        mona_instance_t mona,
        na_addr_t addr)
{
    return NA_Addr_get_serialize_size(mona->na_class, addr);
}

na_return_t mona_addr_serialize(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        na_addr_t addr)
{
    return NA_Addr_serialize(mona->na_class, buf, buf_size, addr);
}

na_return_t mona_addr_deserialize(
        mona_instance_t mona,
        na_addr_t *addr,
        const void *buf,
        na_size_t buf_size)
{
    return NA_Addr_deserialize(mona->na_class, addr, buf, buf_size);
}

na_size_t mona_msg_get_max_unexpected_size(
        mona_instance_t mona)
{
    return NA_Msg_get_max_unexpected_size(mona->na_class);
}

na_size_t mona_msg_get_max_expected_size(
        mona_instance_t mona)
{
    return NA_Msg_get_max_expected_size(mona->na_class);
}

na_size_t mona_msg_get_unexpected_header_size(
        mona_instance_t mona)
{
    return NA_Msg_get_unexpected_header_size(mona->na_class);
}

na_size_t mona_msg_get_expected_header_size(
        mona_instance_t mona)
{
    return NA_Msg_get_expected_header_size(mona->na_class);
}

na_tag_t mona_msg_get_max_tag(mona_instance_t mona)
{
    return NA_Msg_get_max_tag(mona->na_class);
}

na_op_id_t mona_op_create(mona_instance_t mona)
{
    return NA_Op_create(mona->na_class);
}

na_return_t mona_op_destroy(
        mona_instance_t mona,
        na_op_id_t op_id)
{
    return NA_Op_destroy(mona->na_class, op_id);
}

void* mona_msg_buf_alloc(
        mona_instance_t mona,
        na_size_t buf_size,
        void **plugin_data)
{
    return NA_Msg_buf_alloc(mona->na_class, buf_size, plugin_data);
}

na_return_t mona_msg_buf_free(
        mona_instance_t mona,
        void *buf,
        void *plugin_data)
{
    return NA_Msg_buf_free(mona->na_class, buf, plugin_data);
}

na_return_t mona_msg_init_unexpected(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size)
{
    return NA_Msg_init_unexpected(mona->na_class, buf, buf_size);
}

static na_return_t mona_wait_internal(mona_request_t req)
{
    na_return_t* waited_na_ret = NULL;
    na_return_t  na_ret = NA_SUCCESS;

    ABT_eventual_wait(req->eventual, (void**)&waited_na_ret);
    na_ret = *waited_na_ret;
    ABT_eventual_free(&(req->eventual));

    return na_ret;
}

na_return_t mona_wait(mona_request_t req)
{
    na_return_t na_ret = mona_wait_internal(req);
    free(req);
    return na_ret;
}

int mona_test(mona_request_t req, int* flag)
{
    return ABT_eventual_test(req->eventual, NULL, flag);
}

static int mona_callback(const struct na_cb_info *info)
{
    na_return_t na_ret = info->ret;
    mona_request_t req = (mona_request_t)(info->arg);
    ABT_eventual_set(req->eventual, &na_ret, sizeof(na_ret));
    return NA_SUCCESS;
}

static na_return_t mona_msg_isend_unexpected_internal(
        mona_instance_t mona,
        const void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t dest_addr,
        na_uint8_t dest_id,
        na_tag_t tag,
        na_op_id_t *op_id,
        mona_request_t req)
{
    int ret;
    na_return_t na_ret;
    ABT_eventual eventual = ABT_EVENTUAL_NULL;

    ret = ABT_eventual_create(sizeof(na_ret), &eventual);
    if(ret != 0)
        return NA_NOMEM;
            
    req->eventual = eventual;
    return NA_Msg_send_unexpected(
            mona->na_class, mona->na_context,
            mona_callback, (void*)req,
            buf, buf_size, plugin_data,
            dest_addr, dest_id, tag, op_id);
}

na_return_t mona_msg_send_unexpected(
        mona_instance_t mona,
        const void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t dest_addr,
        na_uint8_t dest_id,
        na_tag_t tag,
        na_op_id_t *op_id)
{
    mona_request req = { ABT_EVENTUAL_NULL };
    na_return_t na_ret = mona_msg_isend_unexpected_internal(
            mona, buf, buf_size, plugin_data, dest_addr, dest_id, tag, op_id, &req);
    if(na_ret != NA_SUCCESS) {
        return na_ret;
    }
    return mona_wait_internal(&req);
}

na_return_t mona_msg_isend_unexpected(
        mona_instance_t mona,
        const void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t dest_addr,
        na_uint8_t dest_id,
        na_tag_t tag,
        na_op_id_t *op_id,
        mona_request_t* req)
{
    mona_request_t tmp_req = calloc(1, sizeof(*tmp_req));
    tmp_req->eventual = ABT_EVENTUAL_NULL;
    na_return_t na_ret = mona_msg_isend_unexpected_internal(
            mona, buf, buf_size, plugin_data, dest_addr, dest_id, tag, op_id, tmp_req);
    if(na_ret != NA_SUCCESS)
        free(tmp_req);
    else
        *req = tmp_req;
    return na_ret;
}

static na_return_t mona_msg_irecv_unexpected_internal(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_op_id_t *op_id,
        mona_request_t req)
{
    int ret;
    na_return_t na_ret;
    ABT_eventual eventual = ABT_EVENTUAL_NULL;

    ret = ABT_eventual_create(sizeof(na_ret), &eventual);
    if(ret != 0)
        return NA_NOMEM;
            
    req->eventual = eventual;
    return NA_Msg_recv_unexpected(
            mona->na_class, mona->na_context,
            mona_callback, (void*)req,
            buf, buf_size, plugin_data,
            op_id);
}

na_return_t mona_msg_recv_unexpected(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_op_id_t *op_id)
{
    mona_request req = { ABT_EVENTUAL_NULL };
    na_return_t na_ret = mona_msg_irecv_unexpected_internal(
            mona, buf, buf_size, plugin_data, op_id, &req);
    if(na_ret != NA_SUCCESS)
        return na_ret;
    return mona_wait_internal(&req);
}

na_return_t mona_msg_irecv_unexpected(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_op_id_t *op_id,
        mona_request_t* req)
{
    mona_request_t tmp_req = calloc(1, sizeof(*tmp_req));
    tmp_req->eventual = ABT_EVENTUAL_NULL;
    na_return_t na_ret = mona_msg_irecv_unexpected_internal(
            mona, buf, buf_size, plugin_data, op_id, tmp_req);
    if(na_ret != NA_SUCCESS)
        free(tmp_req);
    else
        *req = tmp_req;
    return na_ret;
}

na_return_t mona_msg_init_expected(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size)
{
    return NA_Msg_init_expected(mona->na_class, buf, buf_size);
}

static na_return_t mona_msg_isend_expected_internal(
        mona_instance_t mona,
        const void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t dest_addr,
        na_uint8_t dest_id,
        na_tag_t tag,
        na_op_id_t *op_id,
        mona_request_t req)
{
    int ret;
    na_return_t na_ret;
    ABT_eventual eventual = ABT_EVENTUAL_NULL;

    ret = ABT_eventual_create(sizeof(na_ret), &eventual);
    if(ret != 0)
        return NA_NOMEM;
            
    req->eventual = eventual;
    return NA_Msg_send_expected(
            mona->na_class, mona->na_context,
            mona_callback, (void*)req,
            buf, buf_size, plugin_data,
            dest_addr, dest_id, tag, op_id);
}

na_return_t mona_msg_send_expected(
        mona_instance_t mona,
        const void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t dest_addr,
        na_uint8_t dest_id,
        na_tag_t tag,
        na_op_id_t *op_id)
{
    mona_request req = { ABT_EVENTUAL_NULL };
    na_return_t na_ret = mona_msg_isend_expected_internal(
            mona, buf, buf_size, plugin_data, dest_addr, dest_id, tag, op_id, &req);
    if(na_ret != NA_SUCCESS)
        return na_ret;
    return mona_wait_internal(&req);
}

na_return_t mona_msg_isend_expected(
        mona_instance_t mona,
        const void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t dest_addr,
        na_uint8_t dest_id,
        na_tag_t tag,
        na_op_id_t *op_id,
        mona_request_t* req)
{
    mona_request_t tmp_req = calloc(1, sizeof(*tmp_req));
    tmp_req->eventual = ABT_EVENTUAL_NULL;
    na_return_t na_ret = mona_msg_isend_expected_internal(
            mona, buf, buf_size, plugin_data, dest_addr, dest_id, tag, op_id, tmp_req);
    if(na_ret != NA_SUCCESS)
        free(tmp_req);
    else
        *req = tmp_req;
    return na_ret;
}

static na_return_t mona_msg_irecv_expected_internal(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t source_addr,
        na_uint8_t source_id,
        na_tag_t tag,
        na_op_id_t *op_id,
        mona_request_t req)
{
    int ret;
    na_return_t na_ret;
    ABT_eventual eventual = ABT_EVENTUAL_NULL;

    ret = ABT_eventual_create(sizeof(na_ret), &eventual);
    if(ret != 0)
        return NA_NOMEM;
            
    req->eventual = eventual;
    return NA_Msg_recv_expected(
            mona->na_class, mona->na_context,
            mona_callback, (void*)req,
            buf, buf_size, plugin_data,
            source_addr, source_id, tag, op_id);
}

na_return_t mona_msg_recv_expected(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t source_addr,
        na_uint8_t source_id,
        na_tag_t tag,
        na_op_id_t *op_id)
{
    mona_request req = { ABT_EVENTUAL_NULL };
    na_return_t na_ret = mona_msg_irecv_expected_internal(
            mona, buf, buf_size, plugin_data, source_addr, source_id, tag, op_id, &req);
    if(na_ret != NA_SUCCESS)
        return na_ret;
    return mona_wait_internal(&req);
}

na_return_t mona_msg_irecv_expected(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        void *plugin_data,
        na_addr_t source_addr,
        na_uint8_t source_id,
        na_tag_t tag,
        na_op_id_t *op_id,
        mona_request_t* req)
{
    mona_request_t tmp_req = calloc(1, sizeof(*tmp_req));
    tmp_req->eventual = ABT_EVENTUAL_NULL;
    na_return_t na_ret = mona_msg_irecv_expected_internal(
            mona, buf, buf_size, plugin_data, source_addr, source_id, tag, op_id, tmp_req);
    if(na_ret != NA_SUCCESS)
        return na_ret;
    else
        *req = tmp_req;
    return na_ret;
}

na_return_t mona_mem_handle_create(
        mona_instance_t mona,
        void *buf,
        na_size_t buf_size,
        unsigned long flags,
        na_mem_handle_t *mem_handle)
{
    return NA_Mem_handle_create(
            mona->na_class,
            buf, buf_size, flags,
            mem_handle);
}

na_return_t mona_mem_handle_create_segments(
        mona_instance_t mona,
        struct na_segment *segments,
        na_size_t segment_count,
        unsigned long flags,
        na_mem_handle_t *mem_handle)
{
    return NA_Mem_handle_create_segments(
            mona->na_class,
            segments, segment_count, flags,
            mem_handle);
}

na_return_t mona_mem_handle_free(
        mona_instance_t mona,
        na_mem_handle_t mem_handle)
{
    return NA_Mem_handle_free(mona->na_class, mem_handle);
}

na_return_t mona_mem_register(
        mona_instance_t mona,
        na_mem_handle_t mem_handle)
{
    return NA_Mem_register(mona->na_class, mem_handle);
}

na_return_t mona_mem_deregister(
        mona_instance_t mona,
        na_mem_handle_t mem_handle)
{
    return NA_Mem_deregister(mona->na_class, mem_handle);
}

na_return_t mona_mem_publish(
        mona_instance_t mona,
        na_mem_handle_t mem_handle)
{
    return NA_Mem_publish(mona->na_class, mem_handle);
}

na_return_t mona_mem_unpublish(
        mona_instance_t mona,
        na_mem_handle_t mem_handle)
{
    return NA_Mem_unpublish(mona->na_class, mem_handle);
}

na_size_t mona_mem_handle_get_serialize_size(
        mona_instance_t mona,
        na_mem_handle_t mem_handle)
{
    return NA_Mem_handle_get_serialize_size(
            mona->na_class, mem_handle);
}

na_return_t mona_mem_handle_serialize(
        mona_instance_t mona,
        void *buf, na_size_t buf_size,
        na_mem_handle_t mem_handle)
{
    return NA_Mem_handle_serialize(
            mona->na_class,
            buf, buf_size,
            mem_handle);
}

na_return_t mona_mem_handle_deserialize(
        mona_instance_t mona,
        na_mem_handle_t *mem_handle,
        const void *buf,
        na_size_t buf_size)
{
    return NA_Mem_handle_deserialize(
            mona->na_class,
            mem_handle, buf, buf_size);
}

static na_return_t mona_iput_internal(
        mona_instance_t mona,
        na_mem_handle_t local_mem_handle,
        na_offset_t local_offset,
        na_mem_handle_t remote_mem_handle,
        na_offset_t remote_offset,
        na_size_t data_size,
        na_addr_t remote_addr,
        na_uint8_t remote_id,
        na_op_id_t *op_id,
        mona_request_t req)
{
    int ret;
    na_return_t na_ret;
    ABT_eventual eventual = ABT_EVENTUAL_NULL;

    ret = ABT_eventual_create(sizeof(na_ret), &eventual);
    if(ret != 0)
        return NA_NOMEM;
            
    req->eventual = eventual;
    return NA_Put(mona->na_class, mona->na_context,
            mona_callback, (void*)req,
            local_mem_handle, local_offset,
            remote_mem_handle, remote_offset,
            data_size, remote_addr,
            remote_id, op_id);
}

na_return_t mona_put(
        mona_instance_t mona,
        na_mem_handle_t local_mem_handle,
        na_offset_t local_offset,
        na_mem_handle_t remote_mem_handle,
        na_offset_t remote_offset,
        na_size_t data_size,
        na_addr_t remote_addr,
        na_uint8_t remote_id,
        na_op_id_t *op_id)
{
    mona_request req = { ABT_EVENTUAL_NULL };
    na_return_t na_ret = mona_iput_internal(
            mona, local_mem_handle, local_offset,
            remote_mem_handle, remote_offset,
            data_size, remote_addr, remote_id, op_id, &req);
    if(na_ret != NA_SUCCESS)
        return na_ret;
    return mona_wait_internal(&req);
}

na_return_t mona_iput(
        mona_instance_t mona,
        na_mem_handle_t local_mem_handle,
        na_offset_t local_offset,
        na_mem_handle_t remote_mem_handle,
        na_offset_t remote_offset,
        na_size_t data_size,
        na_addr_t remote_addr,
        na_uint8_t remote_id,
        na_op_id_t *op_id,
        mona_request_t* req)
{
    mona_request_t tmp_req = calloc(1, sizeof(*tmp_req));
    tmp_req->eventual = ABT_EVENTUAL_NULL;
    na_return_t na_ret = mona_iput_internal(
            mona, local_mem_handle, local_offset,
            remote_mem_handle, remote_offset,
            data_size, remote_addr, remote_id, op_id, tmp_req);
    if(na_ret != NA_SUCCESS)
        free(tmp_req);
    else
        *req = tmp_req;
    return na_ret;
}

static na_return_t mona_iget_internal(
        mona_instance_t mona,
        na_mem_handle_t local_mem_handle,
        na_offset_t local_offset,
        na_mem_handle_t remote_mem_handle,
        na_offset_t remote_offset,
        na_size_t data_size,
        na_addr_t remote_addr,
        na_uint8_t remote_id,
        na_op_id_t *op_id,
        mona_request_t req)
{
    int ret;
    na_return_t na_ret;
    ABT_eventual eventual = ABT_EVENTUAL_NULL;

    ret = ABT_eventual_create(sizeof(na_ret), &eventual);
    if(ret != 0)
        return NA_NOMEM;
            
    req->eventual = eventual;
    return NA_Get(mona->na_class, mona->na_context,
            mona_callback, (void*)req,
            local_mem_handle, local_offset,
            remote_mem_handle, remote_offset,
            data_size, remote_addr,
            remote_id, op_id);
}

na_return_t mona_get(
        mona_instance_t mona,
        na_mem_handle_t local_mem_handle,
        na_offset_t local_offset,
        na_mem_handle_t remote_mem_handle,
        na_offset_t remote_offset,
        na_size_t data_size,
        na_addr_t remote_addr,
        na_uint8_t remote_id,
        na_op_id_t *op_id)
{
    mona_request req = { ABT_EVENTUAL_NULL };
    na_return_t na_ret = mona_iget_internal(
            mona, local_mem_handle,
            local_offset, remote_mem_handle,
            remote_offset, data_size,
            remote_addr, remote_id,
            op_id, &req);
    if(na_ret != NA_SUCCESS)
        return na_ret;
    return mona_wait_internal(&req);
}

na_return_t mona_iget(
        mona_instance_t mona,
        na_mem_handle_t local_mem_handle,
        na_offset_t local_offset,
        na_mem_handle_t remote_mem_handle,
        na_offset_t remote_offset,
        na_size_t data_size,
        na_addr_t remote_addr,
        na_uint8_t remote_id,
        na_op_id_t *op_id,
        mona_request_t* req)
{
    mona_request_t tmp_req = calloc(1, sizeof(*tmp_req));
    tmp_req->eventual = ABT_EVENTUAL_NULL;
    na_return_t na_ret = mona_iget_internal(
            mona, local_mem_handle,
            local_offset, remote_mem_handle,
            remote_offset, data_size,
            remote_addr, remote_id,
            op_id, tmp_req);
    if(na_ret != NA_SUCCESS)
        free(tmp_req);
    else
        *req = tmp_req;
    return na_ret;
}

int mona_poll_get_fd(mona_instance_t mona)
{
    return NA_Poll_get_fd(mona->na_class, mona->na_context);
}

na_bool_t mona_poll_try_wait(mona_instance_t mona)
{
    return NA_Poll_try_wait(mona->na_class, mona->na_context);
}

na_return_t mona_cancel(
        mona_instance_t mona,
        na_op_id_t op_id)
{
    return NA_Cancel(mona->na_class, mona->na_context, op_id);
}

const char* mona_error_to_string(int errnum)
{
    return NA_Error_to_string((na_return_t)errnum);
}

#ifdef __cplusplus
}
#endif
