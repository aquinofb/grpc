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

#include <stdio.h>
#include <string.h>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/cmdline.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>

#include "src/core/lib/security/credentials/jwt/jwt_verifier.h"

typedef struct {
  grpc_pollset *pollset;
  gpr_mu *mu;
  int is_done;
  int success;
} synchronizer;

static void print_usage_and_exit(gpr_cmdline *cl, const char *argv0) {
  char *usage = gpr_cmdline_usage_string(cl, argv0);
  fprintf(stderr, "%s", usage);
  gpr_free(usage);
  gpr_cmdline_destroy(cl);
  exit(1);
}

static void on_jwt_verification_done(grpc_exec_ctx *exec_ctx, void *user_data,
                                     grpc_jwt_verifier_status status,
                                     grpc_jwt_claims *claims) {
  synchronizer *sync = user_data;

  sync->success = (status == GRPC_JWT_VERIFIER_OK);
  if (sync->success) {
    char *claims_str;
    GPR_ASSERT(claims != NULL);
    claims_str =
        grpc_json_dump_to_string((grpc_json *)grpc_jwt_claims_json(claims), 2);
    printf("Claims: \n\n%s\n", claims_str);
    gpr_free(claims_str);
    grpc_jwt_claims_destroy(exec_ctx, claims);
  } else {
    GPR_ASSERT(claims == NULL);
    fprintf(stderr, "Verification failed with error %s\n",
            grpc_jwt_verifier_status_to_string(status));
  }

  gpr_mu_lock(sync->mu);
  sync->is_done = 1;
  GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(sync->pollset, NULL));
  gpr_mu_unlock(sync->mu);
}

int main(int argc, char **argv) {
  synchronizer sync;
  grpc_jwt_verifier *verifier;
  gpr_cmdline *cl;
  char *jwt = NULL;
  char *aud = NULL;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;

  grpc_init();
  cl = gpr_cmdline_create("JWT verifier tool");
  gpr_cmdline_add_string(cl, "jwt", "JSON web token to verify", &jwt);
  gpr_cmdline_add_string(cl, "aud", "Audience for the JWT", &aud);
  gpr_cmdline_parse(cl, argc, argv);
  if (jwt == NULL || aud == NULL) {
    print_usage_and_exit(cl, argv[0]);
  }

  verifier = grpc_jwt_verifier_create(NULL, 0);

  grpc_init();

  sync.pollset = gpr_malloc(grpc_pollset_size());
  grpc_pollset_init(sync.pollset, &sync.mu);
  sync.is_done = 0;

  grpc_jwt_verifier_verify(&exec_ctx, verifier, sync.pollset, jwt, aud,
                           on_jwt_verification_done, &sync);

  gpr_mu_lock(sync.mu);
  while (!sync.is_done) {
    grpc_pollset_worker *worker = NULL;
    if (!GRPC_LOG_IF_ERROR(
            "pollset_work",
            grpc_pollset_work(&exec_ctx, sync.pollset, &worker,
                              gpr_now(GPR_CLOCK_MONOTONIC),
                              gpr_inf_future(GPR_CLOCK_MONOTONIC))))
      sync.is_done = true;
    gpr_mu_unlock(sync.mu);
    grpc_exec_ctx_flush(&exec_ctx);
    gpr_mu_lock(sync.mu);
  }
  gpr_mu_unlock(sync.mu);

  gpr_free(sync.pollset);

  grpc_jwt_verifier_destroy(&exec_ctx, verifier);
  grpc_exec_ctx_finish(&exec_ctx);
  gpr_cmdline_destroy(cl);
  grpc_shutdown();
  return !sync.success;
}
