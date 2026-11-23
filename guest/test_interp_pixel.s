// Physical U is 0.75 before the split. Replace it with the interpolated
// 0.25 in even columns (left half), or even rows (right half).
s_wqm_b64 exec, exec
s_mov_b32 m0, s24
s_mov_b32 s9, 0
s_mov_b32 s11, 0
s_load_dwordx8 s[16:23], s[8:9], 0
s_load_dwordx4 s[24:27], s[10:11], 0
v_cvt_u32_f32 v14, v2
v_cvt_u32_f32 v5, v3
v_cmp_lt_u32 vcc, v14, 16
v_cndmask_b32 v15, v5, v14, vcc
v_and_b32 v15, 1, v15
v_mov_b32 v6, 0x3f400000
v_mov_b32 v7, 0.25
v_mov_b32 v8, -1.0
v_mov_b32 v9, -2.0
v_mov_b32 v12, 0
v_mov_b32 v13, 1.0
v_cmp_eq_u32 vcc, 0, v15
s_and_saveexec_b64 s[4:5], vcc
s_cbranch_execz .Lrestore
v_interp_p1_f32 v6, v0, attr0.x
v_interp_p1_f32 v7, v0, attr0.y
v_interp_p2_f32 v6, v1, attr0.x
v_interp_p2_f32 v7, v1, attr0.y
s_waitcnt lgkmcnt(0)
image_sample_b v8, [v12, v6, v7], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D
image_sample_b v9, [v13, v6, v7], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D
.Lrestore:
s_mov_b64 exec, s[4:5]
// v4 starts as the symbolic rasterizer front-face input. It has no
// portable raw bits in quads left out by WQM. Define it in whole selected
// quads, then sample under a strict subset of those lanes.
v_mov_b32 v17, -3.0
v_and_b32 v16, 3, v14
v_cmp_eq_u32 vcc, 0, v16
s_wqm_b64 s[28:29], vcc
s_and_saveexec_b64 s[30:31], s[28:29]
s_cbranch_execz .Lquad_done
v_interp_p1_f32 v4, v0, attr0.x
v_interp_p2_f32 v4, v1, attr0.x
v_mul_f32 v16, 0.25, v2
v_add_f32 v4, v4, v16
v_and_b32 v16, 1, v14
v_cmp_eq_u32 vcc, 1, v16
s_and_saveexec_b64 s[28:29], vcc
s_cbranch_execz .Lnarrow_done
image_sample v17, [v4, v7], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D
.Lnarrow_done:
s_mov_b64 exec, s[28:29]
.Lquad_done:
s_mov_b64 exec, s[30:31]
s_waitcnt vmcnt(0)
exp mrt0 v8, v9, v6, v17 done vm
s_endpgm
