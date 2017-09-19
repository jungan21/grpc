/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <grpc/compression.h>
#include <grpc/grpc.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/useful.h>

#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/compression/algorithm_metadata.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/support/arena.h"
#include "src/core/lib/support/string.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/call.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/completion_queue.h"
#include "src/core/lib/surface/validate_metadata.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/metadata.h"
#include "src/core/lib/transport/static_metadata.h"
#include "src/core/lib/transport/transport.h"

/** The maximum number of concurrent batches possible.
    Based upon the maximum number of individually queueable ops in the batch
    api:
      - initial metadata send
      - message send
      - status/close send (depending on client/server)
      - initial metadata recv
      - message recv
      - status/close recv (depending on client/server) */
#define MAX_CONCURRENT_BATCHES 6

#define MAX_SEND_EXTRA_METADATA_COUNT 3

/* Status data for a request can come from several sources; this
   enumerates them all, and acts as a priority sorting for which
   status to return to the application - earlier entries override
   later ones */
typedef enum {
  /* Status came from the application layer overriding whatever
     the wire says */
  STATUS_FROM_API_OVERRIDE = 0,
  /* Status came from 'the wire' - or somewhere below the surface
     layer */
  STATUS_FROM_WIRE,
  /* Status was created by some internal channel stack operation: must come via
     add_batch_error */
  STATUS_FROM_CORE,
  /* Status was created by some surface error */
  STATUS_FROM_SURFACE,
  /* Status came from the server sending status */
  STATUS_FROM_SERVER_STATUS,
  STATUS_SOURCE_COUNT
} status_source;

typedef struct {
  bool is_set;
  grpc_error *error;
} received_status;

static gpr_atm pack_received_status(received_status r) {
  return r.is_set ? (1 | (gpr_atm)r.error) : 0;
}

static received_status unpack_received_status(gpr_atm atm) {
  return (atm & 1) == 0
             ? (received_status){.is_set = false, .error = GRPC_ERROR_NONE}
             : (received_status){.is_set = true,
                                 .error = (grpc_error *)(atm & ~(gpr_atm)1)};
}

#define MAX_ERRORS_PER_BATCH 4

typedef struct batch_control {
  grpc_call *call;
  /* Share memory for cq_completion and notify_tag as they are never needed
     simultaneously. Each byte used in this data structure count as six bytes
     per call, so any savings we can make are worthwhile,

     We use notify_tag to determine whether or not to send notification to the
     completion queue. Once we've made that determination, we can reuse the
     memory for cq_completion. */
  union {
    grpc_cq_completion cq_completion;
    struct {
      /* Any given op indicates completion by either (a) calling a closure or
         (b) sending a notification on the call's completion queue.  If
         \a is_closure is true, \a tag indicates a closure to be invoked;
         otherwise, \a tag indicates the tag to be used in the notification to
         be sent to the completion queue. */
      void *tag;
      bool is_closure;
    } notify_tag;
  } completion_data;
  grpc_closure start_batch;
  grpc_closure finish_batch;
  gpr_refcount steps_to_complete;

  grpc_error *errors[MAX_ERRORS_PER_BATCH];
  gpr_atm num_errors;

  grpc_transport_stream_op_batch op;
} batch_control;

typedef struct {
  gpr_mu child_list_mu;
  grpc_call *first_child;
} parent_call;

typedef struct {
  grpc_call *parent;
  /** siblings: children of the same parent form a list, and this list is
     protected under
      parent->mu */
  grpc_call *sibling_next;
  grpc_call *sibling_prev;
} child_call;

#define RECV_NONE ((gpr_atm)0)
#define RECV_INITIAL_METADATA_FIRST ((gpr_atm)1)

struct grpc_call {
  gpr_refcount ext_ref;
  gpr_arena *arena;
  grpc_call_combiner call_combiner;
  grpc_completion_queue *cq;
  grpc_polling_entity pollent;
  grpc_channel *channel;
  gpr_timespec start_time;
  /* parent_call* */ gpr_atm parent_call_atm;
  child_call *child_call;

  /* client or server call */
  bool is_client;
  /** has grpc_call_unref been called */
  bool destroy_called;
  /** flag indicating that cancellation is inherited */
  bool cancellation_is_inherited;
  /** which ops are in-flight */
  bool sent_initial_metadata;
  bool sending_message;
  bool sent_final_op;
  bool received_initial_metadata;
  bool receiving_message;
  bool requested_final_op;
  gpr_atm any_ops_sent_atm;
  gpr_atm received_final_op_atm;

  batch_control *active_batches[MAX_CONCURRENT_BATCHES];
  grpc_transport_stream_op_batch_payload stream_op_payload;

  /* first idx: is_receiving, second idx: is_trailing */
  grpc_metadata_batch metadata_batch[2][2];

  /* Buffered read metadata waiting to be returned to the application.
     Element 0 is initial metadata, element 1 is trailing metadata. */
  grpc_metadata_array *buffered_metadata[2];

  grpc_metadata compression_md;

  // A char* indicating the peer name.
  gpr_atm peer_string;

  /* Packed received call statuses from various sources */
  gpr_atm status[STATUS_SOURCE_COUNT];

  /* Call data useful used for reporting. Only valid after the call has
   * completed */
  grpc_call_final_info final_info;

  /* Compression algorithm for *incoming* data */
  grpc_message_compression_algorithm incoming_message_compression_algorithm;
  /* Stream compression algorithm for *incoming* data */
  grpc_stream_compression_algorithm incoming_stream_compression_algorithm;
  /* Supported encodings (compression algorithms), a bitset */
  uint32_t encodings_accepted_by_peer;
  /* Supported stream encodings (stream compression algorithms), a bitset */
  uint32_t stream_encodings_accepted_by_peer;

  /* Contexts for various subsystems (security, tracing, ...). */
  grpc_call_context_element context[GRPC_CONTEXT_COUNT];

  /* for the client, extra metadata is initial metadata; for the
     server, it's trailing metadata */
  grpc_linked_mdelem send_extra_metadata[MAX_SEND_EXTRA_METADATA_COUNT];
  int send_extra_metadata_count;
  gpr_timespec send_deadline;

  grpc_slice_buffer_stream sending_stream;

  grpc_byte_stream *receiving_stream;
  grpc_byte_buffer **receiving_buffer;
  grpc_slice receiving_slice;
  grpc_closure receiving_slice_ready;
  grpc_closure receiving_stream_ready;
  grpc_closure receiving_initial_metadata_ready;
  uint32_t test_only_last_message_flags;

  grpc_closure release_call;

  union {
    struct {
      grpc_status_code *status;
      grpc_slice *status_details;
    } client;
    struct {
      int *cancelled;
    } server;
  } final_op;

  /* recv_state can contain one of the following values:
     RECV_NONE :                 :  no initial metadata and messages received
     RECV_INITIAL_METADATA_FIRST :  received initial metadata first
     a batch_control*            :  received messages first

                 +------1------RECV_NONE------3-----+
                 |                                  |
                 |                                  |
                 v                                  v
     RECV_INITIAL_METADATA_FIRST        receiving_stream_ready_bctlp
           |           ^                      |           ^
           |           |                      |           |
           +-----2-----+                      +-----4-----+

    For 1, 4: See receiving_initial_metadata_ready() function
    For 2, 3: See receiving_stream_ready() function */
  gpr_atm recv_state;
};

grpc_tracer_flag grpc_call_error_trace =
    GRPC_TRACER_INITIALIZER(false, "call_error");
grpc_tracer_flag grpc_compression_trace =
    GRPC_TRACER_INITIALIZER(false, "compression");

#define CALL_STACK_FROM_CALL(call) ((grpc_call_stack *)((call) + 1))
#define CALL_FROM_CALL_STACK(call_stack) (((grpc_call *)(call_stack)) - 1)
#define CALL_ELEM_FROM_CALL(call, idx) \
  grpc_call_stack_element(CALL_STACK_FROM_CALL(call), idx)
#define CALL_FROM_TOP_ELEM(top_elem) \
  CALL_FROM_CALL_STACK(grpc_call_stack_from_top_element(top_elem))

static void execute_batch(grpc_exec_ctx *exec_ctx, grpc_call *call,
                          grpc_transport_stream_op_batch *op,
                          grpc_closure *start_batch_closure);
static void cancel_with_status(grpc_exec_ctx *exec_ctx, grpc_call *c,
                               status_source source, grpc_status_code status,
                               const char *description);
static void cancel_with_error(grpc_exec_ctx *exec_ctx, grpc_call *c,
                              status_source source, grpc_error *error);
static void destroy_call(grpc_exec_ctx *exec_ctx, void *call_stack,
                         grpc_error *error);
static void receiving_slice_ready(grpc_exec_ctx *exec_ctx, void *bctlp,
                                  grpc_error *error);
static void get_final_status(grpc_call *call,
                             void (*set_value)(grpc_status_code code,
                                               void *user_data),
                             void *set_value_user_data, grpc_slice *details);
static void set_status_value_directly(grpc_status_code status, void *dest);
static void set_status_from_error(grpc_exec_ctx *exec_ctx, grpc_call *call,
                                  status_source source, grpc_error *error);
static void process_data_after_md(grpc_exec_ctx *exec_ctx, batch_control *bctl);
static void post_batch_completion(grpc_exec_ctx *exec_ctx, batch_control *bctl);
static void add_batch_error(grpc_exec_ctx *exec_ctx, batch_control *bctl,
                            grpc_error *error, bool has_cancelled);

static void add_init_error(grpc_error **composite, grpc_error *new) {
  if (new == GRPC_ERROR_NONE) return;
  if (*composite == GRPC_ERROR_NONE)
    *composite = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Call creation failed");
  *composite = grpc_error_add_child(*composite, new);
}

void *grpc_call_arena_alloc(grpc_call *call, size_t size) {
  return gpr_arena_alloc(call->arena, size);
}

static parent_call *get_or_create_parent_call(grpc_call *call) {
  parent_call *p = (parent_call *)gpr_atm_acq_load(&call->parent_call_atm);
  if (p == NULL) {
    p = (parent_call *)gpr_arena_alloc(call->arena, sizeof(*p));
    gpr_mu_init(&p->child_list_mu);
    if (!gpr_atm_rel_cas(&call->parent_call_atm, (gpr_atm)NULL, (gpr_atm)p)) {
      gpr_mu_destroy(&p->child_list_mu);
      p = (parent_call *)gpr_atm_acq_load(&call->parent_call_atm);
    }
  }
  return p;
}

