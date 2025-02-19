/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "test/core/end2end/end2end_tests.h"

#include <stdio.h>
#include <string.h>

#include <grpc/grpc_security.h>
#include <grpc/byte_buffer.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>
#include "test/core/end2end/cq_verifier.h"
#include "src/core/security/credentials.h"
#include "src/core/support/string.h"

static const char iam_token[] = "token";
static const char iam_selector[] = "selector";
static const char overridden_iam_token[] = "overridden_token";
static const char overridden_iam_selector[] = "overridden_selector";

typedef enum { NONE, OVERRIDE, DESTROY } override_mode;

enum { TIMEOUT = 200000 };

static void *tag(gpr_intptr t) { return (void *)t; }

static grpc_end2end_test_fixture begin_test(grpc_end2end_test_config config,
                                            const char *test_name,
                                            grpc_channel_args *client_args,
                                            grpc_channel_args *server_args) {
  grpc_end2end_test_fixture f;
  gpr_log(GPR_INFO, "%s/%s", test_name, config.name);
  f = config.create_fixture(client_args, server_args);
  config.init_client(&f, client_args);
  config.init_server(&f, server_args);
  return f;
}

static gpr_timespec n_seconds_time(int n) {
  return GRPC_TIMEOUT_SECONDS_TO_DEADLINE(n);
}

static gpr_timespec five_seconds_time(void) { return n_seconds_time(5); }

static void drain_cq(grpc_completion_queue *cq) {
  grpc_event ev;
  do {
    ev = grpc_completion_queue_next(cq, five_seconds_time());
  } while (ev.type != GRPC_QUEUE_SHUTDOWN);
}

static void shutdown_server(grpc_end2end_test_fixture *f) {
  if (!f->server) return;
  grpc_server_shutdown_and_notify(f->server, f->cq, tag(1000));
  GPR_ASSERT(grpc_completion_queue_pluck(f->cq, tag(1000),
                                         GRPC_TIMEOUT_SECONDS_TO_DEADLINE(5))
                 .type == GRPC_OP_COMPLETE);
  grpc_server_destroy(f->server);
  f->server = NULL;
}

static void shutdown_client(grpc_end2end_test_fixture *f) {
  if (!f->client) return;
  grpc_channel_destroy(f->client);
  f->client = NULL;
}

static void end_test(grpc_end2end_test_fixture *f) {
  shutdown_server(f);
  shutdown_client(f);

  grpc_completion_queue_shutdown(f->cq);
  drain_cq(f->cq);
  grpc_completion_queue_destroy(f->cq);
}

static void print_auth_context(int is_client, const grpc_auth_context *ctx) {
  const grpc_auth_property *p;
  grpc_auth_property_iterator it;
  gpr_log(GPR_INFO, "%s peer:", is_client ? "client" : "server");
  gpr_log(GPR_INFO, "\tauthenticated: %s",
          grpc_auth_context_peer_is_authenticated(ctx) ? "YES" : "NO");
  it = grpc_auth_context_peer_identity(ctx);
  while ((p = grpc_auth_property_iterator_next(&it)) != NULL) {
    gpr_log(GPR_INFO, "\t\t%s: %s", p->name, p->value);
  }
  gpr_log(GPR_INFO, "\tall properties:");
  it = grpc_auth_context_property_iterator(ctx);
  while ((p = grpc_auth_property_iterator_next(&it)) != NULL) {
    gpr_log(GPR_INFO, "\t\t%s: %s", p->name, p->value);
  }
}

static void test_call_creds_failure(grpc_end2end_test_config config) {
  grpc_call *c;
  grpc_credentials *creds = NULL;
  grpc_end2end_test_fixture f =
      begin_test(config, "test_call_creds_failure", NULL, NULL);
  gpr_timespec deadline = five_seconds_time();
  c = grpc_channel_create_call(f.client, NULL, GRPC_PROPAGATE_DEFAULTS, f.cq,
                               "/foo", "foo.test.google.fr", deadline);
  GPR_ASSERT(c);

  /* Try with credentials unfit to be set on a call (channel creds). */
  creds = grpc_fake_transport_security_credentials_create();
  GPR_ASSERT(grpc_call_set_credentials(c, creds) != GRPC_CALL_OK);
  grpc_credentials_release(creds);

  grpc_call_destroy(c);

  end_test(&f);
  config.tear_down_data(&f);
}

