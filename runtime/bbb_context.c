////////////////////////////////
//~ Context

static __thread bbb_Context* bbb_tls_ctx = NULL;

void
bbb_ctx_init(bbb_Context* ctx, u64 scratch_size) {
  bbb_xassert(ctx && "Context is NULL");
  bbb_xassert(scratch_size > 0 && "Scratch size is zero");
  bbb_MemoryZeroStruct(ctx);

  ctx->scratch = bbb_arena_create_vm(scratch_size);
  ctx->perm    = bbb_arena_create_vm(bbb_MB(512));
  bbb_tls_ctx  = ctx;
}

void
bbb_ctx_free(void) {
  if (!bbb_tls_ctx) return;
  bbb_arena_destroy(&bbb_tls_ctx->scratch);
  bbb_arena_destroy(&bbb_tls_ctx->perm);
  bbb_tls_ctx = NULL;
}

bbb_Arena*
bbb_ctx_scratch(void) {
  return bbb_tls_ctx ? &bbb_tls_ctx->scratch : NULL;
}

bbb_Arena*
bbb_ctx_perm(void) {
  return bbb_tls_ctx ? &bbb_tls_ctx->perm : NULL;
}