static parent_call *get_parent_call(grpc_call *call) {
  return (parent_call *)gpr_atm_acq_load(&call->parent_call_atm);
}

grpc_error *grpc_call_create(grpc_exec_ctx *exec_ctx,
                             const grpc_call_create_args *args,
                             grpc_call **out_call) {
  size_t i, j;
  grpc_error *error = GRPC_ERROR_NONE;
  grpc_channel_stack *channel_stack =
      grpc_channel_get_channel_stack(args->channel);
  grpc_call *call;
  GPR_TIMER_BEGIN("grpc_call_create", 0);
  gpr_arena *arena =
      gpr_arena_create(grpc_channel_get_call_size_estimate(args->channel));
  call = (grpc_call *)gpr_arena_alloc(
      arena, sizeof(grpc_call) + channel_stack->call_stack_size);
  gpr_ref_init(&call->ext_ref, 1);
  call->arena = arena;
  grpc_call_combiner_init(&call->call_combiner);
  *out_call = call;
  call->channel = args->channel;
  call->cq = args->cq;
  call->start_time = gpr_now(GPR_CLOCK_MONOTONIC);
  /* Always support no compression */
  GPR_BITSET(&call->encodings_accepted_by_peer, GRPC_MESSAGE_COMPRESS_NONE);
  call->is_client = args->server_transport_data == NULL;
  if (call->is_client) {
    GRPC_STATS_INC_CLIENT_CALLS_CREATED(exec_ctx);
  } else {
    GRPC_STATS_INC_SERVER_CALLS_CREATED(exec_ctx);
  }
  call->stream_op_payload.context = call->context;
  grpc_slice path = grpc_empty_slice();
  if (call->is_client) {
    GPR_ASSERT(args->add_initial_metadata_count <
               MAX_SEND_EXTRA_METADATA_COUNT);
    for (i = 0; i < args->add_initial_metadata_count; i++) {
      call->send_extra_metadata[i].md = args->add_initial_metadata[i];
      if (grpc_slice_eq(GRPC_MDKEY(args->add_initial_metadata[i]),
                        GRPC_MDSTR_PATH)) {
        path = grpc_slice_ref_internal(
            GRPC_MDVALUE(args->add_initial_metadata[i]));
      }
    }
    call->send_extra_metadata_count = (int)args->add_initial_metadata_count;
  } else {
    GPR_ASSERT(args->add_initial_metadata_count == 0);
    call->send_extra_metadata_count = 0;
  }
  for (i = 0; i < 2; i++) {
    for (j = 0; j < 2; j++) {
      call->metadata_batch[i][j].deadline = gpr_inf_future(GPR_CLOCK_MONOTONIC);
    }
  }
  gpr_timespec send_deadline =
      gpr_convert_clock_type(args->send_deadline, GPR_CLOCK_MONOTONIC);

  bool immediately_cancel = false;

  if (args->parent_call != NULL) {
    child_call *cc = call->child_call =
        gpr_arena_alloc(arena, sizeof(child_call));
    call->child_call->parent = args->parent_call;

    GRPC_CALL_INTERNAL_REF(args->parent_call, "child");
    GPR_ASSERT(call->is_client);
    GPR_ASSERT(!args->parent_call->is_client);

    parent_call *pc = get_or_create_parent_call(args->parent_call);

    gpr_mu_lock(&pc->child_list_mu);

    if (args->propagation_mask & GRPC_PROPAGATE_DEADLINE) {
      send_deadline = gpr_time_min(
          gpr_convert_clock_type(send_deadline,
                                 args->parent_call->send_deadline.clock_type),
          args->parent_call->send_deadline);
    }
    /* for now GRPC_PROPAGATE_TRACING_CONTEXT *MUST* be passed with
     * GRPC_PROPAGATE_STATS_CONTEXT */
    /* TODO(ctiller): This should change to use the appropriate census start_op
     * call. */
    if (args->propagation_mask & GRPC_PROPAGATE_CENSUS_TRACING_CONTEXT) {
      if (0 == (args->propagation_mask & GRPC_PROPAGATE_CENSUS_STATS_CONTEXT)) {
        add_init_error(&error, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                   "Census tracing propagation requested "
                                   "without Census context propagation"));
      }
      grpc_call_context_set(
          call, GRPC_CONTEXT_TRACING,
          args->parent_call->context[GRPC_CONTEXT_TRACING].value, NULL);
    } else if (args->propagation_mask & GRPC_PROPAGATE_CENSUS_STATS_CONTEXT) {
      add_init_error(&error, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                 "Census context propagation requested "
                                 "without Census tracing propagation"));
    }
    if (args->propagation_mask & GRPC_PROPAGATE_CANCELLATION) {
      call->cancellation_is_inherited = 1;
      if (gpr_atm_acq_load(&args->parent_call->received_final_op_atm)) {
        immediately_cancel = true;
      }
    }

    if (pc->first_child == NULL) {
      pc->first_child = call;
      cc->sibling_next = cc->sibling_prev = call;
    } else {
      cc->sibling_next = pc->first_child;
      cc->sibling_prev = pc->first_child->child_call->sibling_prev;
      cc->sibling_next->child_call->sibling_prev =
          cc->sibling_prev->child_call->sibling_next = call;
    }

    gpr_mu_unlock(&pc->child_list_mu);
  }

  call->send_deadline = send_deadline;

  GRPC_CHANNEL_INTERNAL_REF(args->channel, "call");
  /* initial refcount dropped by grpc_call_unref */
  grpc_call_element_args call_args = {
      .call_stack = CALL_STACK_FROM_CALL(call),
      .server_transport_data = args->server_transport_data,
      .context = call->context,
      .path = path,
      .start_time = call->start_time,
      .deadline = send_deadline,
      .arena = call->arena,
      .call_combiner = &call->call_combiner};
  add_init_error(&error, grpc_call_stack_init(exec_ctx, channel_stack, 1,
                                              destroy_call, call, &call_args));
  if (error != GRPC_ERROR_NONE) {
    cancel_with_error(exec_ctx, call, STATUS_FROM_SURFACE,
                      GRPC_ERROR_REF(error));
  }
  if (immediately_cancel) {
    cancel_with_error(exec_ctx, call, STATUS_FROM_API_OVERRIDE,
                      GRPC_ERROR_CANCELLED);
  }
  if (args->cq != NULL) {
    GPR_ASSERT(
        args->pollset_set_alternative == NULL &&
        "Only one of 'cq' and 'pollset_set_alternative' should be non-NULL.");
    GRPC_CQ_INTERNAL_REF(args->cq, "bind");
    call->pollent =
        grpc_polling_entity_create_from_pollset(grpc_cq_pollset(args->cq));
  }
  if (args->pollset_set_alternative != NULL) {
    call->pollent = grpc_polling_entity_create_from_pollset_set(
        args->pollset_set_alternative);
  }
  if (!grpc_polling_entity_is_empty(&call->pollent)) {
    grpc_call_stack_set_pollset_or_pollset_set(
        exec_ctx, CALL_STACK_FROM_CALL(call), &call->pollent);
  }

  grpc_slice_unref_internal(exec_ctx, path);

  GPR_TIMER_END("grpc_call_create", 0);
  return error;
}

void grpc_call_set_completion_queue(grpc_exec_ctx *exec_ctx, grpc_call *call,
                                    grpc_completion_queue *cq) {
  GPR_ASSERT(cq);

  if (grpc_polling_entity_pollset_set(&call->pollent) != NULL) {
    gpr_log(GPR_ERROR, "A pollset_set is already registered for this call.");
    abort();
  }
  call->cq = cq;
  GRPC_CQ_INTERNAL_REF(cq, "bind");
  call->pollent = grpc_polling_entity_create_from_pollset(grpc_cq_pollset(cq));
  grpc_call_stack_set_pollset_or_pollset_set(
      exec_ctx, CALL_STACK_FROM_CALL(call), &call->pollent);
}

#ifndef NDEBUG
#define REF_REASON reason
#define REF_ARG , const char *reason
#else
#define REF_REASON ""
#define REF_ARG
#endif
void grpc_call_internal_ref(grpc_call *c REF_ARG) {
  GRPC_CALL_STACK_REF(CALL_STACK_FROM_CALL(c), REF_REASON);
}
void grpc_call_internal_unref(grpc_exec_ctx *exec_ctx, grpc_call *c REF_ARG) {
  GRPC_CALL_STACK_UNREF(exec_ctx, CALL_STACK_FROM_CALL(c), REF_REASON);
}

static void release_call(grpc_exec_ctx *exec_ctx, void *call,
                         grpc_error *error) {
  grpc_call *c = (grpc_call *)call;
  grpc_channel *channel = c->channel;
  grpc_call_combiner_destroy(&c->call_combiner);
  gpr_free((char *)c->peer_string);
  grpc_channel_update_call_size_estimate(channel, gpr_arena_destroy(c->arena));
  GRPC_CHANNEL_INTERNAL_UNREF(exec_ctx, channel, "call");
}

static void set_status_value_directly(grpc_status_code status, void *dest);
static void destroy_call(grpc_exec_ctx *exec_ctx, void *call,
                         grpc_error *error) {
  size_t i;
  int ii;
  grpc_call *c = (grpc_call *)call;
  GPR_TIMER_BEGIN("destroy_call", 0);
  for (i = 0; i < 2; i++) {
    grpc_metadata_batch_destroy(
        exec_ctx, &c->metadata_batch[1 /* is_receiving */][i /* is_initial */]);
  }
  if (c->receiving_stream != NULL) {
    grpc_byte_stream_destroy(exec_ctx, c->receiving_stream);
  }
  parent_call *pc = get_parent_call(c);
  if (pc != NULL) {
    gpr_mu_destroy(&pc->child_list_mu);
  }
  for (ii = 0; ii < c->send_extra_metadata_count; ii++) {
    GRPC_MDELEM_UNREF(exec_ctx, c->send_extra_metadata[ii].md);
  }
  for (i = 0; i < GRPC_CONTEXT_COUNT; i++) {
    if (c->context[i].destroy) {
      c->context[i].destroy(c->context[i].value);
    }
  }
  if (c->cq) {
    GRPC_CQ_INTERNAL_UNREF(exec_ctx, c->cq, "bind");
  }

  get_final_status(call, set_status_value_directly, &c->final_info.final_status,
                   NULL);
  c->final_info.stats.latency =
      gpr_time_sub(gpr_now(GPR_CLOCK_MONOTONIC), c->start_time);

  for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
    GRPC_ERROR_UNREF(
        unpack_received_status(gpr_atm_acq_load(&c->status[i])).error);
  }

  grpc_call_stack_destroy(exec_ctx, CALL_STACK_FROM_CALL(c), &c->final_info,
                          GRPC_CLOSURE_INIT(&c->release_call, release_call, c,
                                            grpc_schedule_on_exec_ctx));
  GPR_TIMER_END("destroy_call", 0);
}

