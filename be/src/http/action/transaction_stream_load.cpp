// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "http/action/transaction_stream_load.h"

#include <deque>
#include <future>
#include <sstream>

// use string iequal
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <fmt/format.h>
#include <rapidjson/prettywriter.h>
#include <thrift/protocol/TDebugProtocol.h>

#include "common/logging.h"
#include "common/utils.h"
#include "gen_cpp/FrontendService.h"
#include "gen_cpp/FrontendService_types.h"
#include "gen_cpp/HeartbeatService_types.h"
#include "http/http_channel.h"
#include "http/http_common.h"
#include "http/http_headers.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/utils.h"
#include "runtime/client_cache.h"
#include "runtime/current_thread.h"
#include "runtime/exec_env.h"
#include "runtime/fragment_mgr.h"
#include "runtime/load_path_mgr.h"
#include "runtime/plan_fragment_executor.h"
#include "runtime/stream_load/load_stream_mgr.h"
#include "runtime/stream_load/stream_load_context.h"
#include "runtime/stream_load/stream_load_executor.h"
#include "runtime/stream_load/stream_load_pipe.h"
#include "runtime/stream_load/transaction_mgr.h"
#include "util/byte_buffer.h"
#include "util/debug_util.h"
#include "util/defer_op.h"
#include "util/json_util.h"
#include "util/metrics.h"
#include "util/starrocks_metrics.h"
#include "util/thrift_rpc_helper.h"
#include "util/time.h"
#include "util/uid_util.h"

namespace starrocks {

#ifdef BE_TEST
extern TStreamLoadPutResult k_stream_load_put_result;
#endif

static TFileFormatType::type parse_stream_load_format(const std::string& format_str) {
    if (boost::iequals(format_str, "csv")) {
        return TFileFormatType::FORMAT_CSV_PLAIN;
    } else if (boost::iequals(format_str, "json")) {
        return TFileFormatType::FORMAT_JSON;
    } else if (boost::iequals(format_str, "gzip")) {
        return TFileFormatType::FORMAT_CSV_GZ;
    } else if (boost::iequals(format_str, "bzip2")) {
        return TFileFormatType::FORMAT_CSV_BZ2;
    } else if (boost::iequals(format_str, "lz4")) {
        return TFileFormatType::FORMAT_CSV_LZ4_FRAME;
    } else if (boost::iequals(format_str, "deflate")) {
        return TFileFormatType::FORMAT_CSV_DEFLATE;
    } else if (boost::iequals(format_str, "zstd")) {
        return TFileFormatType::FORMAT_CSV_ZSTD;
    }
    return TFileFormatType::FORMAT_UNKNOWN;
}

TransactionManagerAction::TransactionManagerAction(ExecEnv* exec_env) : _exec_env(exec_env) {}

TransactionManagerAction::~TransactionManagerAction() = default;

static void _send_reply(HttpRequest* req, const std::string& str) {
    HttpChannel::send_reply(req, str);
}

void TransactionManagerAction::_send_error_reply(HttpRequest* req, Status st) {
    auto ctx = std::make_unique<StreamLoadContext>(_exec_env);
    ctx->label = req->header(HTTP_LABEL_KEY);

    auto str = ctx->to_resp_json(req->param(HTTP_TXN_OP_KEY), st);
    HttpChannel::send_reply(req, str);
}

void TransactionManagerAction::handle(HttpRequest* req) {
    Status st = Status::OK();
    std::string resp;

    auto txn_op = req->param(HTTP_TXN_OP_KEY);
    if (boost::iequals(txn_op, TXN_LIST)) {
        st = _exec_env->transaction_mgr()->list_transactions(req, &resp);
        return _send_reply(req, resp);
    }

    if (req->header(HTTP_LABEL_KEY).empty()) {
        return _send_error_reply(req, Status::InvalidArgument(fmt::format("empty label")));
    }

    if (boost::iequals(txn_op, TXN_BEGIN)) {
        st = _exec_env->transaction_mgr()->begin_transaction(req, &resp);
    } else if (boost::iequals(txn_op, TXN_COMMIT)) {
        st = _exec_env->transaction_mgr()->commit_transaction(req, &resp);
    } else if (boost::iequals(txn_op, TXN_ROLLBACK)) {
        st = _exec_env->transaction_mgr()->rollback_transaction(req, &resp);
    } else {
        return _send_error_reply(req,
                                 Status::InvalidArgument(fmt::format("unsupport transaction operation {}", txn_op)));
    }

    LOG(INFO) << "Execute transaction label=" << req->header(HTTP_LABEL_KEY) << " op=" << txn_op << " status=" << st;

    _send_reply(req, resp);
}

TransactionStreamLoadAction::TransactionStreamLoadAction(ExecEnv* exec_env) : _exec_env(exec_env) {}

TransactionStreamLoadAction::~TransactionStreamLoadAction() = default;

void TransactionStreamLoadAction::_send_error_reply(HttpRequest* req, Status st) {
    auto ctx = std::make_unique<StreamLoadContext>(_exec_env);
    ctx->label = req->header(HTTP_LABEL_KEY);

    auto str = ctx->to_resp_json(TXN_LOAD, st);
    HttpChannel::send_reply(req, str);
}

void TransactionStreamLoadAction::handle(HttpRequest* req) {
    auto ctx = _exec_env->stream_context_mgr()->get(req->header(HTTP_LABEL_KEY));
    if (ctx == nullptr) {
        return;
    }
    DeferOp defer([&] {
        if (ctx->unref()) {
            delete ctx;
        }
    });
    ctx->last_active_ts = MonotonicNanos();

    if (!ctx->status.ok()) {
        if (ctx->need_rollback) {
            _exec_env->transaction_mgr()->_rollback_transaction(ctx);
        }
    }

    auto resp = _exec_env->transaction_mgr()->_build_reply(TXN_LOAD, ctx);
    ctx->lock.unlock();
    _send_reply(req, resp);
}

int TransactionStreamLoadAction::on_header(HttpRequest* req) {
    auto label = req->header(HTTP_LABEL_KEY);
    if (label.empty()) {
        _send_error_reply(req, Status::InvalidArgument(fmt::format("Invalid label {}", req->header(HTTP_LABEL_KEY))));
        return -1;
    }

    auto ctx = _exec_env->stream_context_mgr()->get(label);
    if (ctx == nullptr) {
        _send_error_reply(req, Status::TransactionNotExists(fmt::format("Transaction with label {} not exists",
                                                                        req->header(HTTP_LABEL_KEY))));
        return -1;
    }
    DeferOp defer([&] {
        if (ctx->unref()) {
            delete ctx;
        }
    });

    if (ctx->db != req->param(HTTP_DB_KEY)) {
        _send_error_reply(req,
                          Status::InvalidArgument(fmt::format("Request database {} not equal transaction database {}",
                                                              req->param(HTTP_DB_KEY), ctx->db)));
        return -1;
    }

    if (!ctx->lock.try_lock()) {
        _send_error_reply(req, Status::TransactionInProcessing("Transaction in processing, please retry later"));
        return -1;
    }
    ctx->table = req->param(HTTP_TABLE_KEY);
    ctx->last_active_ts = MonotonicNanos();

    LOG(INFO) << "new streaming load request." << ctx->brief() << ", db=" << ctx->db << ", tbl=" << ctx->table;

    auto st = _on_header(req, ctx);
    if (!st.ok()) {
        ctx->status = st;
        if (ctx->need_rollback) {
            _exec_env->transaction_mgr()->_rollback_transaction(ctx);
        }
        auto resp = _exec_env->transaction_mgr()->_build_reply(TXN_LOAD, ctx);
        ctx->lock.unlock();
        _send_reply(req, resp);
        return -1;
    }
    return 0;
}

Status TransactionStreamLoadAction::_on_header(HttpRequest* http_req, StreamLoadContext* ctx) {
    if (!ctx->status.ok()) {
        return Status::Aborted(fmt::format("current transaction is aborted"));
    }
    // check content length
    size_t max_body_bytes = config::streaming_load_max_mb * 1024 * 1024;
    if (!http_req->header(HttpHeaders::CONTENT_LENGTH).empty()) {
        ctx->body_bytes += std::stol(http_req->header(HttpHeaders::CONTENT_LENGTH));
        if (ctx->body_bytes > max_body_bytes) {
            LOG(WARNING) << "body exceed max size." << ctx->brief();

            std::stringstream ss;
            ss << "body exceed max size: " << max_body_bytes << ", limit: " << max_body_bytes;
            return Status::InternalError(ss.str());
        }
    } else {
#ifndef BE_TEST
        evhttp_connection_set_max_body_size(evhttp_request_get_connection(http_req->get_evhttp_request()),
                                            max_body_bytes);
#endif
    }
    // get format of this put
    if (http_req->header(HTTP_FORMAT_KEY).empty()) {
        ctx->format = TFileFormatType::FORMAT_CSV_PLAIN;
    } else {
        ctx->format = parse_stream_load_format(http_req->header(HTTP_FORMAT_KEY));
        if (ctx->format == TFileFormatType::FORMAT_UNKNOWN) {
            std::stringstream ss;
            ss << "unknown data format, format=" << http_req->header(HTTP_FORMAT_KEY);
            return Status::InternalError(ss.str());
        }

        if (ctx->format == TFileFormatType::FORMAT_JSON) {
            size_t max_body_bytes = config::streaming_load_max_batch_size_mb * 1024 * 1024;
            auto ignore_json_size = boost::iequals(http_req->header(HTTP_IGNORE_JSON_SIZE), "true");
            if (!ignore_json_size && ctx->body_bytes > max_body_bytes) {
                std::stringstream ss;
                ss << "The size of this batch exceed the max size [" << max_body_bytes << "]  of json type data "
                   << " data [ " << ctx->body_bytes
                   << " ]. Set ignore_json_size to skip the check, although it may lead huge memory consuming.";
                return Status::InternalError(ss.str());
            }
        }
    }

    return _exec_plan_fragment(http_req, ctx);
}

Status TransactionStreamLoadAction::_exec_plan_fragment(HttpRequest* http_req, StreamLoadContext* ctx) {
    // put request
    TStreamLoadPutRequest request;

    // setup stream pipe
    auto pipe = _exec_env->load_stream_mgr()->get(ctx->id);
    if (pipe == nullptr) {
        auto pipe = std::make_shared<StreamLoadPipe>();
        RETURN_IF_ERROR(_exec_env->load_stream_mgr()->put(ctx->id, pipe));
        request.fileType = TFileType::FILE_STREAM;
        ctx->body_sink = pipe;
    } else {
        return Status::OK();
    }

    set_request_auth(&request, ctx->auth);
    request.db = ctx->db;
    request.tbl = ctx->table;
    request.txnId = ctx->txn_id;
    request.formatType = ctx->format;
    request.__set_loadId(ctx->id.to_thrift());

    if (!http_req->header(HTTP_COLUMNS).empty()) {
        request.__set_columns(http_req->header(HTTP_COLUMNS));
    }
    if (!http_req->header(HTTP_WHERE).empty()) {
        request.__set_where(http_req->header(HTTP_WHERE));
    }
    if (!http_req->header(HTTP_COLUMN_SEPARATOR).empty()) {
        request.__set_columnSeparator(http_req->header(HTTP_COLUMN_SEPARATOR));
    }
    if (!http_req->header(HTTP_ROW_DELIMITER).empty()) {
        request.__set_rowDelimiter(http_req->header(HTTP_ROW_DELIMITER));
    }
    if (!http_req->header(HTTP_PARTITIONS).empty()) {
        request.__set_partitions(http_req->header(HTTP_PARTITIONS));
        request.__set_isTempPartition(false);
        if (!http_req->header(HTTP_TEMP_PARTITIONS).empty()) {
            return Status::InvalidArgument("Can not specify both partitions and temporary partitions");
        }
    }
    if (!http_req->header(HTTP_TEMP_PARTITIONS).empty()) {
        request.__set_partitions(http_req->header(HTTP_TEMP_PARTITIONS));
        request.__set_isTempPartition(true);
        if (!http_req->header(HTTP_PARTITIONS).empty()) {
            return Status::InvalidArgument("Can not specify both partitions and temporary partitions");
        }
    }
    if (!http_req->header(HTTP_NEGATIVE).empty() && http_req->header(HTTP_NEGATIVE) == "true") {
        request.__set_negative(true);
    } else {
        request.__set_negative(false);
    }
    if (!http_req->header(HTTP_STRICT_MODE).empty()) {
        if (boost::iequals(http_req->header(HTTP_STRICT_MODE), "false")) {
            request.__set_strictMode(false);
        } else if (boost::iequals(http_req->header(HTTP_STRICT_MODE), "true")) {
            request.__set_strictMode(true);
        } else {
            return Status::InvalidArgument("Invalid strict mode format. Must be bool type");
        }
    }
    if (!http_req->header(HTTP_TIMEZONE).empty()) {
        request.__set_timezone(http_req->header(HTTP_TIMEZONE));
    }
    if (!http_req->header(HTTP_LOAD_MEM_LIMIT).empty()) {
        try {
            auto load_mem_limit = std::stoll(http_req->header(HTTP_LOAD_MEM_LIMIT));
            if (load_mem_limit < 0) {
                return Status::InvalidArgument("load_mem_limit must be equal or greater than 0");
            }
            request.__set_loadMemLimit(load_mem_limit);
        } catch (const std::invalid_argument& e) {
            return Status::InvalidArgument("Invalid load mem limit format");
        }
    }
    if (!http_req->header(HTTP_JSONPATHS).empty()) {
        request.__set_jsonpaths(http_req->header(HTTP_JSONPATHS));
    }
    if (!http_req->header(HTTP_JSONROOT).empty()) {
        request.__set_json_root(http_req->header(HTTP_JSONROOT));
    }
    if (!http_req->header(HTTP_STRIP_OUTER_ARRAY).empty()) {
        if (boost::iequals(http_req->header(HTTP_STRIP_OUTER_ARRAY), "true")) {
            request.__set_strip_outer_array(true);
        } else {
            request.__set_strip_outer_array(false);
        }
    } else {
        request.__set_strip_outer_array(false);
    }
    if (http_req->header(HTTP_PARTIAL_UPDATE) == "true") {
        request.__set_partial_update(true);
    } else {
        request.__set_partial_update(false);
    }
    if (!http_req->header(HTTP_TRANSMISSION_COMPRESSION_TYPE).empty()) {
        request.__set_transmission_compression_type(http_req->header(HTTP_TRANSMISSION_COMPRESSION_TYPE));
    }
    if (!http_req->header(HTTP_LOAD_DOP).empty()) {
        try {
            auto parallel_request_num = std::stoll(http_req->header(HTTP_LOAD_DOP));
            request.__set_load_dop(parallel_request_num);
        } catch (const std::invalid_argument& e) {
            return Status::InvalidArgument("Invalid load_dop format");
        }
    }
    if (ctx->timeout_second != -1) {
        request.__set_timeout(ctx->timeout_second);
    }
    request.__set_thrift_rpc_timeout_ms(config::thrift_rpc_timeout_ms);
    // plan this load
    TNetworkAddress master_addr = _exec_env->master_info()->network_address;
#ifndef BE_TEST
    if (!http_req->header(HTTP_MAX_FILTER_RATIO).empty()) {
        ctx->max_filter_ratio = strtod(http_req->header(HTTP_MAX_FILTER_RATIO).c_str(), nullptr);
    }

    int64_t stream_load_put_start_time = MonotonicNanos();
    RETURN_IF_ERROR(ThriftRpcHelper::rpc<FrontendServiceClient>(
            master_addr.hostname, master_addr.port,
            [&request, ctx](FrontendServiceConnection& client) { client->streamLoadPut(ctx->put_result, request); }));
    ctx->stream_load_put_cost_nanos = MonotonicNanos() - stream_load_put_start_time;
#else
    ctx->put_result = k_stream_load_put_result;
#endif
    Status plan_status(ctx->put_result.status);
    if (!plan_status.ok()) {
        LOG(WARNING) << "plan streaming load failed. errmsg=" << plan_status.get_error_msg() << " " << ctx->brief();
        return plan_status;
    }
    VLOG(3) << "params is " << apache::thrift::ThriftDebugString(ctx->put_result.params);

    if (!http_req->header(HTTP_EXEC_MEM_LIMIT).empty()) {
        auto exec_mem_limit = std::stoll(http_req->header(HTTP_EXEC_MEM_LIMIT));
        if (exec_mem_limit <= 0) {
            return Status::InvalidArgument("exec_mem_limit must be greater than 0");
        }
        ctx->put_result.params.query_options.mem_limit = exec_mem_limit;
    }

    // check reuse
    return _exec_env->stream_load_executor()->execute_plan_fragment(ctx);
}

void TransactionStreamLoadAction::on_chunk_data(HttpRequest* req) {
    auto ctx = _exec_env->stream_context_mgr()->get(req->header(HTTP_LABEL_KEY));
    if (ctx == nullptr) {
        return;
    }
    DeferOp defer([&] {
        if (ctx->unref()) {
            delete ctx;
        }
    });

    SCOPED_THREAD_LOCAL_MEM_TRACKER_SETTER(ctx->instance_mem_tracker.get());

    struct evhttp_request* ev_req = req->get_evhttp_request();
    auto evbuf = evhttp_request_get_input_buffer(ev_req);

    int64_t start_read_data_time = MonotonicNanos();
    while (evbuffer_get_length(evbuf) > 0) {
        ByteBufferPtr bb = ByteBuffer::allocate(4096);
        int remove_bytes;
        {
            // The memory is applied for in http server thread,
            // so the release of this memory must be recorded in ProcessMemTracker
            SCOPED_THREAD_LOCAL_MEM_TRACKER_SETTER(nullptr);
            remove_bytes = evbuffer_remove(evbuf, bb->ptr, bb->capacity);
        }
        bb->pos = remove_bytes;
        bb->flip();
        auto st = ctx->body_sink->append(bb);
        if (!st.ok()) {
            LOG(WARNING) << "append body content failed. errmsg=" << st.get_error_msg() << ctx->brief();
            ctx->status = st;
            return;
        }
        ctx->receive_bytes += remove_bytes;
    }
    ctx->received_data_cost_nanos = ctx->last_active_ts - start_read_data_time;
    ctx->total_received_data_cost_nanos += ctx->received_data_cost_nanos;
}

} // namespace starrocks