static void request_response_with_payload_and_call_creds(
    const char *test_name, grpc_end2end_test_config config,
    override_mode mode) {
  grpc_call *c;
  grpc_call *s;
  gpr_slice request_payload_slice = gpr_slice_from_copied_string("hello world");
  gpr_slice response_payload_slice = gpr_slice_from_copied_string("hello you");
  grpc_byte_buffer *request_payload =
      grpc_raw_byte_buffer_create(&request_payload_slice, 1);
  grpc_byte_buffer *response_payload =
      grpc_raw_byte_buffer_create(&response_payload_slice, 1);
  gpr_timespec deadline = five_seconds_time();

  grpc_end2end_test_fixture f = begin_test(config, test_name, NULL, NULL);
  cq_verifier *cqv = cq_verifier_create(f.cq);
  grpc_op ops[6];
  grpc_op *op;
  grpc_metadata_array initial_metadata_recv;
  grpc_metadata_array trailing_metadata_recv;
  grpc_metadata_array request_metadata_recv;
  grpc_byte_buffer *request_payload_recv = NULL;
  grpc_byte_buffer *response_payload_recv = NULL;
  grpc_call_details call_details;
  grpc_status_code status;
  char *details = NULL;
  size_t details_capacity = 0;
  int was_cancelled = 2;
  grpc_credentials *creds = NULL;
  grpc_auth_context *s_auth_context = NULL;

  c = grpc_channel_create_call(f.client, NULL, GRPC_PROPAGATE_DEFAULTS, f.cq,
                               "/foo", "foo.test.google.fr", deadline);
  GPR_ASSERT(c);
  creds = grpc_iam_credentials_create(iam_token, iam_selector);
  GPR_ASSERT(creds != NULL);
  GPR_ASSERT(grpc_call_set_credentials(c, creds) == GRPC_CALL_OK);
  switch (mode) {
    case NONE:
      break;
    case OVERRIDE:
      grpc_credentials_release(creds);
      creds = grpc_iam_credentials_create(overridden_iam_token,
                                          overridden_iam_selector);
      GPR_ASSERT(creds != NULL);
      GPR_ASSERT(grpc_call_set_credentials(c, creds) == GRPC_CALL_OK);
      break;
    case DESTROY:
      GPR_ASSERT(grpc_call_set_credentials(c, NULL) == GRPC_CALL_OK);
      break;
  }
  grpc_credentials_release(creds);

  grpc_metadata_array_init(&initial_metadata_recv);
  grpc_metadata_array_init(&trailing_metadata_recv);
  grpc_metadata_array_init(&request_metadata_recv);
  grpc_call_details_init(&call_details);

  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message = request_payload;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata = &initial_metadata_recv;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message = &response_payload_recv;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op->data.recv_status_on_client.status_details_capacity = &details_capacity;
  op->flags = 0;
  op++;
  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(c, ops, op - ops, tag(1)));

  GPR_ASSERT(GRPC_CALL_OK == grpc_server_request_call(
                                 f.server, &s, &call_details,
                                 &request_metadata_recv, f.cq, f.cq, tag(101)));
  cq_expect_completion(cqv, tag(101), 1);
  cq_verify(cqv);
  s_auth_context = grpc_call_auth_context(s);
  GPR_ASSERT(s_auth_context != NULL);
  print_auth_context(0, s_auth_context);
  grpc_auth_context_release(s_auth_context);

  /* Cannot set creds on the server call object. */
  GPR_ASSERT(grpc_call_set_credentials(s, NULL) != GRPC_CALL_OK);

  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message = &request_payload_recv;
  op->flags = 0;
  op++;
  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(s, ops, op - ops, tag(102)));

  cq_expect_completion(cqv, tag(102), 1);
  cq_verify(cqv);

  op = ops;
  op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  op->data.recv_close_on_server.cancelled = &was_cancelled;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message = response_payload;
  op->flags = 0;
  op++;
  op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  op->data.send_status_from_server.trailing_metadata_count = 0;
  op->data.send_status_from_server.status = GRPC_STATUS_OK;
  op->data.send_status_from_server.status_details = "xyz";
  op->flags = 0;
  op++;
  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(s, ops, op - ops, tag(103)));

  cq_expect_completion(cqv, tag(103), 1);
  cq_expect_completion(cqv, tag(1), 1);
  cq_verify(cqv);

  GPR_ASSERT(status == GRPC_STATUS_OK);
  GPR_ASSERT(0 == strcmp(details, "xyz"));
  GPR_ASSERT(0 == strcmp(call_details.method, "/foo"));
  GPR_ASSERT(0 == strcmp(call_details.host, "foo.test.google.fr"));
  GPR_ASSERT(was_cancelled == 0);
  GPR_ASSERT(byte_buffer_eq_string(request_payload_recv, "hello world"));
  GPR_ASSERT(byte_buffer_eq_string(response_payload_recv, "hello you"));

  switch (mode) {
    case NONE:
      GPR_ASSERT(contains_metadata(&request_metadata_recv,
                                   GRPC_IAM_AUTHORIZATION_TOKEN_METADATA_KEY,
                                   iam_token));
      GPR_ASSERT(contains_metadata(&request_metadata_recv,
                                   GRPC_IAM_AUTHORITY_SELECTOR_METADATA_KEY,
                                   iam_selector));
      break;
    case OVERRIDE:
      GPR_ASSERT(contains_metadata(&request_metadata_recv,
                                   GRPC_IAM_AUTHORIZATION_TOKEN_METADATA_KEY,
                                   overridden_iam_token));
      GPR_ASSERT(contains_metadata(&request_metadata_recv,
                                   GRPC_IAM_AUTHORITY_SELECTOR_METADATA_KEY,
                                   overridden_iam_selector));
      break;
    case DESTROY:
      GPR_ASSERT(!contains_metadata(&request_metadata_recv,
                                    GRPC_IAM_AUTHORIZATION_TOKEN_METADATA_KEY,
                                    iam_token));
      GPR_ASSERT(!contains_metadata(&request_metadata_recv,
                                    GRPC_IAM_AUTHORITY_SELECTOR_METADATA_KEY,
                                    iam_selector));
      GPR_ASSERT(!contains_metadata(&request_metadata_recv,
                                    GRPC_IAM_AUTHORIZATION_TOKEN_METADATA_KEY,
                                    overridden_iam_token));
      GPR_ASSERT(!contains_metadata(&request_metadata_recv,
                                    GRPC_IAM_AUTHORITY_SELECTOR_METADATA_KEY,
                                    overridden_iam_selector));
      break;
  }

  gpr_free(details);
  grpc_metadata_array_destroy(&initial_metadata_recv);
  grpc_metadata_array_destroy(&trailing_metadata_recv);
  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);

  grpc_call_destroy(c);
  grpc_call_destroy(s);

  cq_verifier_destroy(cqv);

  grpc_byte_buffer_destroy(request_payload);
  grpc_byte_buffer_destroy(response_payload);
  grpc_byte_buffer_destroy(request_payload_recv);
  grpc_byte_buffer_destroy(response_payload_recv);

  end_test(&f);
  config.tear_down_data(&f);
}

void test_request_response_with_payload_and_call_creds(
    grpc_end2end_test_config config) {
  request_response_with_payload_and_call_creds(
      "test_request_response_with_payload_and_call_creds", config, NONE);
}

void test_request_response_with_payload_and_overridden_call_creds(
    grpc_end2end_test_config config) {
  request_response_with_payload_and_call_creds(
      "test_request_response_with_payload_and_overridden_call_creds", config,
      OVERRIDE);
}

void test_request_response_with_payload_and_deleted_call_creds(
    grpc_end2end_test_config config) {
  request_response_with_payload_and_call_creds(
      "test_request_response_with_payload_and_deleted_call_creds", config,
      DESTROY);
}

void grpc_end2end_tests(grpc_end2end_test_config config) {
  if (config.feature_mask & FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS) {
    test_call_creds_failure(config);
    test_request_response_with_payload_and_call_creds(config);
    test_request_response_with_payload_and_overridden_call_creds(config);
    test_request_response_with_payload_and_deleted_call_creds(config);
  }
}