void grpc_call_ref(grpc_call *c) { gpr_ref(&c->ext_ref); }

void grpc_call_unref(grpc_call *c) {
  if (!gpr_unref(&c->ext_ref)) return;

  child_call *cc = c->child_call;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;

  GPR_TIMER_BEGIN("grpc_call_unref", 0);
  GRPC_API_TRACE("grpc_call_unref(c=%p)", 1, (c));

  if (cc) {
    parent_call *pc = get_parent_call(cc->parent);
    gpr_mu_lock(&pc->child_list_mu);
    if (c == pc->first_child) {
      pc->first_child = cc->sibling_next;
      if (c == pc->first_child) {
        pc->first_child = NULL;
      }
    }
    cc->sibling_prev->child_call->sibling_next = cc->sibling_next;
    cc->sibling_next->child_call->sibling_prev = cc->sibling_prev;
    gpr_mu_unlock(&pc->child_list_mu);
    GRPC_CALL_INTERNAL_UNREF(&exec_ctx, cc->parent, "child");
  }

  GPR_ASSERT(!c->destroy_called);
  c->destroy_called = 1;
  bool cancel = gpr_atm_acq_load(&c->any_ops_sent_atm) != 0 &&
                gpr_atm_acq_load(&c->received_final_op_atm) == 0;
  if (cancel) {
    cancel_with_error(&exec_ctx, c, STATUS_FROM_API_OVERRIDE,
                      GRPC_ERROR_CANCELLED);
  } else {
    // Unset the call combiner cancellation closure.  This has the
    // effect of scheduling the previously set cancellation closure, if
    // any, so that it can release any internal references it may be
    // holding to the call stack.
    grpc_call_combiner_set_notify_on_cancel(&exec_ctx, &c->call_combiner, NULL);
  }
  GRPC_CALL_INTERNAL_UNREF(&exec_ctx, c, "destroy");
  grpc_exec_ctx_finish(&exec_ctx);
  GPR_TIMER_END("grpc_call_unref", 0);
}

grpc_call_error grpc_call_cancel(grpc_call *call, void *reserved) {
  GRPC_API_TRACE("grpc_call_cancel(call=%p, reserved=%p)", 2, (call, reserved));
  GPR_ASSERT(!reserved);
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  cancel_with_error(&exec_ctx, call, STATUS_FROM_API_OVERRIDE,
                    GRPC_ERROR_CANCELLED);
  grpc_exec_ctx_finish(&exec_ctx);
  return GRPC_CALL_OK;
}

// This is called via the call combiner to start sending a batch down
// the filter stack.
static void execute_batch_in_call_combiner(grpc_exec_ctx *exec_ctx, void *arg,
                                           grpc_error *ignored) {
  grpc_transport_stream_op_batch *batch = (grpc_transport_stream_op_batch *)arg;
  grpc_call *call = (grpc_call *)batch->handler_private.extra_arg;
  GPR_TIMER_BEGIN("execute_batch", 0);
  grpc_call_element *elem = CALL_ELEM_FROM_CALL(call, 0);
  GRPC_CALL_LOG_OP(GPR_INFO, elem, batch);
  elem->filter->start_transport_stream_op_batch(exec_ctx, elem, batch);
  GPR_TIMER_END("execute_batch", 0);
}

// start_batch_closure points to a caller-allocated closure to be used
// for entering the call combiner.
static void execute_batch(grpc_exec_ctx *exec_ctx, grpc_call *call,
                          grpc_transport_stream_op_batch *batch,
                          grpc_closure *start_batch_closure) {
  batch->handler_private.extra_arg = call;
  GRPC_CLOSURE_INIT(start_batch_closure, execute_batch_in_call_combiner, batch,
                    grpc_schedule_on_exec_ctx);
  GRPC_CALL_COMBINER_START(exec_ctx, &call->call_combiner, start_batch_closure,
                           GRPC_ERROR_NONE, "executing batch");
}

char *grpc_call_get_peer(grpc_call *call) {
  char *peer_string = (char *)gpr_atm_acq_load(&call->peer_string);
  if (peer_string != NULL) return gpr_strdup(peer_string);
  peer_string = grpc_channel_get_target(call->channel);
  if (peer_string != NULL) return peer_string;
  return gpr_strdup("unknown");
}

grpc_call *grpc_call_from_top_element(grpc_call_element *elem) {
  return CALL_FROM_TOP_ELEM(elem);
}

/*******************************************************************************
 * CANCELLATION
 */

grpc_call_error grpc_call_cancel_with_status(grpc_call *c,
                                             grpc_status_code status,
                                             const char *description,
                                             void *reserved) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  GRPC_API_TRACE(
      "grpc_call_cancel_with_status("
      "c=%p, status=%d, description=%s, reserved=%p)",
      4, (c, (int)status, description, reserved));
  GPR_ASSERT(reserved == NULL);
  cancel_with_status(&exec_ctx, c, STATUS_FROM_API_OVERRIDE, status,
                     description);
  grpc_exec_ctx_finish(&exec_ctx);
  return GRPC_CALL_OK;
}

typedef struct {
  grpc_call *call;
  grpc_closure start_batch;
  grpc_closure finish_batch;
} cancel_state;

// The on_complete callback used when sending a cancel_stream batch down
// the filter stack.  Yields the call combiner when the batch is done.
static void done_termination(grpc_exec_ctx *exec_ctx, void *arg,
                             grpc_error *error) {
  cancel_state *state = (cancel_state *)arg;
  GRPC_CALL_COMBINER_STOP(exec_ctx, &state->call->call_combiner,
                          "on_complete for cancel_stream op");
  GRPC_CALL_INTERNAL_UNREF(exec_ctx, state->call, "termination");
  gpr_free(state);
}

static void cancel_with_error(grpc_exec_ctx *exec_ctx, grpc_call *c,
                              status_source source, grpc_error *error) {
  GRPC_CALL_INTERNAL_REF(c, "termination");
  // Inform the call combiner of the cancellation, so that it can cancel
  // any in-flight asynchronous actions that may be holding the call
  // combiner.  This ensures that the cancel_stream batch can be sent
  // down the filter stack in a timely manner.
  grpc_call_combiner_cancel(exec_ctx, &c->call_combiner, GRPC_ERROR_REF(error));
  set_status_from_error(exec_ctx, c, source, GRPC_ERROR_REF(error));
  cancel_state *state = (cancel_state *)gpr_malloc(sizeof(*state));
  state->call = c;
  GRPC_CLOSURE_INIT(&state->finish_batch, done_termination, state,
                    grpc_schedule_on_exec_ctx);
  grpc_transport_stream_op_batch *op =
      grpc_make_transport_stream_op(&state->finish_batch);
  op->cancel_stream = true;
  op->payload->cancel_stream.cancel_error = error;
  execute_batch(exec_ctx, c, op, &state->start_batch);
}

static grpc_error *error_from_status(grpc_status_code status,
                                     const char *description) {
  // copying 'description' is needed to ensure the grpc_call_cancel_with_status
  // guarantee that can be short-lived.
  return grpc_error_set_int(
      grpc_error_set_str(GRPC_ERROR_CREATE_FROM_COPIED_STRING(description),
                         GRPC_ERROR_STR_GRPC_MESSAGE,
                         grpc_slice_from_copied_string(description)),
      GRPC_ERROR_INT_GRPC_STATUS, status);
}

static void cancel_with_status(grpc_exec_ctx *exec_ctx, grpc_call *c,
                               status_source source, grpc_status_code status,
                               const char *description) {
  cancel_with_error(exec_ctx, c, source,
                    error_from_status(status, description));
}

/*******************************************************************************
 * FINAL STATUS CODE MANIPULATION
 */

static bool get_final_status_from(
    grpc_call *call, grpc_error *error, bool allow_ok_status,
    void (*set_value)(grpc_status_code code, void *user_data),
    void *set_value_user_data, grpc_slice *details) {
  grpc_status_code code;
  grpc_slice slice = grpc_empty_slice();
  grpc_error_get_status(error, call->send_deadline, &code, &slice, NULL);
  if (code == GRPC_STATUS_OK && !allow_ok_status) {
    return false;
  }

  set_value(code, set_value_user_data);
  if (details != NULL) {
    *details = grpc_slice_ref_internal(slice);
  }
  return true;
}

static void get_final_status(grpc_call *call,
                             void (*set_value)(grpc_status_code code,
                                               void *user_data),
                             void *set_value_user_data, grpc_slice *details) {
  int i;
  received_status status[STATUS_SOURCE_COUNT];
  for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
    status[i] = unpack_received_status(gpr_atm_acq_load(&call->status[i]));
  }
  if (GRPC_TRACER_ON(grpc_call_error_trace)) {
    gpr_log(GPR_DEBUG, "get_final_status %s", call->is_client ? "CLI" : "SVR");
    for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
      if (status[i].is_set) {
        gpr_log(GPR_DEBUG, "  %d: %s", i, grpc_error_string(status[i].error));
      }
    }
  }
  /* first search through ignoring "OK" statuses: if something went wrong,
   * ensure we report it */
  for (int allow_ok_status = 0; allow_ok_status < 2; allow_ok_status++) {
    /* search for the best status we can present: ideally the error we use has a
       clearly defined grpc-status, and we'll prefer that. */
    for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
      if (status[i].is_set &&
          grpc_error_has_clear_grpc_status(status[i].error)) {
        if (get_final_status_from(call, status[i].error, allow_ok_status != 0,
                                  set_value, set_value_user_data, details)) {
          return;
        }
      }
    }
    /* If no clearly defined status exists, search for 'anything' */
    for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
      if (status[i].is_set) {
        if (get_final_status_from(call, status[i].error, allow_ok_status != 0,
                                  set_value, set_value_user_data, details)) {
          return;
        }
      }
    }
  }
  /* If nothing exists, set some default */
  if (call->is_client) {
    set_value(GRPC_STATUS_UNKNOWN, set_value_user_data);
  } else {
    set_value(GRPC_STATUS_OK, set_value_user_data);
  }
}

