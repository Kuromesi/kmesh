/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Copyright Authors of Kmesh */

#ifndef __KMESH_FILTER_H__
#define __KMESH_FILTER_H__

#if !ENHANCED_KERNEL
#include "local_ratelimit.h"
#endif

#include "tcp_proxy.h"
#include "tail_call.h"
#include "bpf_log.h"
#include "kmesh_common.h"
#include "listener/listener.pb-c.h"
#include "filter/tcp_proxy.pb-c.h"
#include "filter/http_connection_manager.pb-c.h"

static inline int filter_match_check(const Listener__Filter *filter, const address_t *addr, const ctx_buff_t *ctx)
{
    int match = 0;
    switch (filter->config_type_case) {
    case LISTENER__FILTER__CONFIG_TYPE_HTTP_CONNECTION_MANAGER:
        match = 1;
        break;
    case LISTENER__FILTER__CONFIG_TYPE_TCP_PROXY:
        match = 1;
        break;
    default:
        break;
    }
    return match;
}

static inline int filter_chain_filter_match(
    const Listener__FilterChain *filter_chain,
    const address_t *addr,
    const ctx_buff_t *ctx,
    Listener__Filter **filter_ptr,
    __u64 *filter_ptr_idx)
{
    void *ptrs = NULL;
    Listener__Filter *filter = NULL;

    if (!filter_ptr || !filter_ptr_idx) {
        BPF_LOG(ERR, FILTERCHAIN, "invalid params\n");
        return -1;
    }

    if (filter_chain->n_filters == 0 || filter_chain->n_filters > KMESH_PER_FILTER_NUM) {
        BPF_LOG(ERR, FILTERCHAIN, "nfilter num(%d) invalid\n", filter_chain->n_filters);
        return -1;
    }

    /* filter match */
    ptrs = KMESH_GET_PTR_VAL(filter_chain->filters, void *);
    if (!ptrs) {
        BPF_LOG(ERR, FILTER, "failed to get filter ptrs\n");
        return -1;
    }

/* limit loop cap to pass bpf verify */
#pragma unroll
    for (unsigned int i = 0; i < KMESH_PER_FILTER_NUM; i++) {
        if (i >= filter_chain->n_filters) {
            break;
        }

        filter = (Listener__Filter *)KMESH_GET_PTR_VAL((void *)*((__u64 *)ptrs + i), Listener__Filter);
        if (!filter) {
            continue;
        }

        // FIXME: repeat on filter_manager
        if (filter_match_check(filter, addr, ctx)) {
            *filter_ptr = filter;
            *filter_ptr_idx = (__u64) * ((__u64 *)ptrs + i);
            return 0;
        }
    }
    return -1;
}

static inline int handle_http_connection_manager(
    const Filter__HttpConnectionManager *http_conn, const address_t *addr, ctx_buff_t *ctx, struct bpf_mem_ptr *msg)
{
    int ret;
    char *route_name = NULL;
    ctx_key_t ctx_key = {0};
    ctx_val_t ctx_val = {0};

    route_name = KMESH_GET_PTR_VAL((http_conn->route_config_name), char *);
    if (!route_name) {
        BPF_LOG(ERR, FILTER, "failed to get http conn route name\n");
        return -1;
    }

    KMESH_TAIL_CALL_CTX_KEY(ctx_key, KMESH_TAIL_CALL_ROUTER_CONFIG, *addr);
    KMESH_TAIL_CALL_CTX_VALSTR(ctx_val, msg, route_name);

    KMESH_TAIL_CALL_WITH_CTX(KMESH_TAIL_CALL_ROUTER_CONFIG, ctx_key, ctx_val);
    return KMESH_TAIL_CALL_RET(ret);
}

