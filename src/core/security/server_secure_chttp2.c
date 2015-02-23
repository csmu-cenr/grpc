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

#include <grpc/grpc.h>

#include <string.h>

#include "src/core/channel/http_filter.h"
#include "src/core/channel/http_server_filter.h"
#include "src/core/iomgr/resolve_address.h"
#include "src/core/iomgr/tcp_server.h"
#include "src/core/security/security_context.h"
#include "src/core/security/secure_transport_setup.h"
#include "src/core/surface/server.h"
#include "src/core/transport/chttp2_transport.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/useful.h>

static grpc_transport_setup_result setup_transport(void *server,
                                                   grpc_transport *transport,
                                                   grpc_mdctx *mdctx) {
  static grpc_channel_filter const *extra_filters[] = {&grpc_http_server_filter,
                                                       &grpc_http_filter};
  return grpc_server_setup_transport(server, transport, extra_filters,
                                     GPR_ARRAY_SIZE(extra_filters), mdctx);
}

static void on_secure_transport_setup_done(void *server,
                                           grpc_security_status status,
                                           grpc_endpoint *secure_endpoint) {
  if (status == GRPC_SECURITY_OK) {
    grpc_create_chttp2_transport(
        setup_transport, server, grpc_server_get_channel_args(server),
        secure_endpoint, NULL, 0, grpc_mdctx_create(), 0);
  } else {
    gpr_log(GPR_ERROR, "Secure transport failed with error %d", status);
  }
}

typedef struct {
  grpc_tcp_server *tcp;
  grpc_security_context *ctx;
  grpc_server *server;
} secured_port;

static void on_accept(void *spp, grpc_endpoint *tcp) {
  secured_port *sp = spp;
  grpc_setup_secure_transport(sp->ctx, tcp, on_secure_transport_setup_done, sp->server);
}

/* Server callback: start listening on our ports */
static void start(grpc_server *server, void *spp, grpc_pollset **pollsets,
                  size_t pollset_count) {
  secured_port *sp = spp;
  grpc_tcp_server_start(sp->tcp, pollsets, pollset_count, on_accept, sp);
}

/* Server callback: destroy the tcp listener (so we don't generate further
   callbacks) */
static void destroy(grpc_server *server, void *spp) {
  secured_port *sp = spp;
  grpc_tcp_server_destroy(sp->tcp);
  grpc_security_context_unref(sp->ctx);
  gpr_free(sp);
}

int grpc_server_add_secure_http2_port(grpc_server *server, const char *addr, grpc_server_credentials *creds) {
  grpc_resolved_addresses *resolved = NULL;
  grpc_tcp_server *tcp = NULL;
  size_t i;
  unsigned count = 0;
  int port_num = -1;
  int port_temp;
  grpc_security_status status = GRPC_SECURITY_ERROR;
  grpc_security_context *ctx = NULL;
  secured_port *sp = NULL;

  /* create security context */
  if (creds == NULL) goto error;

  if (!strcmp(creds->type, GRPC_CREDENTIALS_TYPE_SSL)) {
    status = grpc_ssl_server_security_context_create(
        grpc_ssl_server_credentials_get_config(creds), &ctx);
  } else if (!strcmp(creds->type,
                     GRPC_CREDENTIALS_TYPE_FAKE_TRANSPORT_SECURITY)) {
    ctx = grpc_fake_server_security_context_create();
    status = GRPC_SECURITY_OK;
  }

  if (status != GRPC_SECURITY_OK) {
    gpr_log(GPR_ERROR,
            "Unable to create secure server with credentials of type %s.",
            creds->type);
    goto error;
  }

  /* resolve address */
  resolved = grpc_blocking_resolve_address(addr, "https");
  if (!resolved) {
    goto error;
  }

  tcp = grpc_tcp_server_create();
  if (!tcp) {
    goto error;
  }

  for (i = 0; i < resolved->naddrs; i++) {
    port_temp = grpc_tcp_server_add_port(
        tcp, (struct sockaddr *)&resolved->addrs[i].addr,
        resolved->addrs[i].len);
    if (port_temp >= 0) {
      if (port_num == -1) {
        port_num = port_temp;
      } else {
        GPR_ASSERT(port_num == port_temp);
      }
      count++;
    }
  }
  if (count == 0) {
    gpr_log(GPR_ERROR, "No address added out of total %d resolved",
            resolved->naddrs);
    goto error;
  }
  if (count != resolved->naddrs) {
    gpr_log(GPR_ERROR, "Only %d addresses added out of total %d resolved",
            count, resolved->naddrs);
    /* if it's an error, don't we want to goto error; here ? */
  }
  grpc_resolved_addresses_destroy(resolved);

  sp = gpr_malloc(sizeof(secured_port));
  sp->tcp = tcp;
  sp->ctx = ctx;
  sp->server = server;

  /* Register with the server only upon success */
  grpc_server_add_listener(server, sp, start, destroy);

  return port_num;

/* Error path: cleanup and return */
error:
  if (ctx) {
    grpc_security_context_unref(ctx);
  }
  if (resolved) {
    grpc_resolved_addresses_destroy(resolved);
  }
  if (tcp) {
    grpc_tcp_server_destroy(tcp);
  }
  if (sp) {
    gpr_free(sp);
  }
  return 0;
}