static void set_status_from_error(grpc_exec_ctx *exec_ctx, grpc_call *call,
                                  status_source source, grpc_error *error) {
  if (!gpr_atm_rel_cas(&call->status[source],
                       pack_received_status((received_status){
                           .is_set = false, .error = GRPC_ERROR_NONE}),
                       pack_received_status((received_status){
                           .is_set = true, .error = error}))) {
    GRPC_ERROR_UNREF(error);
  }
}

/*******************************************************************************
 * COMPRESSION
 */

static void set_incoming_message_compression_algorithm(
    grpc_call *call, grpc_message_compression_algorithm algo) {
  GPR_ASSERT(algo < GRPC_MESSAGE_COMPRESS_ALGORITHMS_COUNT);
  call->incoming_message_compression_algorithm = algo;
}

static void set_incoming_stream_compression_algorithm(
    grpc_call *call, grpc_stream_compression_algorithm algo) {
  GPR_ASSERT(algo < GRPC_STREAM_COMPRESS_ALGORITHMS_COUNT);
  call->incoming_stream_compression_algorithm = algo;
}

grpc_compression_algorithm
grpc_call_test_only_get_compression_algorithm(grpc_call *call) {
  grpc_compression_algorithm algorithm = GRPC_COMPRESS_NONE;
  grpc_compression_algorithm_from_message_stream_compression_algorithm(
      &algorithm, call->incoming_message_compression_algorithm,
      call->incoming_stream_compression_algorithm);
  return algorithm;
}

static grpc_compression_algorithm compression_algorithm_for_level_locked(
    grpc_call *call, grpc_compression_level level) {
  return grpc_compression_algorithm_for_level(level,
                                              call->encodings_accepted_by_peer);
}

uint32_t grpc_call_test_only_get_message_flags(grpc_call *call) {
  uint32_t flags;
  flags = call->test_only_last_message_flags;
  return flags;
}

static void destroy_encodings_accepted_by_peer(void *p) { return; }

static void set_encodings_accepted_by_peer(grpc_exec_ctx *exec_ctx,
                                           grpc_call *call, grpc_mdelem mdel,
                                           uint32_t *encodings_accepted_by_peer,
                                           bool stream_encoding) {
  size_t i;
  uint32_t algorithm;
  grpc_slice_buffer accept_encoding_parts;
  grpc_slice accept_encoding_slice;
  void *accepted_user_data;

  accepted_user_data =
      grpc_mdelem_get_user_data(mdel, destroy_encodings_accepted_by_peer);
  if (accepted_user_data != NULL) {
    *encodings_accepted_by_peer =
        (uint32_t)(((uintptr_t)accepted_user_data) - 1);
    return;
  }

  *encodings_accepted_by_peer = 0;

  accept_encoding_slice = GRPC_MDVALUE(mdel);
  grpc_slice_buffer_init(&accept_encoding_parts);
  grpc_slice_split_without_space(accept_encoding_slice, ",", &accept_encoding_parts);

  GPR_BITSET(encodings_accepted_by_peer, GRPC_COMPRESS_NONE);
  for (i = 0; i < accept_encoding_parts.count; i++) {
    int r;
    grpc_slice accept_encoding_entry_slice = accept_encoding_parts.slices[i];
    if (!stream_encoding) {
      r = grpc_message_compression_algorithm_parse(
          accept_encoding_entry_slice,
          (grpc_message_compression_algorithm *)&algorithm);
    } else {
      r = grpc_stream_compression_algorithm_parse(
          accept_encoding_entry_slice,
          (grpc_stream_compression_algorithm *)&algorithm);
    }
    if (r) {
      GPR_BITSET(encodings_accepted_by_peer, algorithm);
    } else {
      char *accept_encoding_entry_str =
          grpc_slice_to_c_string(accept_encoding_entry_slice);
      gpr_log(GPR_ERROR,
              "Invalid entry in accept encoding metadata: '%s'. Ignoring.",
              accept_encoding_entry_str);
      gpr_free(accept_encoding_entry_str);
    }
  }

  grpc_slice_buffer_destroy_internal(exec_ctx, &accept_encoding_parts);

  grpc_mdelem_set_user_data(
      mdel, destroy_encodings_accepted_by_peer,
      (void *)(((uintptr_t)(*encodings_accepted_by_peer)) + 1));
}

uint32_t grpc_call_test_only_get_encodings_accepted_by_peer(grpc_call *call) {
  uint32_t encodings_accepted_by_peer;
  encodings_accepted_by_peer = call->encodings_accepted_by_peer;
  return encodings_accepted_by_peer;
}

grpc_stream_compression_algorithm
grpc_call_test_only_get_incoming_stream_encodings(grpc_call *call) {
  return call->incoming_stream_compression_algorithm;
}

static grpc_linked_mdelem *linked_from_md(const grpc_metadata *md) {
  return (grpc_linked_mdelem *)&md->internal_data;
}

static grpc_metadata *get_md_elem(grpc_metadata *metadata,
                                  grpc_metadata *additional_metadata, int i,
                                  int count) {
  grpc_metadata *res =
      i < count ? &metadata[i] : &additional_metadata[i - count];
  GPR_ASSERT(res);
  return res;
}

static int prepare_application_metadata(
    grpc_exec_ctx *exec_ctx, grpc_call *call, int count,
    grpc_metadata *metadata, int is_trailing, int prepend_extra_metadata,
    grpc_metadata *additional_metadata, int additional_metadata_count) {
  int total_count = count + additional_metadata_count;
  int i;
  grpc_metadata_batch *batch =
      &call->metadata_batch[0 /* is_receiving */][is_trailing];
  for (i = 0; i < total_count; i++) {
    const grpc_metadata *md =
        get_md_elem(metadata, additional_metadata, i, count);
    grpc_linked_mdelem *l = linked_from_md(md);
    GPR_ASSERT(sizeof(grpc_linked_mdelem) == sizeof(md->internal_data));
    if (!GRPC_LOG_IF_ERROR("validate_metadata",
                           grpc_validate_header_key_is_legal(md->key))) {
      break;
    } else if (!grpc_is_binary_header(md->key) &&
               !GRPC_LOG_IF_ERROR(
                   "validate_metadata",
                   grpc_validate_header_nonbin_value_is_legal(md->value))) {
      break;
    }
    l->md = grpc_mdelem_from_grpc_metadata(exec_ctx, (grpc_metadata *)md);
  }
  if (i != total_count) {
    for (int j = 0; j < i; j++) {
      const grpc_metadata *md =
          get_md_elem(metadata, additional_metadata, j, count);
      grpc_linked_mdelem *l = linked_from_md(md);
      GRPC_MDELEM_UNREF(exec_ctx, l->md);
    }
    return 0;
  }
  if (prepend_extra_metadata) {
    if (call->send_extra_metadata_count == 0) {
      prepend_extra_metadata = 0;
    } else {
      for (i = 0; i < call->send_extra_metadata_count; i++) {
        GRPC_LOG_IF_ERROR("prepare_application_metadata",
                          grpc_metadata_batch_link_tail(
                              exec_ctx, batch, &call->send_extra_metadata[i]));
      }
    }
  }
  for (i = 0; i < total_count; i++) {
    grpc_metadata *md = get_md_elem(metadata, additional_metadata, i, count);
    grpc_linked_mdelem *l = linked_from_md(md);
    grpc_error *error = grpc_metadata_batch_link_tail(exec_ctx, batch, l);
    if (error != GRPC_ERROR_NONE) {
      GRPC_MDELEM_UNREF(exec_ctx, l->md);
    }
    GRPC_LOG_IF_ERROR("prepare_application_metadata", error);
  }
  call->send_extra_metadata_count = 0;

  return 1;
}

/* we offset status by a small amount when storing it into transport metadata
   as metadata cannot store a 0 value (which is used as OK for grpc_status_codes
   */
#define STATUS_OFFSET 1
static void destroy_status(void *ignored) {}

static uint32_t decode_status(grpc_mdelem md) {
  uint32_t status;
  void *user_data;
  if (grpc_mdelem_eq(md, GRPC_MDELEM_GRPC_STATUS_0)) return 0;
  if (grpc_mdelem_eq(md, GRPC_MDELEM_GRPC_STATUS_1)) return 1;
  if (grpc_mdelem_eq(md, GRPC_MDELEM_GRPC_STATUS_2)) return 2;
  user_data = grpc_mdelem_get_user_data(md, destroy_status);
  if (user_data != NULL) {
    status = ((uint32_t)(intptr_t)user_data) - STATUS_OFFSET;
  } else {
    if (!grpc_parse_slice_to_uint32(GRPC_MDVALUE(md), &status)) {
      status = GRPC_STATUS_UNKNOWN; /* could not parse status code */
    }
    grpc_mdelem_set_user_data(md, destroy_status,
                              (void *)(intptr_t)(status + STATUS_OFFSET));
  }
  return status;
}

static grpc_message_compression_algorithm decode_message_compression(grpc_mdelem md) {
  grpc_message_compression_algorithm algorithm =
      grpc_message_compression_algorithm_from_slice(GRPC_MDVALUE(md));
  if (algorithm == GRPC_MESSAGE_COMPRESS_ALGORITHMS_COUNT) {
    char *md_c_str = grpc_slice_to_c_string(GRPC_MDVALUE(md));
    gpr_log(GPR_ERROR,
            "Invalid incoming message compression algorithm: '%s'. "
            "Interpreting incoming data as uncompressed.",
            md_c_str);
    gpr_free(md_c_str);
    return GRPC_MESSAGE_COMPRESS_NONE;
  }
  return algorithm;
}

static grpc_stream_compression_algorithm decode_stream_compression(
    grpc_mdelem md) {
  grpc_stream_compression_algorithm algorithm =
      grpc_stream_compression_algorithm_from_slice(GRPC_MDVALUE(md));
  if (algorithm == GRPC_STREAM_COMPRESS_ALGORITHMS_COUNT) {
    char *md_c_str = grpc_slice_to_c_string(GRPC_MDVALUE(md));
    gpr_log(GPR_ERROR,
            "Invalid incoming stream compression algorithm: '%s'. Interpreting "
            "incoming data as uncompressed.",
            md_c_str);
    gpr_free(md_c_str);
    return GRPC_STREAM_COMPRESS_NONE;
  }
  return algorithm;
}

