#include "base.inc"

// input: [input_size / 4, sequence * batch]
// w: [hidden_size * num_directions, input_size]
// gates: [hidden_size * num_directions, sequence * batch]
__kernel void LSTMONNXGates(GLOBAL_SIZE_2_DIMS __read_only image2d_t input,
                            __read_only image2d_t w,
                            __private const int input_size_updiv_4,
                            __write_only image2d_t output_gates) {
    const int hid_dir_idx  = get_global_id(0);
    const int seq_b_idx = get_global_id(1);

    DEAL_NON_UNIFORM_DIM2(hid_dir_idx, seq_b_idx);

    FLOAT4 out = 0;
    FLOAT4 weights_0, weights_1, weights_2, weights_3;
    for (int i = 0; i < input_size_updiv_4; i++) {
        int input_index = i << 2;
        FLOAT4 in = RI_F(input, SAMPLER, (int2)(i, seq_b_idx));
        weights_0 = RI_F(w, SAMPLER, (int2)(hid_dir_idx, input_index));
        weights_1 = RI_F(w, SAMPLER, (int2)(hid_dir_idx, input_index + 1));
        weights_2 = RI_F(w, SAMPLER, (int2)(hid_dir_idx, input_index + 2));
        weights_3 = RI_F(w, SAMPLER, (int2)(hid_dir_idx, input_index + 3));

        out = mad(in.x, weights_0, out);
        out = mad(in.y, weights_1, out);
        out = mad(in.z, weights_2, out);
        out = mad(in.w, weights_3, out);
    }

    WI_F(output_gates, (int2)(hid_dir_idx, seq_b_idx), out);
}

