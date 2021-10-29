// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the 
// specific language governing permissions and limitations under the License.

#ifndef TNN_CONV_SGEMM_AVX_16xI_H_
#define TNN_CONV_SGEMM_AVX_16xI_H_

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <fstream>
#include <immintrin.h>
#include <xmmintrin.h>
#include <exception>
#include <utility>

#include <xbyak/xbyak.h>

#include "tnn/device/x86/acc/compute/jit/common/type_def.h"
#include "tnn/device/x86/acc/compute/jit/common/abi_info.h"
#include "tnn/device/x86/acc/compute/jit/common/asm_common.h"
#include "tnn/device/x86/acc/compute/jit/utils/macro.h"
#include "tnn/device/x86/acc/compute/jit/kernels/base_jit_kernel.h"

namespace TNN_NS {
namespace jit {

template<int I, int M_BLOCK_SIZE, int N_BLOCK_SIZE>
class conv_sgemm_avx_16xi: public base_jit_kernel {

public:
    static void naive_impl(const dim_t K,
                           const float * src_a, const dim_t lda,
                           const float * src_b, dim_t ldb,
                           float * dst, dim_t ldc,
                           const float * bias, dim_t first, dim_t act_type) {}

    using func_ptr_t = decltype(&conv_sgemm_avx_16xi::naive_impl);

    virtual std::string get_kernel_name() {
        std::stringstream buf;
        buf << JIT_KERNEL_NAME(conv_sgemm_avx_16) << "_" << I << "_" << M_BLOCK_SIZE << "_" << N_BLOCK_SIZE;
        return buf.str();
    }

public:
    conv_sgemm_avx_16xi() {

#ifdef XBYAK64
        constexpr int N_r = MIN_(6, I);

        declare_param<const dim_t>();       // 0. K
        declare_param<const float *>();     // 1. src_a
        declare_param<const dim_t>();       // 2. lda
        declare_param<const float *>();     // 3. src_b
        declare_param<const dim_t>();       // 4. ldb
        declare_param<float *>();           // 5. dst
        declare_param<const dim_t>();       // 6. ldc
        declare_param<const float *>();     // 7. bias
        declare_param<dim_t>();             // 8. first
        declare_param<dim_t>();             // 9. act_type

        abi_prolog();

        // stack_var K         = get_arguement_to_stack(0);
        reg_var K           = get_arguement(0);
        reg_var src_a       = get_arguement(1);
        reg_var lda         = get_arguement(2);
        reg_var src_b       = get_arguement(3);
        reg_var N           = get_arguement(4);
        reg_var dst         = get_arguement(5);
        reg_var ldc         = get_arguement(6);
        reg_var bias        = get_arguement(7);
        reg_var first       = get_arguement(8);
        reg_var act_type    = get_arguement(9);

        reg_var c[3] = {REG_VAR_ARRAY_3};
        reg_var op_6f(this);
        vreg_var v_const(this);
        vreg_var c_data[2][6] = {{VREG_VAR_ARRAY_6}, {VREG_VAR_ARRAY_6}};
        vreg_var a_data[2] = {VREG_VAR_ARRAY_2};
        vreg_var b_data[2] = {VREG_VAR_ARRAY_2};
        
        ldc.restore();

        if (I == 6 && N_BLOCK_SIZE == 6) {
            N.restore();
            dst.restore();
            bias.restore();
            src_b.restore();
            first.restore();

            LOOP(N, LoopN)
            {
                // dst n0
                mov(c[0].aquire(), dst);
                lea(c[1].aquire(), byte[dst + (ldc * 8)]);
                lea(c[2].aquire(), byte[c[1] + (ldc * 8)]);

                Xbyak::RegExp c_addr[6] = {
                    Xbyak::RegExp(c[0]),
                    Xbyak::RegExp(c[0] + (ldc * 4)),
                    Xbyak::RegExp(c[1]),
                    Xbyak::RegExp(c[1] + (ldc * 4)),
                    Xbyak::RegExp(c[2]),
                    Xbyak::RegExp(c[2] + (ldc * 4)),
                };

                cmp(bias, 0);
                jne("L_init");
                for(int i=0;i<N_r;i++) {
                    c_data[0][i].aquire();
                    c_data[1][i].aquire();
                    vxorps(c_data[0][i], c_data[0][i], c_data[0][i]);
                    vxorps(c_data[1][i], c_data[1][i], c_data[1][i]);
                }
                jmp("L_init_end");
                L("L_init");
                for(int i=0;i<N_r;i++) {
                    vbroadcastss(c_data[0][i], dword[bias + i * 4]);
                    vbroadcastss(c_data[1][i], dword[bias + i * 4]);
                }
                // bias += 6 * sizeof(float)
                lea(bias, byte[bias + 24]);
                L("L_init_end");

                // for lstm gemm, need to accumulate based on dst data
                cmp(first, 0);
                je("L_fromC");
                for(int i=0;i<N_r;i++) {
                    vmovups(c_data[0][i], yword[c_addr[i]]);
                    vmovups(c_data[1][i], yword[c_addr[i] + 8 * 4]);
                }
                L("L_fromC");

                src_a.restore();
                K.restore();

                LOOP(K, SGEMM_AVX_8X6_K) 
                {
                    vmovaps(a_data[0].aquire(), yword[src_a]);
                    vmovaps(a_data[1].aquire(), yword[src_a + 8 * 4]);
                    prefetcht0(yword[src_a + 256]);

                    for(int i=0;i<N_r;i+=2) {
                        vbroadcastss(b_data[0].aquire(), yword[src_b + i * 4]);
                        vfmadd231ps(c_data[0][i],   a_data[0], b_data[0]);
                        vfmadd231ps(c_data[1][i],   a_data[1], b_data[0].release());

                        if (i + 1 < N_r) {
                            vbroadcastss(b_data[1].aquire(), yword[src_b + i * 4 + 4]);
                            vfmadd231ps(c_data[0][i+1], a_data[0], b_data[1]);
                            vfmadd231ps(c_data[1][i+1], a_data[1], b_data[1].release());
                        }
                    }

                    a_data[0].release();
                    a_data[1].release();

                    lea(src_a, byte[src_a + M_BLOCK_SIZE * 4]);
                    lea(src_b, byte[src_b + N_BLOCK_SIZE * 4]);
                }

                src_a.release();
                K.release();

                // only support fuse relu, relu6
                act_type.restore();
                cmp(act_type, 0);
                je("L_post_end_1");
                    v_const.aquire();
                    vxorps(v_const, v_const, v_const);
                    for(int i=0;i<N_r;i++) {
                        vmaxps(c_data[0][i], c_data[0][i], v_const);
                        vmaxps(c_data[1][i], c_data[1][i], v_const);
                    }
                    v_const.release();
                L("L_post_end_1");

                cmp(act_type, 2);
                jne("L_post_end_2");
                    op_6f.restore();
                    v_const.aquire();
                    // 6.f
                    mov(op_6f.cvt32(), 0x40C00000);
                    movd(v_const.xmm(), op_6f.cvt32());
                    vbroadcastss(v_const, v_const.xmm());
                    for(int i=0;i<N_r;i++) {
                        vminps(c_data[0][i], c_data[0][i], v_const);
                        vminps(c_data[1][i], c_data[1][i], v_const);
                    }
                    v_const.release();
                    op_6f.release();
                L("L_post_end_2");
                act_type.release();

                for(int i=0;i<N_r;i++) {
                    vmovups(yword[c_addr[i]],         c_data[0][i]);
                    vmovups(yword[c_addr[i] + 8 * 4], c_data[1][i]);
                }

                // dst = c[2] + 2 x ldc x sizeof(float)
                lea(dst, byte[c[2] + (ldc * 8)]);
            }
            N.release();
            dst.release();
            bias.release();
            src_b.release();
            first.release();
        }
        else
        {
            mov(c[0].aquire(), dst.restore());
            lea(c[1].aquire(), byte[dst + (ldc * 8)]);
            lea(c[2].aquire(), byte[c[1]+ (ldc * 8)]);
            dst.release();

            Xbyak::RegExp c_addr[6] = {
                Xbyak::RegExp(c[0]),
                Xbyak::RegExp(c[0] + (ldc * 4)),
                Xbyak::RegExp(c[1]),
                Xbyak::RegExp(c[1] + (ldc * 4)),
                Xbyak::RegExp(c[2]),
                Xbyak::RegExp(c[2] + (ldc * 4)),
            };

            bias.restore();
            cmp(bias, 0);
            jne("L_init");
            for(int i=0;i<N_r;i++) {
                c_data[0][i].aquire();
                c_data[1][i].aquire();
                vxorps(c_data[0][i], c_data[0][i], c_data[0][i]);
                vxorps(c_data[1][i], c_data[1][i], c_data[1][i]);
            }
            jmp("L_init_end");
            L("L_init");
            for(int i=0;i<N_r;i++) {
                vbroadcastss(c_data[0][i], dword[bias + i * 4]);
                vbroadcastss(c_data[1][i], dword[bias + i * 4]);
            }
            L("L_init_end");
            bias.release();

            // for lstm gemm, need to accumulate based on dst data
            first.restore();
            cmp(first, 0);
            je("L_fromC");
            for(int i=0;i<N_r;i++) {
                vmovups(c_data[0][i], yword[c_addr[i]]);
                vmovups(c_data[1][i], yword[c_addr[i] + 8 * 4]);
            }
            L("L_fromC");
            first.release();

            src_a.restore();
            src_b.restore();
            K.restore();

            LOOP(K, SGEMM_AVX_8X6_K) 
            {
                vmovaps(a_data[0].aquire(), yword[src_a]);
                vmovaps(a_data[1].aquire(), yword[src_a + 8 * 4]);

                for(int i=0;i<N_r;i+=2) {
                    vbroadcastss(b_data[0].aquire(), yword[src_b + i * 4]);
                    vfmadd231ps(c_data[0][i],   a_data[0], b_data[0]);
                    vfmadd231ps(c_data[1][i],   a_data[1], b_data[0].release());

                    if (i + 1 < N_r) {
                        vbroadcastss(b_data[1].aquire(), yword[src_b + i * 4 + 4]);
                        vfmadd231ps(c_data[0][i+1], a_data[0], b_data[1]);
                        vfmadd231ps(c_data[1][i+1], a_data[1], b_data[1].release());
                    }
                }

                a_data[0].release();
                a_data[1].release();

                lea(src_a, byte[src_a + M_BLOCK_SIZE * 4]);
                lea(src_b, byte[src_b + N_BLOCK_SIZE * 4]);
            }

            src_a.release();
            src_b.release();
            K.release();

            // only support fuse relu, relu6
            act_type.restore();
            cmp(act_type, 0);
            je("L_post_end_1");
                v_const.aquire();
                vxorps(v_const, v_const, v_const);
                for(int i=0;i<N_r;i++) {
                    vmaxps(c_data[0][i], c_data[0][i], v_const);
                    vmaxps(c_data[1][i], c_data[1][i], v_const);
                }
                v_const.release();
            L("L_post_end_1");

            cmp(act_type, 2);
            jne("L_post_end_2");
                op_6f.restore();
                v_const.aquire();
                // 6.f
                mov(op_6f.cvt32(), 0x40C00000);
                movd(v_const.xmm(), op_6f.cvt32());
                vbroadcastss(v_const, v_const.xmm());
                for(int i=0;i<N_r;i++) {
                    vminps(c_data[0][i], c_data[0][i], v_const);
                    vminps(c_data[1][i], c_data[1][i], v_const);
                }
                v_const.release();
                op_6f.release();
            L("L_post_end_2");
            act_type.release();

            for(int i=0;i<N_r;i++) {
                vmovups(yword[c_addr[i]],         c_data[0][i]);
                vmovups(yword[c_addr[i] + 8 * 4], c_data[1][i]);
            }
        }

        abi_epilog();
#endif // XBYAK64
        ret();
    }

    virtual ~conv_sgemm_avx_16xi() {

    }

private:

};

} // namespace jit
} // namespace tnn

#endif // TNN_CONV_SGEMM_AVX_16xI_H_