static void publish_app_metadata(grpc_call *call, grpc_metadata_batch *b,
                                 int is_trailing) {
  if (b->list.count == 0) return;
  GPR_TIMER_BEGIN("publish_app_metadata", 0);
  grpc_metadata_array *dest;
  grpc_metadata *mdusr;
  dest = call->buffered_metadata[is_trailing];
  if (dest->count + b->list.count > dest->capacity) {
    dest->capacity =
        GPR_MAX(dest->capacity + b->list.count, dest->capacity * 3 / 2);
    dest->metadata = (grpc_metadata *)gpr_realloc(
        dest->metadata, sizeof(grpc_metadata) * dest->capacity);
  }
  for (grpc_linked_mdelem *l = b->list.head; l != NULL; l = l->next) {
    mdusr = &dest->metadata[dest->count++];
    /* we pass back borrowed slices that are valid whilst the call is valid */
    mdusr->key = GRPC_MDKEY(l->md);
    mdusr->value = GRPC_MDVALUE(l->md);
  }
  GPR_TIMER_END("publish_app_metadata", 0);
}

static void recv_initial_filter(grpc_exec_ctx *exec_ctx, grpc_call *call,
                                grpc_metadata_batch *b) {
  if (b->idx.named.content_encoding != NULL) {
    GPR_TIMER_BEGIN("incoming_stream_compression_algorithm", 0);
    set_incoming_stream_compression_algorithm(
        call, decode_stream_compression(b->idx.named.content_encoding->md));
    GPR_TIMER_END("incoming_stream_compression_algorithm", 0);
    grpc_metadata_batch_remove(exec_ctx, b, b->idx.named.content_encoding);
  }
  if (b->idx.named.grpc_encoding != NULL) {
    GPR_TIMER_BEGIN("incoming_message_compression_algorithm", 0);
    set_incoming_message_compression_algorithm(
        call, decode_message_compression(b->idx.named.grpc_encoding->md));
    GPR_TIMER_END("incoming_message_compression_algorithm", 0);
    grpc_metadata_batch_remove(exec_ctx, b, b->idx.named.grpc_encoding);
  }
  uint32_t message_encodings_accepted_by_peer = 1u;
  uint32_t stream_encodings_accepted_by_peer = 1u;
  if (b->idx.named.grpc_accept_encoding != NULL) {
    GPR_TIMER_BEGIN("encodings_accepted_by_peer", 0);
    set_encodings_accepted_by_peer(exec_ctx, call,
                                   b->idx.named.grpc_accept_encoding->md,
                                   &message_encodings_accepted_by_peer, false);
    grpc_metadata_batch_remove(exec_ctx, b, b->idx.named.grpc_accept_encoding);
    GPR_TIMER_END("encodings_accepted_by_peer", 0);
  }
  if (b->idx.named.accept_encoding != NULL) {
    GPR_TIMER_BEGIN("stream_encodings_accepted_by_peer", 0);
    set_encodings_accepted_by_peer(exec_ctx, call,
                                   b->idx.named.accept_encoding->md,
                                   &stream_encodings_accepted_by_peer, true);
    grpc_metadata_batch_remove(exec_ctx, b, b->idx.named.accept_encoding);
    GPR_TIMER_END("stream_encodings_accepted_by_peer", 0);
  }
  call->encodings_accepted_by_peer =
      grpc_compression_bitset_from_message_stream_compression_bitset(
          message_encodings_accepted_by_peer,
          stream_encodings_accepted_by_peer);
  publish_app_metadata(call, b, false);
}

static void recv_trailing_filter(grpc_exec_ctx *exec_ctx, void *args,
                                 grpc_metadata_batch *b) {
  grpc_call *call = (grpc_call *)args;
  if (b->idx.named.grpc_status != NULL) {
    uint32_t status_code = decode_status(b->idx.named.grpc_status->md);
    grpc_error *error =
        status_code == GRPC_STATUS_OK
            ? GRPC_ERROR_NONE
            : grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                     "Error received from peer"),
                                 GRPC_ERROR_INT_GRPC_STATUS,
                                 (intptr_t)status_code);
    if (b->idx.named.grpc_message != NULL) {
      error = grpc_error_set_str(
          error, GRPC_ERROR_STR_GRPC_MESSAGE,
          grpc_slice_ref_internal(GRPC_MDVALUE(b->idx.named.grpc_message->md)));
      grpc_metadata_batch_remove(exec_ctx, b, b->idx.named.grpc_message);
    } else if (error != GRPC_ERROR_NONE) {
      error = grpc_error_set_str(error, GRPC_ERROR_STR_GRPC_MESSAGE,
                                 grpc_empty_slice());
    }
    set_status_from_error(exec_ctx, call, STATUS_FROM_WIRE, error);
    grpc_metadata_batch_remove(exec_ctx, b, b->idx.named.grpc_status);
  }
  publish_app_metadata(call, b, true);
}

grpc_call_stack *grpc_call_get_call_stack(grpc_call *call) {
  return CALL_STACK_FROM_CALL(call);
}

/*******************************************************************************
 * BATCH API IMPLEMENTATION
 */

static void set_status_value_directly(grpc_status_code status, void *dest) {
  *(grpc_status_code *)dest = status;
}

static void set_cancelled_value(grpc_status_code status, void *dest) {
  *(int *)dest = (status != GRPC_STATUS_OK);
}

static bool are_write_flags_valid(uint32_t flags) {
  /* check that only bits in GRPC_WRITE_(INTERNAL?)_USED_MASK are set */
  const uint32_t allowed_write_positions =
      (GRPC_WRITE_USED_MASK | GRPC_WRITE_INTERNAL_USED_MASK);
  const uint32_t invalid_positions = ~allowed_write_positions;
  return !(flags & invalid_positions);
}

static bool are_initial_metadata_flags_valid(uint32_t flags, bool is_client) {
  /* check that only bits in GRPC_WRITE_(INTERNAL?)_USED_MASK are set */
  uint32_t invalid_positions = ~GRPC_INITIAL_METADATA_USED_MASK;
  if (!is_client) {
    invalid_positions |= GRPC_INITIAL_METADATA_IDEMPOTENT_REQUEST;
  }
  return !(flags & invalid_positions);
}

static int batch_slot_for_op(grpc_op_type type) {
  switch (type) {
    case GRPC_OP_SEND_INITIAL_METADATA:
      return 0;
    case GRPC_OP_SEND_MESSAGE:
      return 1;
    case GRPC_OP_SEND_CLOSE_FROM_CLIENT:
    case GRPC_OP_SEND_STATUS_FROM_SERVER:
      return 2;
    case GRPC_OP_RECV_INITIAL_METADATA:
      return 3;
    case GRPC_OP_RECV_MESSAGE:
      return 4;
    case GRPC_OP_RECV_CLOSE_ON_SERVER:
    case GRPC_OP_RECV_STATUS_ON_CLIENT:
      return 5;
  }
  GPR_UNREACHABLE_CODE(return 123456789);
}

static batch_control *allocate_batch_control(grpc_call *call,
                                             const grpc_op *ops,
                                             size_t num_ops) {
  int slot = batch_slot_for_op(ops[0].op);
  batch_control **pslot = &call->active_batches[slot];
  if (*pslot == NULL) {
    *pslot =
        (batch_control *)gpr_arena_alloc(call->arena, sizeof(batch_control));
  }
  batch_control *bctl = *pslot;
  if (bctl->call != NULL) {
    return NULL;
  }
  memset(bctl, 0, sizeof(*bctl));
  bctl->call = call;
  bctl->op.payload = &call->stream_op_payload;
  return bctl;
}

static void finish_batch_completion(grpc_exec_ctx *exec_ctx, void *user_data,
                                    grpc_cq_completion *storage) {
  batch_control *bctl = (batch_control *)user_data;
  grpc_call *call = bctl->call;
  bctl->call = NULL;
  GRPC_CALL_INTERNAL_UNREF(exec_ctx, call, "completion");
}

static grpc_error *consolidate_batch_errors(batch_control *bctl) {
  size_t n = (size_t)gpr_atm_acq_load(&bctl->num_errors);
  if (n == 0) {
    return GRPC_ERROR_NONE;
  } else if (n == 1) {
    /* Skip creating a composite error in the case that only one error was
       logged */
    grpc_error *e = bctl->errors[0];
    bctl->errors[0] = NULL;
    return e;
  } else {
    grpc_error *error = GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
        "Call batch failed", bctl->errors, n);
    for (size_t i = 0; i < n; i++) {
      GRPC_ERROR_UNREF(bctl->errors[i]);
      bctl->errors[i] = NULL;
    }
    return error;
  }
}

static void post_batch_completion(grpc_exec_ctx *exec_ctx,
                                  batch_control *bctl) {
  grpc_call *next_child_call;
  grpc_call *call = bctl->call;
  grpc_error *error = consolidate_batch_errors(bctl);

  if (bctl->op.send_initial_metadata) {
    grpc_metadata_batch_destroy(
        exec_ctx,
        &call->metadata_batch[0 /* is_receiving */][0 /* is_trailing */]);
  }
  if (bctl->op.send_message) {
    call->sending_message = false;
  }
  if (bctl->op.send_trailing_metadata) {
    grpc_metadata_batch_destroy(
        exec_ctx,
        &call->metadata_batch[0 /* is_receiving */][1 /* is_trailing */]);
  }
  if (bctl->op.recv_trailing_metadata) {
    grpc_metadata_batch *md =
        &call->metadata_batch[1 /* is_receiving */][1 /* is_trailing */];
    recv_trailing_filter(exec_ctx, call, md);

    /* propagate cancellation to any interested children */
    gpr_atm_rel_store(&call->received_final_op_atm, 1);
    parent_call *pc = get_parent_call(call);
    if (pc != NULL) {
      grpc_call *child;
      gpr_mu_lock(&pc->child_list_mu);
      child = pc->first_child;
      if (child != NULL) {
        do {
          next_child_call = child->child_call->sibling_next;
          if (child->cancellation_is_inherited) {
            GRPC_CALL_INTERNAL_REF(child, "propagate_cancel");
            cancel_with_error(exec_ctx, child, STATUS_FROM_API_OVERRIDE,
                              GRPC_ERROR_CANCELLED);
            GRPC_CALL_INTERNAL_UNREF(exec_ctx, child, "propagate_cancel");
          }
          child = next_child_call;
        } while (child != pc->first_child);
      }
      gpr_mu_unlock(&pc->child_list_mu);
    }

    if (call->is_client) {
      get_final_status(call, set_status_value_directly,
                       call->final_op.client.status,
                       call->final_op.client.status_details);
    } else {
      get_final_status(call, set_cancelled_value,
                       call->final_op.server.cancelled, NULL);
    }

    GRPC_ERROR_UNREF(error);
    error = GRPC_ERROR_NONE;
  }

  if (bctl->completion_data.notify_tag.is_closure) {
    /* unrefs bctl->error */
    bctl->call = NULL;
    GRPC_CLOSURE_RUN(exec_ctx, bctl->completion_data.notify_tag.tag, error);
    GRPC_CALL_INTERNAL_UNREF(exec_ctx, call, "completion");
  } else {
    /* unrefs bctl->error */
    grpc_cq_end_op(
        exec_ctx, bctl->call->cq, bctl->completion_data.notify_tag.tag, error,
        finish_batch_completion, bctl, &bctl->completion_data.cq_completion);
  }
}