SEC_TAIL(KMESH_PORG_CALLS, KMESH_TAIL_CALL_FILTER)
int filter_manager(ctx_buff_t *ctx)
{
    int ret = 0;
    ctx_key_t ctx_key = {0};
    ctx_val_t *ctx_val = NULL;
    Listener__Filter *filter = NULL;
    Filter__HttpConnectionManager *http_conn = NULL;
    Filter__TcpProxy *tcp_proxy = NULL;

    DECLARE_VAR_ADDRESS(ctx, addr);
    KMESH_TAIL_CALL_CTX_KEY(ctx_key, KMESH_TAIL_CALL_FILTER, addr);
    ctx_val = kmesh_tail_lookup_ctx(&ctx_key);
    if (!ctx_val) {
        BPF_LOG(ERR, FILTER, "failed to lookup tail call val\n");
        return KMESH_TAIL_CALL_RET(-1);
    }

    filter = (Listener__Filter *)KMESH_GET_PTR_VAL((void *)ctx_val->val, Listener__Filter);
    if (!filter) {
        BPF_LOG(ERR, FILTER, "failed to get filter\n");
        return KMESH_TAIL_CALL_RET(-1);
    }
    kmesh_tail_delete_ctx(&ctx_key);

    switch (filter->config_type_case) {
#if ENHANCED_KERNEL
    case LISTENER__FILTER__CONFIG_TYPE_HTTP_CONNECTION_MANAGER:
        http_conn = KMESH_GET_PTR_VAL(filter->http_connection_manager, Filter__HttpConnectionManager);
        ret = bpf_parse_header_msg(ctx);
        if (GET_RET_PROTO_TYPE(ret) != PROTO_HTTP_1_1) {
            BPF_LOG(DEBUG, FILTER, "http filter manager,only support http1.1 this version");
            break;
        }

        if (!http_conn) {
            BPF_LOG(ERR, FILTER, "get http_conn failed\n");
            ret = -1;
            break;
        }
        ret = handle_http_connection_manager(http_conn, &addr, ctx, ctx_val->msg);
        break;
#endif
    case LISTENER__FILTER__CONFIG_TYPE_TCP_PROXY:
        tcp_proxy = KMESH_GET_PTR_VAL(filter->tcp_proxy, Filter__TcpProxy);
        if (!tcp_proxy) {
            BPF_LOG(ERR, FILTER, "get tcp_prxoy failed\n");
            ret = -1;
            break;
        }
        ret = tcp_proxy_manager(tcp_proxy, ctx);
        break;
    default:
        break;
    }
    return KMESH_TAIL_CALL_RET(ret);
}

SEC_TAIL(KMESH_PORG_CALLS, KMESH_TAIL_CALL_FILTER_CHAIN)
int filter_chain_manager(ctx_buff_t *ctx)
{
    int ret = 0;
    __u64 filter_idx = 0;
    ctx_key_t ctx_key = {0};
    ctx_val_t ctx_val = {0};
    ctx_val_t *ctx_val_ptr = NULL;
    Listener__FilterChain *filter_chain = NULL;
    Listener__Filter *filter = NULL;

    DECLARE_VAR_ADDRESS(ctx, addr);

    KMESH_TAIL_CALL_CTX_KEY(ctx_key, KMESH_TAIL_CALL_FILTER_CHAIN, addr);
    ctx_val_ptr = kmesh_tail_lookup_ctx(&ctx_key);
    if (!ctx_val_ptr) {
        BPF_LOG(ERR, FILTERCHAIN, "failed to lookup tail ctx\n");
        return KMESH_TAIL_CALL_RET(-1);
    }
    kmesh_tail_delete_ctx(&ctx_key);

    filter_chain = (Listener__FilterChain *)KMESH_GET_PTR_VAL((void *)ctx_val_ptr->val, Listener__FilterChain);
    if (filter_chain == NULL) {
        return KMESH_TAIL_CALL_RET(-1);
    }

#if !ENHANCED_KERNEL
    /* ratelimit check */
    ret = Local_rate_limit__check_and_take(filter_chain, &addr, ctx);
    if (ret != 0) {
        BPF_LOG(ERR, FILTERCHAIN, "rate limited, addr=%s\n", ip2str(&addr.ipv4, 1));
        return KMESH_TAIL_CALL_RET(-1);
    }
#endif

    /* filter match */
    ret = filter_chain_filter_match(filter_chain, &addr, ctx, &filter, &filter_idx);
    if (ret != 0) {
        BPF_LOG(ERR, FILTERCHAIN, "no match filter, addr=%s\n", ip2str(&addr.ipv4, 1));
        return KMESH_TAIL_CALL_RET(-1);
    }

    // FIXME: when filter_manager unsuccessful,
    // we should skip back and handle next filter, rather than exit.

    KMESH_TAIL_CALL_CTX_KEY(ctx_key, KMESH_TAIL_CALL_FILTER, addr);
    KMESH_TAIL_CALL_CTX_VAL(ctx_val, ctx_val_ptr->msg, filter_idx);

    KMESH_TAIL_CALL_WITH_CTX(KMESH_TAIL_CALL_FILTER, ctx_key, ctx_val);
    return KMESH_TAIL_CALL_RET(ret);
}

#endif