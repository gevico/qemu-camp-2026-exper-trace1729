// arg_dma is a struct tnat contains the decoded fields of the instruction

static bool trans_dma(DisasContext *ctx, arg_dma *a) {
    
    // gen_helper_dma is will expand the helper function into TCG code, and the arguments are passed as TCG constants
    gen_helper_dma(tcg_env, tcg_constant_tl(a->rd), tcg_constant_tl(a->rs1), tcg_constant_tl(a->rs2));
    return true;
}

static bool trans_sort(DisasContext *ctx, arg_r *a) {
    gen_helper_sort(tcg_env, tcg_constant_tl(a->rd), tcg_constant_tl(a->rs1), tcg_constant_tl(a->rs2));    
    return true;
}

static bool trans_crush(DisasContext *ctx, arg_r *a) {
    gen_helper_crush(tcg_env, tcg_constant_tl(a->rd), tcg_constant_tl(a->rs1), tcg_constant_tl(a->rs2));    
    return true;
}

static bool trans_expand(DisasContext *ctx, arg_r *a) {
    gen_helper_expand(tcg_env, tcg_constant_tl(a->rd), tcg_constant_tl(a->rs1), tcg_constant_tl(a->rs2));    
    return true;
}

static bool trans_vdot(DisasContext *ctx, arg_r *a) {
    gen_helper_vdot(tcg_env, tcg_constant_tl(a->rd), tcg_constant_tl(a->rs1), tcg_constant_tl(a->rs2));    
    return true;
}
