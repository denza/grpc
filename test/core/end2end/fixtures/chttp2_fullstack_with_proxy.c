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

#include <string.h>

#include "src/core/channel/client_channel.h"
#include "src/core/channel/connected_channel.h"
#include "src/core/channel/http_server_filter.h"
#include "src/core/surface/channel.h"
#include "src/core/surface/server.h"
#include "src/core/transport/chttp2_transport.h"
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>
#include <grpc/support/thd.h>
#include <grpc/support/useful.h>
#include "test/core/end2end/fixtures/proxy.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

typedef struct fullstack_fixture_data {
  grpc_end2end_proxy *proxy;
} fullstack_fixture_data;

static grpc_server *create_proxy_server(const char *port) {
  grpc_server *s = grpc_server_create(NULL);
  GPR_ASSERT(grpc_server_add_insecure_http2_port(s, port));
  return s;
}

static grpc_channel *create_proxy_client(const char *target) {
  return grpc_insecure_channel_create(target, NULL);
}

static const grpc_end2end_proxy_def proxy_def = {create_proxy_server,
                                                 create_proxy_client};

static grpc_end2end_test_fixture chttp2_create_fixture_fullstack(
    grpc_channel_args *client_args, grpc_channel_args *server_args) {
  grpc_end2end_test_fixture f;
  fullstack_fixture_data *ffd = gpr_malloc(sizeof(fullstack_fixture_data));
  memset(&f, 0, sizeof(f));

  ffd->proxy = grpc_end2end_proxy_create(&proxy_def);

  f.fixture_data = ffd;
  f.cq = grpc_completion_queue_create();

  return f;
}

void chttp2_init_client_fullstack(grpc_end2end_test_fixture *f,
                                  grpc_channel_args *client_args) {
  fullstack_fixture_data *ffd = f->fixture_data;
  f->client = grpc_insecure_channel_create(
      grpc_end2end_proxy_get_client_target(ffd->proxy), client_args);
  GPR_ASSERT(f->client);
}

void chttp2_init_server_fullstack(grpc_end2end_test_fixture *f,
                                  grpc_channel_args *server_args) {
  fullstack_fixture_data *ffd = f->fixture_data;
  if (f->server) {
    grpc_server_destroy(f->server);
  }
  f->server = grpc_server_create(server_args);
  grpc_server_register_completion_queue(f->server, f->cq);
  GPR_ASSERT(grpc_server_add_insecure_http2_port(
      f->server, grpc_end2end_proxy_get_server_port(ffd->proxy)));
  grpc_server_start(f->server);
}

void chttp2_tear_down_fullstack(grpc_end2end_test_fixture *f) {
  fullstack_fixture_data *ffd = f->fixture_data;
  grpc_end2end_proxy_destroy(ffd->proxy);
  gpr_free(ffd);
}

/* All test configurations */
static grpc_end2end_test_config configs[] = {
    {"chttp2/fullstack+proxy", FEATURE_MASK_SUPPORTS_DELAYED_CONNECTION,
     chttp2_create_fixture_fullstack, chttp2_init_client_fullstack,
     chttp2_init_server_fullstack, chttp2_tear_down_fullstack},
};

int main(int argc, char **argv) {
  size_t i;

  grpc_test_init(argc, argv);
  grpc_init();

  for (i = 0; i < sizeof(configs) / sizeof(*configs); i++) {
    grpc_end2end_tests(configs[i]);
  }

  grpc_shutdown();

  return 0;
}
