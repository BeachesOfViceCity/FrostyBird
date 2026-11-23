s_wqm_b64 exec, exec
v_cvt_u32_f32 v1, v0
v_and_b32 v2, 1, v1
v_cmp_eq_u32 vcc, 0, v2
v_cndmask_b32 v4, 17, 9, vcc
s_and_saveexec_b64 s[4:5], vcc
v_readfirstlane_b32 s6, v4
s_andn2_b64 exec, s[4:5], exec
v_readfirstlane_b32 s7, v4
s_mov_b64 exec, s[4:5]
v_and_b32 v2, 3, v1
v_mov_b32 v10, s7
v_mov_b32 v11, s6
v_add_nc_u32 v5, 0x64, v2
v_cmp_gt_u32 vcc, 2, v2
s_and_saveexec_b64 s[8:9], vcc
s_cbranch_execz .Lskip
s_mov_b64 s[10:11], exec
.Lstore:
v_readfirstlane_b32 s12, v2
v_cmp_eq_u32 vcc, s12, v2
s_and_saveexec_b64 vcc, vcc
s_mov_b32 m0, s12
v_movreld_b32 v10, v5
s_xor_b64 exec, exec, vcc
s_cbranch_execnz .Lstore
s_nop 0
s_mov_b64 exec, s[10:11]
.Lskip:
s_mov_b64 exec, s[8:9]
s_mov_b64 s[10:11], exec
v_cmp_gt_u32 vcc, 2, v2
.Lload:
v_readfirstlane_b32 s12, v2
v_cmp_eq_u32 s[14:15], s12, v2
s_and_saveexec_b64 s[14:15], s[14:15]
s_mov_b32 m0, s12
v_movrels_b32 v3, v10
s_xor_b64 exec, exec, s[14:15]
s_cbranch_execnz .Lload
s_mov_b64 exec, s[10:11]
v_cndmask_b32 v6, s7, v3, vcc
v_add_nc_u32 v7, v10, v11
v_cvt_f32_u32 v4, s6
v_cvt_f32_u32 v5, s7
v_cvt_f32_u32 v6, v6
v_cvt_f32_u32 v7, v7
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm
