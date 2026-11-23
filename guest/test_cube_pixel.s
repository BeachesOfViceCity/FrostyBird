s_wqm_b64 exec, exec
s_mov_b32 s9, 0
s_mov_b32 s11, 0
s_load_dwordx8 s[16:23], s[8:9], 0
s_load_dwordx4 s[24:27], s[10:11], 0
v_cvt_u32_f32 v2, v0
v_cvt_u32_f32 v3, v1
v_lshrrev_b32 v4, 2, v2
v_min_u32 v4, 5, v4
v_lshrrev_b32 v5, 2, v3
v_and_b32 v5, 1, v5
v_lshl_add_u32 v14, v5, 3, v4
v_cvt_f32_u32 v14, v14
v_and_b32 v12, 3, v2
v_and_b32 v13, 3, v3
v_cvt_f32_u32 v12, v12
v_cvt_f32_u32 v13, v13
v_add_f32 v12, 0.5, v12
v_add_f32 v13, 0.5, v13
v_mul_f32 v12, 0x3d800000, v12
v_mul_f32 v13, 0x3d800000, v13
v_add_f32 v12, 0x3fa00000, v12
v_add_f32 v13, 0x3fa00000, v13
v_mov_b32 v11, 1.0
v_mov_b32 v16, 0x3e800000
v_mov_b32 v17, 0
s_waitcnt lgkmcnt(0)
image_sample_b v20, v[11:14], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D_ARRAY
image_sample_d v21, [v16, v17, v17, v16, v12, v13, v14], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D_ARRAY
v_mov_b32 v22, -1.0
v_and_b32 v6, 4, v2
v_cmp_eq_u32 vcc, 0, v6
s_and_saveexec_b64 s[4:5], vcc
s_cbranch_execz .Louter
v_mov_b32 v24, v12
v_mov_b32 v25, v13
v_and_b32 v6, 1, v2
v_cmp_eq_u32 vcc, 0, v6
v_mov_b32 v11, 2.0
s_and_saveexec_b64 s[2:3], vcc
s_cbranch_execz .Lrestore
image_sample_b v22, [v11, v24, v25, v14], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D_ARRAY
.Lrestore:
s_mov_b64 exec, s[2:3]
.Louter:
s_mov_b64 exec, s[4:5]
image_sample_lz v23, v[12:14], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D_ARRAY
s_waitcnt vmcnt(0)
exp mrt0 v20, v21, v22, v23 done vm
s_endpgm