// gates: [hidden_size * num_directions, sequence * batch]
// r: [hidden_size * num_directions, hidden_size]
// bias: [2* hidden_size, num_directions]
// initial cell: [hidden_size / 4, num_directions * batch]
// initial hidden: [hidden_size / 4, num_directions * batch]
// output: [hidden_size / 4 * num_directions, sequence * batch]
// cell: [hidden_size / 4, num_directions * batch]
// hidden: [hidden_size / 4, num_directions * batch]
__kernel void LSTMONNXForward(GLOBAL_SIZE_2_DIMS __read_only image2d_t gates,
                              __read_only image2d_t r,
                              __read_only image2d_t bias,
                              __read_only image2d_t h_0,
                              __read_only image2d_t c_0,
                              __private const int sequence,
                              __private const int num_directions,
                              __private const int hidden_size_updiv_4,
                              __private const int reverse,
                              __local FLOAT4* h_local,
                              __write_only image2d_t output,
                              __write_only image2d_t output_hidden,
                              __write_only image2d_t output_cell) {
    const int hid_4_dir_idx  = get_global_id(0);
    const int batch = get_global_id(1);

    DEAL_NON_UNIFORM_DIM2(hid_4_dir_idx, batch);
    const int hid_4_idx = hid_4_dir_idx % hidden_size_updiv_4;
    const int dir_idx   = hid_4_dir_idx / hidden_size_updiv_4;

    bool forward = dir_idx == 0 && !reverse;

    const int hid_4_idx_I = hid_4_idx;
    const int hid_4_idx_O = hid_4_idx_I + hidden_size_updiv_4;
    const int hid_4_idx_F = hid_4_idx_O + hidden_size_updiv_4;
    const int hid_4_idx_C = hid_4_idx_F + hidden_size_updiv_4;
    int4 hid_4_idx = (int4)(hid_4_idx_I, hid_4_idx_O, hid_4_idx_F, hid_4_idx_C);

    const int hid_4_idx_b_r_I = hid_4_idx_C + hidden_size_updiv_4;
    const int hid_4_idx_b_r_O = hid_4_idx_b_r_I + hidden_size_updiv_4;
    const int hid_4_idx_b_r_F = hid_4_idx_b_r_O + hidden_size_updiv_4;
    const int hid_4_idx_b_r_C = hid_4_idx_b_r_F + hidden_size_updiv_4;

    FLOAT4 I = RI_F(bias, SAMPLER, (int2)(hid_4_idx_I, dir_idx)) +
               RI_F(bias, SAMPLER, (int2)(hid_4_idx_b_r_I, dir_idx));
    FLOAT4 O = RI_F(bias, SAMPLER, (int2)(hid_4_idx_O, dir_idx)) +
               RI_F(bias, SAMPLER, (int2)(hid_4_idx_b_r_O, dir_idx));
    FLOAT4 F = RI_F(bias, SAMPLER, (int2)(hid_4_idx_F, dir_idx)) +
               RI_F(bias, SAMPLER, (int2)(hid_4_idx_b_r_F, dir_idx));
    FLOAT4 C = RI_F(bias, SAMPLER, (int2)(hid_4_idx_C, dir_idx)) +
               RI_F(bias, SAMPLER, (int2)(hid_4_idx_b_r_C, dir_idx));
    int gates_w_offset = hidden_size * dir_idx;
    int4 gates_w_idx = hid_4_idx + gates_w_offset;
    int gates_h_offset = sequence * batch;
    int state_h_idx = batch * num_directions + dir_idx;

    FLOAT4 cell = RI_F(c_0, SAMPLER, (int2)(hid_4_idx, state_h_idx));
    h_local[hid_4_idx] = RI_F(h_0, SAMPLER, (int2)(hid_4_idx, state_h_idx));
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int seq = 0; seq < sequence; seq++) {
        int t = forward ? seq : sequence - 1 - seq;
        int gates_h = t + gates_h_offset;

        I += RI_F(gates, SAMPLER, int2(gates_w_idx.x, gates_h));
        O += RI_F(gates, SAMPLER, int2(gates_w_idx.y, gates_h));
        F += RI_F(gates, SAMPLER, int2(gates_w_idx.z, gates_h));
        C += RI_F(gates, SAMPLER, int2(gates_w_idx.w, gates_h));

        for (int i = 0; i < hidden_size_updiv_4; ++i) {
            int hidden_index = i << 2;
            FLOAT4 h = h_local[i];
            FLOAT4 r_I_0 = RI_F(r, SAMPLER, int2(gates_w_idx.x, hidden_index));
            FLOAT4 r_I_1 = RI_F(r, SAMPLER, int2(gates_w_idx.x, hidden_index + 1));
            FLOAT4 r_I_2 = RI_F(r, SAMPLER, int2(gates_w_idx.x, hidden_index + 2));
            FLOAT4 r_I_3 = RI_F(r, SAMPLER, int2(gates_w_idx.x, hidden_index + 3));
            FLOAT4 r_O_0 = RI_F(r, SAMPLER, int2(gates_w_idx.y, hidden_index));
            FLOAT4 r_O_1 = RI_F(r, SAMPLER, int2(gates_w_idx.y, hidden_index + 1));
            FLOAT4 r_O_2 = RI_F(r, SAMPLER, int2(gates_w_idx.y, hidden_index + 2));
            FLOAT4 r_O_3 = RI_F(r, SAMPLER, int2(gates_w_idx.y, hidden_index + 3));
            FLOAT4 r_F_0 = RI_F(r, SAMPLER, int2(gates_w_idx.z, hidden_index));
            FLOAT4 r_F_1 = RI_F(r, SAMPLER, int2(gates_w_idx.z, hidden_index + 1));
            FLOAT4 r_F_2 = RI_F(r, SAMPLER, int2(gates_w_idx.z, hidden_index + 2));
            FLOAT4 r_F_3 = RI_F(r, SAMPLER, int2(gates_w_idx.z, hidden_index + 3));
            FLOAT4 r_C_0 = RI_F(r, SAMPLER, int2(gates_w_idx.w, hidden_index));
            FLOAT4 r_C_1 = RI_F(r, SAMPLER, int2(gates_w_idx.w, hidden_index + 1));
            FLOAT4 r_C_2 = RI_F(r, SAMPLER, int2(gates_w_idx.w, hidden_index + 2));
            FLOAT4 r_C_3 = RI_F(r, SAMPLER, int2(gates_w_idx.w, hidden_index + 3));

            I = mad(h.x, r_I_0, I);
            I = mad(h.y, r_I_1, I);
            I = mad(h.z, r_I_2, I);
            I = mad(h.w, r_I_3, I);
            
            O = mad(h.x, r_O_0, O);
            O = mad(h.y, r_O_1, O);
            O = mad(h.z, r_O_2, O);
            O = mad(h.w, r_O_3, O);

            F = mad(h.x, r_F_0, F);
            F = mad(h.y, r_F_1, F);
            F = mad(h.z, r_F_2, F);
            F = mad(h.w, r_F_3, F);

            C = mad(h.x, r_C_0, C);
            C = mad(h.y, r_C_1, C);
            C = mad(h.z, r_C_2, C);
            C = mad(h.w, r_C_3, C);
        }

        I = 1.f / 1.f + exp(-I);
        F = 1.f / 1.f + exp(-F);
        O = 1.f / 1.f + exp(-O);
        C = tanh(C);

        FLOAT4 cell2 = F * cell + I * C;
        FLOAT4 H = O * tanh(cell2);
        h_local[hid_4_idx] = H;
        barrier(CLK_LOCAL_MEM_FENCE);

        WI_F(output, (int2)(hid_4_dir_idx, batch * sequence + t), H);
        cell = cell2;
    }
    WI_F(output_cell, (int2)(hid_4_idx, state_h_idx), cell);
    WI_F(output_hidden, (int2)(hid_4_idx, state_h_idx), h_local[hid_4_idx]);
}