"""Focused cases for the recompiler: delay slots, branch-likely, calls, tail calls, jump tables, unaligned accesses, HI/LO,
integer corners, FPU compares and conversions.  Each function is a complete guest function; the differential harness runs every
one of them against the interpreter with random arguments and compares the returned registers and all of guest RAM.

    python3 lean_gen.py --synth tests/synth/cases_core.py --out private/synth_core
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))
from leangen.synth import Asm

def build():
    a = Asm()
    A = a.asm
    # ------------------------------------------------------------------ entry + small callees
    A('''
    .func main_entry
        jr ra
        nop
    .func leaf_add
        addu v0, a0, a1
        jr ra
        nop
    .func leaf_ret2
        addiu v0, a0, 3
        addiu v1, a1, 5
        jr ra
        nop
    .func leaf_fret
        mtc1 a0, f0
        add.s f0, f0, f12
        jr ra
        nop
    .func leaf_store
        sw a1, 0(a0)
        sw a2, 4(a0)
        jr ra
        nop
    .func leaf_clobber_t
        addiu t0, zero, 77
        addiu t1, zero, 78
        addiu a0, a0, 1
        jr ra
        move v0, a0
    ''')
    # ------------------------------------------------------------------ delay slot corners
    A('''
    .func ds_arith_either_way
        addiu t0, a0, 1
        beq a0, a1, ds1_taken
        addiu t0, t0, 5
        addu v0, t0, a1
        jr ra
        nop
    ds1_taken:
        addu v0, t0, zero
        jr ra
        nop
    .func ds_slot_writes_cond_reg
        move t0, a0
        bne t0, a1, ds2_t
        addiu t0, t0, 1
        addiu v0, t0, 100
        jr ra
        nop
    ds2_t:
        addiu v0, t0, 200
        jr ra
        nop
    .func ds_likely_taken_runs_slot
        move t1, a1
        beql a0, zero, ds3_t
        addiu t1, t1, 7
        addiu v0, t1, 1
        jr ra
        nop
    ds3_t:
        addu v0, t1, a2
        jr ra
        nop
    .func ds_likely_nottaken_annuls
        move t1, a1
        bnel a0, zero, ds4_t
        addiu t1, t1, 9
        addiu v0, t1, 2
        jr ra
        nop
    ds4_t:
        addu v0, t1, a2
        jr ra
        nop
    .func ds_load_in_slot
        lw t0, 0(a0)
        beq t0, zero, ds5_t
        lw t1, 4(a0)
        addu v0, t0, t1
        jr ra
        nop
    ds5_t:
        addiu v0, t1, 1
        jr ra
        nop
    .func ds_load_use_after_branch
        lw t0, 8(a0)
        beq a1, zero, ds6_t
        addiu t0, t0, 3
        lw t0, 12(a0)
        sw t0, 16(a0)
    ds6_t:
        sw t0, 20(a0)
        move v0, t0
        jr ra
        nop
    .func ds_jal_slot_writes_arg
        addiu sp, sp, -16
        sw ra, 0(sp)
        move a0, a1
        jal leaf_add
        addiu a1, a1, 11
        lw ra, 0(sp)
        addiu sp, sp, 16
        jr ra
        nop
    .func ds_jal_slot_uses_arg_then_call_reads
        addiu sp, sp, -16
        sw ra, 0(sp)
        jal leaf_add
        move a1, a0
        lw ra, 0(sp)
        addiu sp, sp, 16
        jr ra
        nop
    .func ds_jr_ra_slot_writes_v0
        addiu t0, a0, 4
        jr ra
        addu v0, t0, a1
    .func ds_jr_ra_slot_pops_sp
        addiu sp, sp, -16
        sw ra, 8(sp)
        sw a0, 4(sp)
        lw ra, 8(sp)
        lw v0, 4(sp)
        jr ra
        addiu sp, sp, 16
    .func ds_jr_ra_slot_load
        lw v1, 0(a0)
        jr ra
        lw v0, 4(a0)
    .func ds_branch_slot_is_store
        beq a0, zero, ds7_t
        sw a1, 0(a2)
        addiu v0, zero, 1
        jr ra
        nop
    ds7_t:
        addiu v0, zero, 2
        jr ra
        nop
    .func ds_back_edge_loop
        move v0, zero
        move t0, a1
        andi t0, t0, 15
    ds8_l:
        addu v0, v0, t0
        addiu t0, t0, -1
        bgtz t0, ds8_l
        sll v0, v0, 1
        jr ra
        nop
    .func ds_likely_loop
        move v0, zero
        andi t0, a1, 7
        addiu t0, t0, 1
    ds9_l:
        addiu v0, v0, 3
        addiu t0, t0, -1
        bgtzl t0, ds9_l
        addiu v0, v0, 1
        jr ra
        nop
    .func ds_slot_branch_target_reads_slot_def
        slt t0, a0, a1
        beq t0, zero, ds10_t
        move v1, a0
        move v1, a1
    ds10_t:
        move v0, v1
        jr ra
        nop
    .func ds_blez_bltz_bgez
        move v0, zero
        blez a0, ds11_a
        nop
        addiu v0, v0, 1
    ds11_a:
        bltz a1, ds11_b
        nop
        addiu v0, v0, 2
    ds11_b:
        bgez a2, ds11_c
        addiu v0, v0, 4
        addiu v0, v0, 8
    ds11_c:
        bltzl a3, ds11_d
        addiu v0, v0, 16
        addiu v0, v0, 32
    ds11_d:
        bgezl a0, ds11_e
        addiu v0, v0, 64
        addiu v0, v0, 128
    ds11_e:
        jr ra
        nop
    ''')
    # ------------------------------------------------------------------ calls
    A('''
    .func call_chain
        addiu sp, sp, -32
        sw ra, 28(sp)
        sw s0, 24(sp)
        sw s1, 20(sp)
        move s0, a0
        move s1, a1
        jal leaf_add
        move a1, s1
        move a0, v0
        jal leaf_ret2
        move a1, s0
        addu v0, v0, v1
        addu v0, v0, s0
        lw s1, 20(sp)
        lw s0, 24(sp)
        lw ra, 28(sp)
        jr ra
        addiu sp, sp, 32
    .func call_float_ret
        addiu sp, sp, -16
        sw ra, 12(sp)
        jal leaf_fret
        move a0, a1
        mov.s f12, f0
        jal leaf_fret
        move a0, a2
        lw ra, 12(sp)
        jr ra
        addiu sp, sp, 16
    .func call_stack_args
        addiu sp, sp, -48
        sw ra, 44(sp)
        sw a0, 16(sp)
        sw a1, 20(sp)
        jal callee_stack_args
        move a3, a2
        lw ra, 44(sp)
        jr ra
        addiu sp, sp, 48
    .func callee_stack_args
        lw t0, 16(sp)
        lw t1, 20(sp)
        addu v0, t0, t1
        addu v0, v0, a3
        jr ra
        nop
    .func call_in_loop
        addiu sp, sp, -32
        sw ra, 28(sp)
        sw s0, 24(sp)
        sw s1, 20(sp)
        andi s0, a1, 7
        move s1, a0
    ci_l:
        move a0, s1
        move a1, s0
        jal leaf_add
        nop
        move s1, v0
        addiu s0, s0, -1
        bgtz s0, ci_l
        nop
        move v0, s1
        lw s1, 20(sp)
        lw s0, 24(sp)
        lw ra, 28(sp)
        jr ra
        addiu sp, sp, 32
    .func recurse_sum
        addiu sp, sp, -16
        sw ra, 12(sp)
        sw s0, 8(sp)
        andi a0, a0, 15
        beq a0, zero, rs_base
        move s0, a0
        addiu a0, a0, -1
        jal recurse_sum
        nop
        addu v0, v0, s0
        b_end:
        lw s0, 8(sp)
        lw ra, 12(sp)
        jr ra
        addiu sp, sp, 16
    rs_base:
        move v0, zero
        lw s0, 8(sp)
        lw ra, 12(sp)
        jr ra
        addiu sp, sp, 16
    .func call_indirect
        addiu sp, sp, -16
        sw ra, 12(sp)
        la t9, leaf_add
        move t0, a0
        andi t0, t0, 1
        beq t0, zero, cind_a
        nop
        la t9, leaf_ret2
    cind_a:
        move a0, a1
        move a1, a2
        jalr t9
        nop
        lw ra, 12(sp)
        jr ra
        addiu sp, sp, 16
    .func call_clobber_check
        addiu sp, sp, -16
        sw ra, 12(sp)
        addiu t0, zero, 5
        jal leaf_clobber_t
        nop
        addu v0, v0, t0
        lw ra, 12(sp)
        jr ra
        addiu sp, sp, 16
    ''')
    # ABI violator: callee leaves s0 changed; its caller reads s0 afterwards (regfile form)
    A('''
    .func viol_leaf
        addiu s0, a0, 5
        addiu s1, a1, 6
        jr ra
        nop
    .func viol_user
        addiu sp, sp, -32
        sw ra, 28(sp)
        sw s0, 24(sp)
        sw s1, 20(sp)
        li s0, 3
        li s1, 4
        jal viol_leaf
        nop
        addu v0, s0, s1
        addu v0, v0, a0
        lw s1, 20(sp)
        lw s0, 24(sp)
        lw ra, 28(sp)
        jr ra
        addiu sp, sp, 32
    ''')
    # ------------------------------------------------------------------ tail calls
    A('''
    .func tail_simple
        addiu a0, a0, 1
        j leaf_add
        nop
    .func tail_slot_arg
        j leaf_add
        addiu a1, a1, 9
    .func tail_after_frame
        addiu sp, sp, -16
        sw ra, 12(sp)
        sw a0, 8(sp)
        lw a1, 8(sp)
        lw ra, 12(sp)
        j leaf_add
        addiu sp, sp, 16
    .func tail_cond
        beq a0, zero, tc_a
        nop
        j leaf_ret2
        nop
    tc_a:
        j leaf_add
        nop
    .func tail_in_loop
        andi t0, a0, 3
    tl_l:
        beq t0, zero, tl_done
        addiu t0, t0, -1
        b tl_l
        nop
    tl_done:
        j leaf_add
        move a0, a1
    .func tail_float
        j leaf_fret
        move a0, a1
    ''')
    # ------------------------------------------------------------------ jump tables
    pats = {}
    def jt_idx(name, n, shift_in_slot):
        labs = ['%s_c%d' % (name, i) for i in range(n)]
        a.table(name + '_tab', labs)
        body = ['.func %s' % name, 'sltiu t0, a0, %d' % n, 'beq t0, zero, %s_def' % name]
        body += (['sll t1, a0, 2'] if shift_in_slot else ['nop', 'sll t1, a0, 2'])
        body += ['la t2, %s_tab' % name, 'addu t2, t2, t1', 'lw t3, 0(t2)', 'nop', 'jr t3', 'nop']
        for i, l in enumerate(labs):
            body += ['%s:' % l, 'addiu v0, a1, %d' % (i * 3 + 1), 'jr ra', 'move v1, a2']
        body += ['%s_def:' % name, 'li v0, -1', 'jr ra', 'nop']
        A('\n'.join(body))
    jt_idx('jt_idx_slot', 6, True)
    jt_idx('jt_idx_noslot', 9, False)
    jt_idx('jt_idx_two', 2, True)
    # case bodies that fall into each other / share code; the index register is used again after the sll (val mode)
    labs = ['jt_val_c%d' % i for i in range(5)]
    a.table('jt_val_tab', labs)
    A('''
    .func jt_val
        sltiu t0, a0, 5
        beq t0, zero, jt_val_def
        sll a0, a0, 2
        la t2, jt_val_tab
        addu t2, t2, a0
        addiu a0, a0, 1
        lw t3, 0(t2)
        nop
        jr t3
        nop
    jt_val_c0:
        addiu v0, a0, 100
        jr ra
        nop
    jt_val_c1:
        addiu v0, a0, 200
        jr ra
        nop
    jt_val_c2:
        addiu a0, a0, 7
    jt_val_c3:
        addiu v0, a0, 300
        jr ra
        nop
    jt_val_c4:
        addiu v0, a1, 400
        jr ra
        nop
    jt_val_def:
        li v0, -2
        jr ra
        nop
    ''')
    labs = ['jt_call_c%d' % i for i in range(4)]
    a.table('jt_call_tab', labs)
    A('''
    .func jt_call
        addiu sp, sp, -16
        sw ra, 12(sp)
        sltiu t0, a0, 4
        beq t0, zero, jt_call_def
        sll t1, a0, 2
        la t2, jt_call_tab
        addu t2, t2, t1
        lw t3, 0(t2)
        nop
        jr t3
        nop
    jt_call_c0:
        jal leaf_add
        move a0, a1
        b jt_call_out
        nop
    jt_call_c1:
        jal leaf_ret2
        move a0, a1
        addu v0, v0, v1
        b jt_call_out
        nop
    jt_call_c2:
        move v0, a2
        b jt_call_out
        nop
    jt_call_c3:
        lw ra, 12(sp)
        j leaf_add
        addiu sp, sp, 16
    jt_call_def:
        li v0, 9
    jt_call_out:
        lw ra, 12(sp)
        jr ra
        addiu sp, sp, 16
    ''')
    # ------------------------------------------------------------------ unaligned accesses
    body = []
    for off in range(4):
        # lwl / lwr in both orders, fused (adjacent) and unfused (a use in between), loads into a register that holds a value
        body += ['.func ua_load_lwl_lwr_%d' % off, 'li v0, 0x11223344', 'lwl v0, %d(a0)' % (off + 3), 'lwr v0, %d(a0)' % off, 'jr ra', 'nop']
        body += ['.func ua_load_lwr_lwl_%d' % off, 'li v0, 0x55667788', 'lwr v0, %d(a0)' % off, 'lwl v0, %d(a0)' % (off + 3), 'jr ra', 'nop']
        body += ['.func ua_load_lwl_only_%d' % off, 'li v0, 0x01020304', 'lwl v0, %d(a0)' % off, 'jr ra', 'nop']
        body += ['.func ua_load_lwr_only_%d' % off, 'li v0, 0x0A0B0C0D', 'lwr v0, %d(a0)' % off, 'jr ra', 'nop']
        body += ['.func ua_load_split_%d' % off, 'li v0, 0x13572468', 'lwl v0, %d(a0)' % (off + 3), 'addu v1, v0, a1', 'lwr v0, %d(a0)' % off,
                 'addu v1, v1, v0', 'jr ra', 'nop']
        body += ['.func ua_store_swl_swr_%d' % off, 'swl a1, %d(a0)' % (off + 3), 'swr a1, %d(a0)' % off, 'jr ra', 'nop']
        body += ['.func ua_store_swr_swl_%d' % off, 'swr a1, %d(a0)' % off, 'swl a1, %d(a0)' % (off + 3), 'jr ra', 'nop']
        body += ['.func ua_store_swl_only_%d' % off, 'swl a1, %d(a0)' % off, 'jr ra', 'nop']
        body += ['.func ua_store_swr_only_%d' % off, 'swr a1, %d(a0)' % off, 'jr ra', 'nop']
        body += ['.func ua_roundtrip_%d' % off, 'lwl t0, %d(a0)' % (off + 3), 'lwr t0, %d(a0)' % off, 'swl t0, %d(a2)' % (off + 3),
                 'swr t0, %d(a2)' % off, 'move v0, t0', 'jr ra', 'nop']
    A('\n'.join(body))
    # ------------------------------------------------------------------ memory width / sign
    A('''
    .func mem_widths
        lb t0, 1(a0)
        lbu t1, 2(a0)
        lh t2, 4(a0)
        lhu t3, 6(a0)
        lw t4, 8(a0)
        sb t0, 16(a0)
        sb t1, 17(a0)
        sh t2, 18(a0)
        sh t3, 20(a0)
        sw t4, 24(a0)
        addu v0, t0, t1
        addu v0, v0, t2
        addu v0, v0, t3
        xor v1, v0, t4
        jr ra
        nop
    .func mem_gp
        lw t0, 0(gp)
        lw v0, 4(gp)
        sw a1, 8(gp)
        addu v0, v0, t0
        jr ra
        nop
    ''')
    # ------------------------------------------------------------------ integer ALU / HI-LO
    A('''
    .func alu_mix
        addu t0, a0, a1
        subu t1, a0, a1
        and t2, a0, a1
        or t3, a0, a1
        xor t4, a0, a1
        nor t5, a0, a1
        slt t6, a0, a1
        sltu t7, a0, a1
        sw t0, 0(a2)
        sw t1, 4(a2)
        sw t2, 8(a2)
        sw t3, 12(a2)
        sw t4, 16(a2)
        sw t5, 20(a2)
        sw t6, 24(a2)
        sw t7, 28(a2)
        sll t0, a0, 5
        srl t1, a0, 7
        sra t2, a0, 9
        rotr t3, a0, 11
        sllv t4, a0, a1
        srlv t5, a0, a1
        srav t6, a0, a1
        slti t7, a0, -5
        sw t0, 32(a2)
        sw t1, 36(a2)
        sw t2, 40(a2)
        sw t3, 44(a2)
        sw t4, 48(a2)
        sw t5, 52(a2)
        sw t6, 56(a2)
        sw t7, 60(a2)
        sltiu t0, a0, 20
        andi t1, a0, 0xF0F0
        ori t2, a0, 0x1234
        xori t3, a0, 0xFFFF
        lui t4, 0x8001
        clz t5, a0
        clo t6, a0
        max t7, a0, a1
        sw t0, 64(a2)
        sw t1, 68(a2)
        sw t2, 72(a2)
        sw t3, 76(a2)
        sw t4, 80(a2)
        sw t5, 84(a2)
        sw t6, 88(a2)
        sw t7, 92(a2)
        min t0, a0, a1
        seb t1, a0
        seh t2, a0
        wsbh t3, a0
        bitrev t4, a0
        ext t5, a0, 3, 9
        ins a1, a0, 4, 12
        wsbw t6, a0
        sw t0, 96(a2)
        sw t1, 100(a2)
        sw t2, 104(a2)
        sw t3, 108(a2)
        sw t4, 112(a2)
        sw t5, 116(a2)
        sw a1, 120(a2)
        sw t6, 124(a2)
        move v0, t0
        jr ra
        nop
    .func alu_movz_movn
        move v0, a0
        move v1, a1
        movz v0, a2, a3
        movn v1, a2, a3
        jr ra
        nop
    .func hilo_mult_div
        mult a0, a1
        mflo t0
        mfhi t1
        multu a0, a1
        mflo t2
        mfhi t3
        sw t0, 0(a2)
        sw t1, 4(a2)
        sw t2, 8(a2)
        sw t3, 12(a2)
        div a0, a1
        mflo t0
        mfhi t1
        divu a0, a1
        mflo t2
        mfhi t3
        sw t0, 16(a2)
        sw t1, 20(a2)
        sw t2, 24(a2)
        sw t3, 28(a2)
        madd a0, a1
        mflo t0
        mfhi t1
        maddu a0, a1
        msub a1, a0
        msubu a1, a0
        mflo t2
        mfhi t3
        mthi a0
        mtlo a1
        mfhi v0
        mflo v1
        sw t0, 32(a2)
        sw t1, 36(a2)
        sw t2, 40(a2)
        sw t3, 44(a2)
        jr ra
        nop
    .func hilo_div_special
        li t0, 0x80000000
        li t1, -1
        div t0, t1
        mflo v0
        mfhi v1
        div a0, zero
        mflo t2
        mfhi t3
        divu a0, zero
        mflo t4
        mfhi t5
        sw t2, 0(a1)
        sw t3, 4(a1)
        sw t4, 8(a1)
        sw t5, 12(a1)
        jr ra
        nop
    ''')
    # ------------------------------------------------------------------ FPU
    fp = []
    conds = ['f', 'un', 'eq', 'ueq', 'olt', 'ult', 'ole', 'ule', 'sf', 'ngle', 'seq', 'ngl', 'lt', 'nge', 'le', 'ngt']
    for i, c in enumerate(conds):
        fp += ['.func fpu_cmp_%s' % c, 'c.%s.s f12, f13' % c, 'bc1t fp_%s_t' % c, 'nop', 'li v0, 1', 'jr ra', 'nop',
               'fp_%s_t:' % c, 'li v0, 2', 'jr ra', 'nop']
        fp += ['.func fpu_cmp_l_%s' % c, 'c.%s.s f12, f13' % c, 'li v0, 5', 'bc1fl fp_l_%s_t' % c, 'li v0, 6', 'li v0, 7', 'jr ra', 'nop',
               'fp_l_%s_t:' % c, 'addiu v0, v0, 100', 'jr ra', 'nop']
    fp += ['.func fpu_cfc_ctc', 'cfc1 t0, 31', 'ctc1 a0, 31', 'cfc1 t1, 31', 'c.lt.s f12, f13', 'cfc1 t2, 31', 'sw t0, 0(a1)', 'sw t1, 4(a1)',
           'sw t2, 8(a1)', 'move v0, t1', 'jr ra', 'nop']
    fp += ['.func fpu_arith', 'add.s f0, f12, f13', 'sub.s f1, f12, f13', 'mul.s f2, f12, f13', 'div.s f3, f12, f13', 'sqrt.s f4, f12',
           'abs.s f5, f13', 'neg.s f6, f12', 'mov.s f7, f13',
           'swc1 f0, 0(a0)', 'swc1 f1, 4(a0)', 'swc1 f2, 8(a0)', 'swc1 f3, 12(a0)', 'swc1 f4, 16(a0)', 'swc1 f5, 20(a0)', 'swc1 f6, 24(a0)',
           'swc1 f7, 28(a0)', 'mov.s f0, f3', 'jr ra', 'nop']
    fp += ['.func fpu_cvt', 'trunc.w.s f0, f12', 'round.w.s f1, f12', 'ceil.w.s f2, f12', 'floor.w.s f3, f12', 'cvt.w.s f4, f12',
           'swc1 f0, 0(a0)', 'swc1 f1, 4(a0)', 'swc1 f2, 8(a0)', 'swc1 f3, 12(a0)', 'swc1 f4, 16(a0)',
           'mfc1 t0, f0', 'mtc1 a1, f5', 'cvt.s.w f5, f5', 'swc1 f5, 20(a0)', 'mfc1 v0, f4', 'jr ra', 'nop']
    fp += ['.func fpu_mem', 'lwc1 f1, 0(a0)', 'lwc1 f2, 4(a0)', 'add.s f1, f1, f2', 'swc1 f1, 8(a0)', 'mfc1 v0, f1', 'mtc1 a1, f9',
           'swc1 f9, 12(a0)', 'jr ra', 'nop']
    A('\n'.join(fp))
    # ------------------------------------------------------------------ imports
    a.import_stub('imp0', 0x11111111)
    a.import_stub('imp1', 0x22222222)
    A('''
    .func call_import
        addiu sp, sp, -16
        sw ra, 12(sp)
        jal imp0
        nop
        move a0, v0
        jal imp1
        move a1, v1
        addu v0, v0, a0
        lw ra, 12(sp)
        jr ra
        addiu sp, sp, 16
    ''')
    im = a.image()
    profile = {'noreturn': [], 'weak_args': 4, 'yield': [], 'hooks': {}, 'extra_inputs': {}}
    return im, profile