static void finish_batch_step(grpc_exec_ctx *exec_ctx, batch_control *bctl) {
  if (gpr_unref(&bctl->steps_to_complete)) {
    post_batch_completion(exec_ctx, bctl);
  }
}

static void continue_receiving_slices(grpc_exec_ctx *exec_ctx,
                                      batch_control *bctl) {
  grpc_error *error;
  grpc_call *call = bctl->call;
  for (;;) {
    size_t remaining = call->receiving_stream->length -
                       (*call->receiving_buffer)->data.raw.slice_buffer.length;
    if (remaining == 0) {
      call->receiving_message = 0;
      grpc_byte_stream_destroy(exec_ctx, call->receiving_stream);
      call->receiving_stream = NULL;
      finish_batch_step(exec_ctx, bctl);
      return;
    }
    if (grpc_byte_stream_next(exec_ctx, call->receiving_stream, remaining,
                              &call->receiving_slice_ready)) {
      error = grpc_byte_stream_pull(exec_ctx, call->receiving_stream,
                                    &call->receiving_slice);
      if (error == GRPC_ERROR_NONE) {
        grpc_slice_buffer_add(&(*call->receiving_buffer)->data.raw.slice_buffer,
                              call->receiving_slice);
      } else {
        grpc_byte_stream_destroy(exec_ctx, call->receiving_stream);
        call->receiving_stream = NULL;
        grpc_byte_buffer_destroy(*call->receiving_buffer);
        *call->receiving_buffer = NULL;
        call->receiving_message = 0;
        finish_batch_step(exec_ctx, bctl);
        return;
      }
    } else {
      return;
    }
  }
}

static void receiving_slice_ready(grpc_exec_ctx *exec_ctx, void *bctlp,
                                  grpc_error *error) {
  batch_control *bctl = (batch_control *)bctlp;
  grpc_call *call = bctl->call;
  grpc_byte_stream *bs = call->receiving_stream;
  bool release_error = false;

  if (error == GRPC_ERROR_NONE) {
    grpc_slice slice;
    error = grpc_byte_stream_pull(exec_ctx, bs, &slice);
    if (error == GRPC_ERROR_NONE) {
      grpc_slice_buffer_add(&(*call->receiving_buffer)->data.raw.slice_buffer,
                            slice);
      continue_receiving_slices(exec_ctx, bctl);
    } else {
      /* Error returned by grpc_byte_stream_pull needs to be released manually
       */
      release_error = true;
    }
  }

  if (error != GRPC_ERROR_NONE) {
    if (GRPC_TRACER_ON(grpc_trace_operation_failures)) {
      GRPC_LOG_IF_ERROR("receiving_slice_ready", GRPC_ERROR_REF(error));
    }
    grpc_byte_stream_destroy(exec_ctx, call->receiving_stream);
    call->receiving_stream = NULL;
    grpc_byte_buffer_destroy(*call->receiving_buffer);
    *call->receiving_buffer = NULL;
    call->receiving_message = 0;
    finish_batch_step(exec_ctx, bctl);
    if (release_error) {
      GRPC_ERROR_UNREF(error);
    }
  }
}

static void process_data_after_md(grpc_exec_ctx *exec_ctx,
                                  batch_control *bctl) {
  grpc_call *call = bctl->call;
  if (call->receiving_stream == NULL) {
    *call->receiving_buffer = NULL;
    call->receiving_message = 0;
    finish_batch_step(exec_ctx, bctl);
  } else {
    call->test_only_last_message_flags = call->receiving_stream->flags;
    if ((call->receiving_stream->flags & GRPC_WRITE_INTERNAL_COMPRESS) &&
        (call->incoming_message_compression_algorithm > GRPC_MESSAGE_COMPRESS_NONE)) {
      grpc_compression_algorithm algo;
      GPR_ASSERT(grpc_compression_algorithm_from_message_stream_compression_algorithm(
          &algo, call->incoming_message_compression_algorithm, 0));
      *call->receiving_buffer = grpc_raw_compressed_byte_buffer_create(
          NULL, 0, algo);
    } else {
      *call->receiving_buffer = grpc_raw_byte_buffer_create(NULL, 0);
    }
    GRPC_CLOSURE_INIT(&call->receiving_slice_ready, receiving_slice_ready, bctl,
                      grpc_schedule_on_exec_ctx);
    continue_receiving_slices(exec_ctx, bctl);
  }
}

static void receiving_stream_ready(grpc_exec_ctx *exec_ctx, void *bctlp,
                                   grpc_error *error) {
  batch_control *bctl = (batch_control *)bctlp;
  grpc_call *call = bctl->call;
  if (error != GRPC_ERROR_NONE) {
    if (call->receiving_stream != NULL) {
      grpc_byte_stream_destroy(exec_ctx, call->receiving_stream);
      call->receiving_stream = NULL;
    }
    add_batch_error(exec_ctx, bctl, GRPC_ERROR_REF(error), true);
    cancel_with_error(exec_ctx, call, STATUS_FROM_SURFACE,
                      GRPC_ERROR_REF(error));
  }
  /* If recv_state is RECV_NONE, we will save the batch_control
   * object with rel_cas, and will not use it after the cas. Its corresponding
   * acq_load is in receiving_initial_metadata_ready() */
  if (error != GRPC_ERROR_NONE || call->receiving_stream == NULL ||
      !gpr_atm_rel_cas(&call->recv_state, RECV_NONE, (gpr_atm)bctlp)) {
    process_data_after_md(exec_ctx, bctlp);
  }
}

// The recv_message_ready callback used when sending a batch containing
// a recv_message op down the filter stack.  Yields the call combiner
// before processing the received message.
static void receiving_stream_ready_in_call_combiner(grpc_exec_ctx *exec_ctx,
                                                    void *bctlp,
                                                    grpc_error *error) {
  batch_control *bctl = (batch_control *)bctlp;
  grpc_call *call = bctl->call;
  GRPC_CALL_COMBINER_STOP(exec_ctx, &call->call_combiner, "recv_message_ready");
  receiving_stream_ready(exec_ctx, bctlp, error);
}

static void validate_filtered_metadata(grpc_exec_ctx *exec_ctx,
                                       batch_control *bctl) {
  grpc_compression_algorithm compression_algorithm;
  grpc_call *call = bctl->call;
  if (call->incoming_stream_compression_algorithm !=
          GRPC_STREAM_COMPRESS_NONE &&
      call->incoming_message_compression_algorithm != GRPC_MESSAGE_COMPRESS_NONE) {
    char *error_msg = NULL;
    gpr_asprintf(&error_msg,
                 "Incoming stream has both stream compression (%d) and message "
                 "compression (%d).",
                 call->incoming_stream_compression_algorithm,
                 call->incoming_message_compression_algorithm);
    gpr_log(GPR_ERROR, "%s", error_msg);
    cancel_with_status(exec_ctx, call, STATUS_FROM_SURFACE,
                       GRPC_STATUS_INTERNAL, error_msg);
    gpr_free(error_msg);
  } else if (
      grpc_compression_algorithm_from_message_stream_compression_algorithm(
          &compression_algorithm, call->incoming_message_compression_algorithm,
          call->incoming_stream_compression_algorithm) == 0) {
    char *error_msg = NULL;
    gpr_asprintf(&error_msg,
                 "Error in incoming message compression (%d) or stream "
                 "compression (%d).",
                 call->incoming_stream_compression_algorithm,
                 call->incoming_message_compression_algorithm);
    cancel_with_status(exec_ctx, call, STATUS_FROM_SURFACE,
                       GRPC_STATUS_INTERNAL, error_msg);
    gpr_free(error_msg);
  } else {
    char *error_msg = NULL;
    const grpc_compression_options compression_options =
        grpc_channel_compression_options(call->channel);
    if (compression_algorithm >= GRPC_COMPRESS_ALGORITHMS_COUNT) {
      gpr_asprintf(&error_msg, "Invalid compression algorithm value '%d'.",
                   compression_algorithm);
      gpr_log(GPR_ERROR, "%s", error_msg);
      cancel_with_status(exec_ctx, call, STATUS_FROM_SURFACE,
                         GRPC_STATUS_UNIMPLEMENTED, error_msg);
    } else if (grpc_compression_options_is_algorithm_enabled(
                   &compression_options, compression_algorithm) == 0) {
      /* check if algorithm is supported by current channel config */
      char *algo_name = NULL;
      grpc_compression_algorithm_name(compression_algorithm, &algo_name);
      gpr_asprintf(&error_msg, "Compression algorithm '%s' is disabled.",
                   algo_name);
      gpr_log(GPR_ERROR, "%s", error_msg);
      cancel_with_status(exec_ctx, call, STATUS_FROM_SURFACE,
                         GRPC_STATUS_UNIMPLEMENTED, error_msg);
    }
    gpr_free(error_msg);

    GPR_ASSERT(call->encodings_accepted_by_peer != 0);
    if (!GPR_BITGET(call->encodings_accepted_by_peer, compression_algorithm)) {
      if (GRPC_TRACER_ON(grpc_compression_trace)) {
        char *algo_name = NULL;
        grpc_compression_algorithm_name(compression_algorithm, &algo_name);
        gpr_log(GPR_ERROR,
                "Compression algorithm ('%s') not present in the bitset of "
                "accepted encodings ('0x%x')",
                algo_name, call->encodings_accepted_by_peer);
      }
    }
  }
}

static void add_batch_error(grpc_exec_ctx *exec_ctx, batch_control *bctl,
                            grpc_error *error, bool has_cancelled) {
  if (error == GRPC_ERROR_NONE) return;
  int idx = (int)gpr_atm_full_fetch_add(&bctl->num_errors, 1);
  if (idx == 0 && !has_cancelled) {
    cancel_with_error(exec_ctx, bctl->call, STATUS_FROM_CORE,
                      GRPC_ERROR_REF(error));
  }
  bctl->errors[idx] = error;
}

static void receiving_initial_metadata_ready(grpc_exec_ctx *exec_ctx,
                                             void *bctlp, grpc_error *error) {
  batch_control *bctl = (batch_control *)bctlp;
  grpc_call *call = bctl->call;

  GRPC_CALL_COMBINER_STOP(exec_ctx, &call->call_combiner,
                          "recv_initial_metadata_ready");

  add_batch_error(exec_ctx, bctl, GRPC_ERROR_REF(error), false);
  if (error == GRPC_ERROR_NONE) {
    grpc_metadata_batch *md =
        &call->metadata_batch[1 /* is_receiving */][0 /* is_trailing */];
    recv_initial_filter(exec_ctx, call, md);

    /* TODO(ctiller): this could be moved into recv_initial_filter now */
    GPR_TIMER_BEGIN("validate_filtered_metadata", 0);
    validate_filtered_metadata(exec_ctx, bctl);
    GPR_TIMER_END("validate_filtered_metadata", 0);

    if (gpr_time_cmp(md->deadline, gpr_inf_future(md->deadline.clock_type)) !=
            0 &&
        !call->is_client) {
      call->send_deadline =
          gpr_convert_clock_type(md->deadline, GPR_CLOCK_MONOTONIC);
    }
  }

  grpc_closure *saved_rsr_closure = NULL;
  while (true) {
    gpr_atm rsr_bctlp = gpr_atm_acq_load(&call->recv_state);
    /* Should only receive initial metadata once */
    GPR_ASSERT(rsr_bctlp != 1);
    if (rsr_bctlp == 0) {
      /* We haven't seen initial metadata and messages before, thus initial
       * metadata is received first.
       * no_barrier_cas is used, as this function won't access the batch_control
       * object saved by receiving_stream_ready() if the initial metadata is
       * received first. */
      if (gpr_atm_no_barrier_cas(&call->recv_state, RECV_NONE,
                                 RECV_INITIAL_METADATA_FIRST)) {
        break;
      }
    } else {
      /* Already received messages */
      saved_rsr_closure = GRPC_CLOSURE_CREATE(receiving_stream_ready,
                                              (batch_control *)rsr_bctlp,
                                              grpc_schedule_on_exec_ctx);
      /* No need to modify recv_state */
      break;
    }
  }
  if (saved_rsr_closure != NULL) {
    GRPC_CLOSURE_RUN(exec_ctx, saved_rsr_closure, GRPC_ERROR_REF(error));
  }

  finish_batch_step(exec_ctx, bctl);
}

static void finish_batch(grpc_exec_ctx *exec_ctx, void *bctlp,
                         grpc_error *error) {
  batch_control *bctl = (batch_control *)bctlp;
  grpc_call *call = bctl->call;
  GRPC_CALL_COMBINER_STOP(exec_ctx, &call->call_combiner, "on_complete");
  add_batch_error(exec_ctx, bctl, GRPC_ERROR_REF(error), false);
  finish_batch_step(exec_ctx, bctl);
}

static void free_no_op_completion(grpc_exec_ctx *exec_ctx, void *p,
                                  grpc_cq_completion *completion) {
  gpr_free(completion);
}

static grpc_call_error call_start_batch(grpc_exec_ctx *exec_ctx,
                                        grpc_call *call, const grpc_op *ops,
                                        size_t nops, void *notify_tag,
                                        int is_notify_tag_closure) {
  size_t i;
  const grpc_op *op;
  batch_control *bctl;
  int num_completion_callbacks_needed = 1;
  grpc_call_error error = GRPC_CALL_OK;

  GPR_TIMER_BEGIN("grpc_call_start_batch", 0);
  GRPC_CALL_LOG_BATCH(GPR_INFO, call, ops, nops, notify_tag);

  if (nops == 0) {
    if (!is_notify_tag_closure) {
      GPR_ASSERT(grpc_cq_begin_op(call->cq, notify_tag));
      grpc_cq_end_op(exec_ctx, call->cq, notify_tag, GRPC_ERROR_NONE,
                     free_no_op_completion, NULL,
                     gpr_malloc(sizeof(grpc_cq_completion)));
    } else {
      GRPC_CLOSURE_SCHED(exec_ctx, notify_tag, GRPC_ERROR_NONE);
    }
    error = GRPC_CALL_OK;
    goto done;
  }

  bctl = allocate_batch_control(call, ops, nops);
  if (bctl == NULL) {
    return GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
  }
  bctl->completion_data.notify_tag.tag = notify_tag;
  bctl->completion_data.notify_tag.is_closure =
      (uint8_t)(is_notify_tag_closure != 0);

  grpc_transport_stream_op_batch *stream_op = &bctl->op;
  grpc_transport_stream_op_batch_payload *stream_op_payload =
      &call->stream_op_payload;

  /* rewrite batch ops into a transport op */
  for (i = 0; i < nops; i++) {
    op = &ops[i];
    if (op->reserved != NULL) {
      error = GRPC_CALL_ERROR;
      goto done_with_error;
    }
    switch (op->op) {
      case GRPC_OP_SEND_INITIAL_METADATA:
        /* Flag validation: currently allow no flags */
        if (!are_initial_metadata_flags_valid(op->flags, call->is_client)) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->sent_initial_metadata) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        /* process compression level */
        memset(&call->compression_md, 0, sizeof(call->compression_md));
        size_t additional_metadata_count = 0;
        grpc_compression_level effective_compression_level =
            GRPC_COMPRESS_LEVEL_NONE;
        bool level_set = false;
        if (op->data.send_initial_metadata.maybe_compression_level.is_set) {
          effective_compression_level =
              op->data.send_initial_metadata.maybe_compression_level.level;
          level_set = true;
        } else {
          const grpc_compression_options copts =
              grpc_channel_compression_options(call->channel);
          if (copts.default_level.is_set) {
            level_set = true;
            effective_compression_level = copts.default_level.level;
          }
        }
        if (level_set && !call->is_client) {
          const grpc_compression_algorithm calgo =
              compression_algorithm_for_level_locked(
                  call, effective_compression_level);
          /* the following will be picked up by the compress filter and used
           * as the call's compression algorithm. */
          call->compression_md.key = GRPC_MDSTR_GRPC_INTERNAL_ENCODING_REQUEST;
          call->compression_md.value = grpc_compression_algorithm_slice(calgo);
          additional_metadata_count++;
        }

        if (op->data.send_initial_metadata.count + additional_metadata_count >
            INT_MAX) {
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        stream_op->send_initial_metadata = true;
        call->sent_initial_metadata = true;
        if (!prepare_application_metadata(
                exec_ctx, call, (int)op->data.send_initial_metadata.count,
                op->data.send_initial_metadata.metadata, 0, call->is_client,
                &call->compression_md, (int)additional_metadata_count)) {
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        /* TODO(ctiller): just make these the same variable? */
        if (call->is_client) {
          call->metadata_batch[0][0].deadline = call->send_deadline;
        }
        stream_op_payload->send_initial_metadata.send_initial_metadata =
            &call->metadata_batch[0 /* is_receiving */][0 /* is_trailing */];
        stream_op_payload->send_initial_metadata.send_initial_metadata_flags =
            op->flags;
        if (call->is_client) {
          stream_op_payload->send_initial_metadata.peer_string =
              &call->peer_string;
        }
        break;
      case GRPC_OP_SEND_MESSAGE:
        if (!are_write_flags_valid(op->flags)) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (op->data.send_message.send_message == NULL) {
          error = GRPC_CALL_ERROR_INVALID_MESSAGE;
          goto done_with_error;
        }
        if (call->sending_message) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        stream_op->send_message = true;
        call->sending_message = true;
        grpc_slice_buffer_stream_init(
            &call->sending_stream,
            &op->data.send_message.send_message->data.raw.slice_buffer,
            op->flags);
        /* If the outgoing buffer is already compressed, mark it as so in the
           flags. These will be picked up by the compression filter and further
           (wasteful) attempts at compression skipped. */
        if (op->data.send_message.send_message->data.raw.compression >
            GRPC_COMPRESS_NONE) {
          call->sending_stream.base.flags |= GRPC_WRITE_INTERNAL_COMPRESS;
        }
        stream_op_payload->send_message.send_message =
            &call->sending_stream.base;
        break;
      case GRPC_OP_SEND_CLOSE_FROM_CLIENT:
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (!call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_SERVER;
          goto done_with_error;
        }
        if (call->sent_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        stream_op->send_trailing_metadata = true;
        call->sent_final_op = true;
        stream_op_payload->send_trailing_metadata.send_trailing_metadata =
            &call->metadata_batch[0 /* is_receiving */][1 /* is_trailing */];
        break;
      case GRPC_OP_SEND_STATUS_FROM_SERVER:
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_CLIENT;
          goto done_with_error;
        }
        if (call->sent_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        if (op->data.send_status_from_server.trailing_metadata_count >
            INT_MAX) {
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        stream_op->send_trailing_metadata = true;
        call->sent_final_op = true;
        GPR_ASSERT(call->send_extra_metadata_count == 0);
        call->send_extra_metadata_count = 1;
        call->send_extra_metadata[0].md = grpc_channel_get_reffed_status_elem(
            exec_ctx, call->channel, op->data.send_status_from_server.status);
        {
          grpc_error *override_error = GRPC_ERROR_NONE;
          if (op->data.send_status_from_server.status != GRPC_STATUS_OK) {
            override_error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                "Error from server send status");
          }
          if (op->data.send_status_from_server.status_details != NULL) {
            call->send_extra_metadata[1].md = grpc_mdelem_from_slices(
                exec_ctx, GRPC_MDSTR_GRPC_MESSAGE,
                grpc_slice_ref_internal(
                    *op->data.send_status_from_server.status_details));
            call->send_extra_metadata_count++;
            char *msg = grpc_slice_to_c_string(
                GRPC_MDVALUE(call->send_extra_metadata[1].md));
            override_error =
                grpc_error_set_str(override_error, GRPC_ERROR_STR_GRPC_MESSAGE,
                                   grpc_slice_from_copied_string(msg));
            gpr_free(msg);
          }
          set_status_from_error(exec_ctx, call, STATUS_FROM_API_OVERRIDE,
                                override_error);
        }
        if (!prepare_application_metadata(
                exec_ctx, call,
                (int)op->data.send_status_from_server.trailing_metadata_count,
                op->data.send_status_from_server.trailing_metadata, 1, 1, NULL,
                0)) {
          for (int n = 0; n < call->send_extra_metadata_count; n++) {
            GRPC_MDELEM_UNREF(exec_ctx, call->send_extra_metadata[n].md);
          }
          call->send_extra_metadata_count = 0;
          error = GRPC_CALL_ERROR_INVALID_METADATA;
          goto done_with_error;
        }
        stream_op_payload->send_trailing_metadata.send_trailing_metadata =
            &call->metadata_batch[0 /* is_receiving */][1 /* is_trailing */];
        break;
      case GRPC_OP_RECV_INITIAL_METADATA:
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->received_initial_metadata) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->received_initial_metadata = true;
        call->buffered_metadata[0] =
            op->data.recv_initial_metadata.recv_initial_metadata;
        GRPC_CLOSURE_INIT(&call->receiving_initial_metadata_ready,
                          receiving_initial_metadata_ready, bctl,
                          grpc_schedule_on_exec_ctx);
        stream_op->recv_initial_metadata = true;
        stream_op_payload->recv_initial_metadata.recv_initial_metadata =
            &call->metadata_batch[1 /* is_receiving */][0 /* is_trailing */];
        stream_op_payload->recv_initial_metadata.recv_initial_metadata_ready =
            &call->receiving_initial_metadata_ready;
        if (!call->is_client) {
          stream_op_payload->recv_initial_metadata.peer_string =
              &call->peer_string;
        }
        num_completion_callbacks_needed++;
        break;
      case GRPC_OP_RECV_MESSAGE:
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->receiving_message) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->receiving_message = true;
        stream_op->recv_message = true;
        call->receiving_buffer = op->data.recv_message.recv_message;
        stream_op_payload->recv_message.recv_message = &call->receiving_stream;
        GRPC_CLOSURE_INIT(&call->receiving_stream_ready,
                          receiving_stream_ready_in_call_combiner, bctl,
                          grpc_schedule_on_exec_ctx);
        stream_op_payload->recv_message.recv_message_ready =
            &call->receiving_stream_ready;
        num_completion_callbacks_needed++;
        break;
      case GRPC_OP_RECV_STATUS_ON_CLIENT:
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (!call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_SERVER;
          goto done_with_error;
        }
        if (call->requested_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->requested_final_op = true;
        call->buffered_metadata[1] =
            op->data.recv_status_on_client.trailing_metadata;
        call->final_op.client.status = op->data.recv_status_on_client.status;
        call->final_op.client.status_details =
            op->data.recv_status_on_client.status_details;
        stream_op->recv_trailing_metadata = true;
        stream_op->collect_stats = true;
        stream_op_payload->recv_trailing_metadata.recv_trailing_metadata =
            &call->metadata_batch[1 /* is_receiving */][1 /* is_trailing */];
        stream_op_payload->collect_stats.collect_stats =
            &call->final_info.stats.transport_stream_stats;
        break;
      case GRPC_OP_RECV_CLOSE_ON_SERVER:
        /* Flag validation: currently allow no flags */
        if (op->flags != 0) {
          error = GRPC_CALL_ERROR_INVALID_FLAGS;
          goto done_with_error;
        }
        if (call->is_client) {
          error = GRPC_CALL_ERROR_NOT_ON_CLIENT;
          goto done_with_error;
        }
        if (call->requested_final_op) {
          error = GRPC_CALL_ERROR_TOO_MANY_OPERATIONS;
          goto done_with_error;
        }
        call->requested_final_op = true;
        call->final_op.server.cancelled =
            op->data.recv_close_on_server.cancelled;
        stream_op->recv_trailing_metadata = true;
        stream_op->collect_stats = true;
        stream_op_payload->recv_trailing_metadata.recv_trailing_metadata =
            &call->metadata_batch[1 /* is_receiving */][1 /* is_trailing */];
        stream_op_payload->collect_stats.collect_stats =
            &call->final_info.stats.transport_stream_stats;
        break;
    }
  }

  GRPC_CALL_INTERNAL_REF(call, "completion");
  if (!is_notify_tag_closure) {
    GPR_ASSERT(grpc_cq_begin_op(call->cq, notify_tag));
  }
  gpr_ref_init(&bctl->steps_to_complete, num_completion_callbacks_needed);

  GRPC_CLOSURE_INIT(&bctl->finish_batch, finish_batch, bctl,
                    grpc_schedule_on_exec_ctx);
  stream_op->on_complete = &bctl->finish_batch;
  gpr_atm_rel_store(&call->any_ops_sent_atm, 1);

  execute_batch(exec_ctx, call, stream_op, &bctl->start_batch);

done:
  GPR_TIMER_END("grpc_call_start_batch", 0);
  return error;

done_with_error:
  /* reverse any mutations that occured */
  if (stream_op->send_initial_metadata) {
    call->sent_initial_metadata = false;
    grpc_metadata_batch_clear(exec_ctx, &call->metadata_batch[0][0]);
  }
  if (stream_op->send_message) {
    call->sending_message = false;
    grpc_byte_stream_destroy(exec_ctx, &call->sending_stream.base);
  }
  if (stream_op->send_trailing_metadata) {
    call->sent_final_op = false;
    grpc_metadata_batch_clear(exec_ctx, &call->metadata_batch[0][1]);
  }
  if (stream_op->recv_initial_metadata) {
    call->received_initial_metadata = false;
  }
  if (stream_op->recv_message) {
    call->receiving_message = false;
  }
  if (stream_op->recv_trailing_metadata) {
    call->requested_final_op = false;
  }
  goto done;
}

grpc_call_error grpc_call_start_batch(grpc_call *call, const grpc_op *ops,
                                      size_t nops, void *tag, void *reserved) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_call_error err;

  GRPC_API_TRACE(
      "grpc_call_start_batch(call=%p, ops=%p, nops=%lu, tag=%p, "
      "reserved=%p)",
      5, (call, ops, (unsigned long)nops, tag, reserved));

  if (reserved != NULL) {
    err = GRPC_CALL_ERROR;
  } else {
    err = call_start_batch(&exec_ctx, call, ops, nops, tag, 0);
  }

  grpc_exec_ctx_finish(&exec_ctx);
  return err;
}

grpc_call_error grpc_call_start_batch_and_execute(grpc_exec_ctx *exec_ctx,
                                                  grpc_call *call,
                                                  const grpc_op *ops,
                                                  size_t nops,
                                                  grpc_closure *closure) {
  return call_start_batch(exec_ctx, call, ops, nops, closure, 1);
}

void grpc_call_context_set(grpc_call *call, grpc_context_index elem,
                           void *value, void (*destroy)(void *value)) {
  if (call->context[elem].destroy) {
    call->context[elem].destroy(call->context[elem].value);
  }
  call->context[elem].value = value;
  call->context[elem].destroy = destroy;
}

void *grpc_call_context_get(grpc_call *call, grpc_context_index elem) {
  return call->context[elem].value;
}

uint8_t grpc_call_is_client(grpc_call *call) { return call->is_client; }

grpc_compression_algorithm grpc_call_compression_for_level(
    grpc_call *call, grpc_compression_level level) {
  grpc_compression_algorithm algo =
      compression_algorithm_for_level_locked(call, level);
  return algo;
}

const char *grpc_call_error_to_string(grpc_call_error error) {
  switch (error) {
    case GRPC_CALL_ERROR:
      return "GRPC_CALL_ERROR";
    case GRPC_CALL_ERROR_ALREADY_ACCEPTED:
      return "GRPC_CALL_ERROR_ALREADY_ACCEPTED";
    case GRPC_CALL_ERROR_ALREADY_FINISHED:
      return "GRPC_CALL_ERROR_ALREADY_FINISHED";
    case GRPC_CALL_ERROR_ALREADY_INVOKED:
      return "GRPC_CALL_ERROR_ALREADY_INVOKED";
    case GRPC_CALL_ERROR_BATCH_TOO_BIG:
      return "GRPC_CALL_ERROR_BATCH_TOO_BIG";
    case GRPC_CALL_ERROR_INVALID_FLAGS:
      return "GRPC_CALL_ERROR_INVALID_FLAGS";
    case GRPC_CALL_ERROR_INVALID_MESSAGE:
      return "GRPC_CALL_ERROR_INVALID_MESSAGE";
    case GRPC_CALL_ERROR_INVALID_METADATA:
      return "GRPC_CALL_ERROR_INVALID_METADATA";
    case GRPC_CALL_ERROR_NOT_INVOKED:
      return "GRPC_CALL_ERROR_NOT_INVOKED";
    case GRPC_CALL_ERROR_NOT_ON_CLIENT:
      return "GRPC_CALL_ERROR_NOT_ON_CLIENT";
    case GRPC_CALL_ERROR_NOT_ON_SERVER:
      return "GRPC_CALL_ERROR_NOT_ON_SERVER";
    case GRPC_CALL_ERROR_NOT_SERVER_COMPLETION_QUEUE:
      return "GRPC_CALL_ERROR_NOT_SERVER_COMPLETION_QUEUE";
    case GRPC_CALL_ERROR_PAYLOAD_TYPE_MISMATCH:
      return "GRPC_CALL_ERROR_PAYLOAD_TYPE_MISMATCH";
    case GRPC_CALL_ERROR_TOO_MANY_OPERATIONS:
      return "GRPC_CALL_ERROR_TOO_MANY_OPERATIONS";
    case GRPC_CALL_ERROR_COMPLETION_QUEUE_SHUTDOWN:
      return "GRPC_CALL_ERROR_COMPLETION_QUEUE_SHUTDOWN";
    case GRPC_CALL_OK:
      return "GRPC_CALL_OK";
  }
  GPR_UNREACHABLE_CODE(return "GRPC_CALL_ERROR_UNKNOW");
}
