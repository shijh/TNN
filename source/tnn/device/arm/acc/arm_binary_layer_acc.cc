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

#include "tnn/device/arm/acc/arm_binary_layer_acc.h"
#include "tnn/device/arm/arm_common.h"
#include "tnn/device/arm/arm_context.h"
#include "tnn/utils/data_format_converter.h"
#include "tnn/utils/data_type_utils.h"
#include "tnn/utils/dims_vector_utils.h"

namespace TNN_NS {

static void BroadCastTypeFilter(const DimsVector &dims_output, const DimsVector &dims_input, BroadcastType &type) {
    if (DimsVectorUtils::Equal(dims_output, dims_input)) {
        type = BroadcastTypeNormal;
        return;
    }
    if (DimsVectorUtils::Equal(dims_output, dims_input, 1)) {
        type = BroadcastTypeElement;
        return;
    }
    if (DimsVectorUtils::Equal(dims_output, dims_input, 2)) {
        type = BroadcastTypeHeightWidth;
        return;
    }
    if (DimsVectorUtils::Equal(dims_output, dims_input, 3)) {
        type = BroadcastTypeWidth;
        return;
    }
    int broadcast_count = DimsVectorUtils::Count(dims_input);
    if (broadcast_count == 1) {
        type = BroadcastTypeSingle;
    } else if (broadcast_count == dims_output[1]) {
        // broadcast dim = [1, channel, 1, 1] or [channel, 1, 1]
        int dims_size = dims_input.size();
        if (dims_size >= 3 && dims_input[dims_size - 3] == dims_output[1]) {
            type = BroadcastTypeChannel;
        } else {
            type = BroadcastTypeGeneral;
        }
    } else {
        type = BroadcastTypeGeneral;
    }
    return;
}

static void BroadCastInit(const DimsVector &dims, const DimsVector &dims0, const DimsVector &dims1, BroadcastType &type,
                          DimsVector &dims_broadcast, bool &swap_flag) {
    if (DimsVectorUtils::Equal(dims0, dims1)) {
        type = BroadcastTypeNormal;
        dims_broadcast.clear();
    } else if (DimsVectorUtils::Equal(dims0, dims1, 1)) {
        type = BroadcastTypeElement;
        dims_broadcast.clear();
        if (dims0[0] < dims1[0])
            swap_flag = true;
    } else if (DimsVectorUtils::Equal(dims0, dims1, 2)) {
        type = BroadcastTypeHeightWidth;
        dims_broadcast.clear();
        if (dims0[1] < dims1[1])
            swap_flag = true;
    } else if (DimsVectorUtils::Equal(dims0, dims1, 3)) {
        type = BroadcastTypeWidth;
        dims_broadcast.clear();
        if (dims0[1] < dims1[1])
            swap_flag = true;
    } else if (DimsVectorUtils::Equal(dims0, dims)) {
        dims_broadcast = dims1;
    } else {
        dims_broadcast = dims0;
        swap_flag      = true;
    }
}

template <typename T>
Status ArmBinaryLayerAcc::BinaryGeneralFunc(T *output_ptr, std::vector<T*> &input_ptrs, DimsVector output_shape, std::vector<DimsVector> &input_shapes) {
    size_t output_size = DimsVectorUtils::Count(output_shape);
    T *workspace = reinterpret_cast<T *>(context_->GetSharedWorkSpace(output_size * 2 * sizeof(T)));
    T *output_nchw = workspace;
    T *input_nchw = workspace + output_size;

    for (int i = 0; i < input_shapes.size(); i++) {
        auto input_shape = input_shapes[i];
        T *input_data = input_ptrs[i];

        DimsVector input_shape_pad;
        for (int j = 0; j < output_shape.size(); j++) {
            input_shape_pad.push_back(1);
        }
        for (int j = 0; j < input_shape.size(); j++) {
            input_shape_pad[input_shape_pad.size() - 1 - j] = 
                input_shape[input_shape.size() - 1 - j];
        }

        int input_batch = input_shape_pad[0];
        int input_channel = input_shape_pad[1];
        int input_hw = DimsVectorUtils::Count(input_shape_pad, 2);
        // nc4hw4 to nchw
        UnpackFloatBlob(input_nchw, input_data, input_batch, input_channel, input_hw);

        int broadcast_single = 1;
        for (int j = 0; j < input_shape_pad.size(); j++) {
            if (input_shape_pad[j] != 1) {
                broadcast_single = 0;
                break;
            }
        }

        int outer_size = 1;
        int inner_size = 1;
        int broad_size = DimsVectorUtils::Count(input_shape);

        for (int j = 0; j < input_shape_pad.size(); j++) {
            if (input_shape_pad[j] == 1) {
                outer_size *= output_shape[j];
            } else {
                break;
            }
        }

        if (!broadcast_single) {
            for (int j = 0; j < input_shape_pad.size(); j++) {
                if (input_shape_pad[input_shape_pad.size() - 1 - j] == 1) {
                    inner_size *= output_shape[output_shape.size() - 1 - j];
                } else {
                    break;
                }
            }
        }

        if (i == 0) {
            for (int o = 0; o < outer_size; o++) {
                auto output_outer = output_nchw + o * broad_size * inner_size;
                for (int b = 0; b < broad_size; b++) {
                    auto output_broad = output_outer + b * inner_size;
                    T input_broad = input_nchw[b];
                    Float4 input_vec = Float4(input_broad);
                    int j = 0;
                    for (; j + 3 < inner_size; j += 4) {
                        Float4::save(output_broad + j, input_vec);
                    }
                    for (; j < inner_size; j++) {
                        output_broad[j] = input_broad;
                    }
                }
            }
        } else {
            for (int o = 0; o < outer_size; o++) {
                auto output_outer = output_nchw + o * broad_size * inner_size;
                for (int b = 0; b < broad_size; b++) {
                    auto output_broad = output_outer + b * inner_size;
                    T input_broad = input_nchw[b];
                    Float4 input_vec = Float4(input_broad);
                    int j = 0;
                    for (; j + 3 < inner_size; j += 4) {
                        Float4 output_vec = Float4::load(output_broad + j);
                        Float4::save(output_broad + j, _Operator(output_vec, input_vec));
                    }

                    int left = inner_size - j;
                    T tmp_output[4] = {0};
                    for (int jj = 0; jj < left; jj++) {
                        tmp_output[jj] = output_broad[j + jj];
                    }
                    Float4::save(tmp_output, _Operator(Float4::load(tmp_output), input_vec));
                    for (int jj = 0; jj < left; jj++) {
                        output_broad[j + jj] = tmp_output[jj];
                    }
                }
            }
        }
    }

    int output_batch = output_shape[0];
    int output_channel = output_shape[1];
    int output_hw = DimsVectorUtils::Count(output_shape, 2);
    PackFloatBlob(output_ptr, output_nchw, output_batch, output_channel, output_hw);

    return TNN_OK;
}

/*
Binary func with different opreator,
set dims0 full shape, dims1 broadcast shape, so we need to swap input ptrs
*/
template <typename Tout, typename Tin1, typename Tin2>
Status ArmBinaryLayerAcc::BinaryFunc(Tout *output_ptr, Tin1 *input0_ptr, Tin2 *input1_ptr, DimsVector &dims0,
                                     DimsVector &dims1) {
    DimsVector dims = DimsVectorUtils::Max(dims0, dims1);
    DimsVector dims_broadcast;
    BroadcastType type = BroadcastTypeUnknown;
    auto _input0       = input0_ptr;
    auto _input1       = input1_ptr;
    bool swap_flag     = false;

    BroadCastInit(dims, dims0, dims1, type, dims_broadcast, swap_flag);

    if (swap_flag) {
        std::swap(_input0, _input1);
    }

    if (dims_broadcast.size()) {
        type = (dims_broadcast[1] == 1) ? BroadcastTypeSingle : BroadcastTypeChannel;
    }

    int count      = dims[0] * ROUND_UP(dims[1], 4) * dims[2] * dims[3];
    int count_quad = UP_DIV(count, 4);

    if (type == BroadcastTypeNormal) {
        for (int n = 0; n < count_quad; n++) {
            auto v1 = Float4::load(_input0 + n * 4);
            auto v2 = Float4::load(_input1 + n * 4);
            Float4::save(output_ptr + n * 4, _Operator(v1, v2));
        }

        return TNN_OK;
    }

    if (swap_flag) {
        if (type == BroadcastTypeSingle) {
            // broadcast single
            for (int n = 0; n < count_quad; n++) {
                auto v1 = Float4::load(_input0 + n * 4);
                auto v2 = Float4(_input1[0]);
                Float4::save(output_ptr + n * 4, _Operator(v2, v1));
            }
        } else if (type == BroadcastTypeChannel) {
            // broadcast channel
            for (int n = 0; n < count_quad; n++) {
                int b               = n / (dims[2] * dims[3] * UP_DIV(dims[1], 4));
                int channel_4_index = n / (dims[2] * dims[3]) - b * UP_DIV(dims[1], 4);
                auto v1             = Float4::load(_input0 + n * 4);
                auto v2             = Float4::load(_input1 + channel_4_index * 4);
                Float4::save(output_ptr + n * 4, _Operator(v2, v1));
            }
        } else if (type == BroadcastTypeElement) {
            // broadcast chw
            for (int n = 0; n < count_quad; n++) {
                int channel_4_index = n % (dims[2] * dims[3] * UP_DIV(dims[1], 4));
                auto v1             = Float4::load(_input0 + n * 4);
                auto v2             = Float4::load(_input1 + channel_4_index * 4);
                Float4::save(output_ptr + n * 4, _Operator(v2, v1));
            }
        } else if (type == BroadcastTypeHeightWidth) {
            // broadcast hw
            for (int n = 0; n < count_quad; n++) {
                int hw_index = n % (dims[2] * dims[3]);
                auto v1      = Float4::load(_input0 + n * 4);
                auto v2      = Float4(_input1[hw_index * 4]);
                Float4::save(output_ptr + n * 4, _Operator(v2, v1));
            }
        } else if (type == BroadcastTypeWidth) {
            // broadcast w
            for (int n = 0; n < count_quad; n++) {
                int hw_index = n % (dims[3]);
                auto v1      = Float4::load(_input0 + n * 4);
                auto v2      = Float4(_input1[hw_index * 4]);
                Float4::save(output_ptr + n * 4, _Operator(v2, v1));
            }
        } else {
            LOGE("Error: invalid add type\n");
            return Status(TNNERR_LAYER_ERR, "Error: Binary layer's unsupported broadcast type");
        }
    } else {
        if (type == BroadcastTypeSingle) {
            // broadcast single
            for (int n = 0; n < count_quad; n++) {
                auto v1 = Float4::load(_input0 + n * 4);
                auto v2 = Float4(_input1[0]);
                Float4::save(output_ptr + n * 4, _Operator(v1, v2));
            }
        } else if (type == BroadcastTypeChannel) {
            // broadcast channel
            for (int n = 0; n < count_quad; n++) {
                int b               = n / (dims[2] * dims[3] * UP_DIV(dims[1], 4));
                int channel_4_index = n / (dims[2] * dims[3]) - b * UP_DIV(dims[1], 4);
                auto v1             = Float4::load(_input0 + n * 4);
                auto v2             = Float4::load(_input1 + channel_4_index * 4);
                Float4::save(output_ptr + n * 4, _Operator(v1, v2));
            }
        } else if (type == BroadcastTypeElement) {
            // broadcast chw
            for (int n = 0; n < count_quad; n++) {
                int channel_4_index = n % (dims[2] * dims[3] * UP_DIV(dims[1], 4));
                auto v1             = Float4::load(_input0 + n * 4);
                auto v2             = Float4::load(_input1 + channel_4_index * 4);
                Float4::save(output_ptr + n * 4, _Operator(v1, v2));
            }
        } else if (type == BroadcastTypeHeightWidth) {
            // broadcast hw
            for (int n = 0; n < count_quad; n++) {
                int hw_index = n % (dims[2] * dims[3]);
                auto v1      = Float4::load(_input0 + n * 4);
                auto v2      = Float4(_input1[hw_index * 4]);
                Float4::save(output_ptr + n * 4, _Operator(v1, v2));
            }
        } else if (type == BroadcastTypeWidth) {
            // broadcast w
            for (int n = 0; n < count_quad; n++) {
                int hw_index = n % (dims[3]);
                auto v1      = Float4::load(_input0 + n * 4);
                auto v2      = Float4(_input1[hw_index * 4]);
                Float4::save(output_ptr + n * 4, _Operator(v1, v2));
            }
        } else {
            LOGE("Error: invalid add type\n");
            return Status(TNNERR_LAYER_ERR, "Error: Binary layer's unsupported broadcast type");
        }
    }

    return TNN_OK;
}

Status ArmBinaryLayerAcc::Init(Context *context, LayerParam *param, LayerResource *resource,
                               const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    RETURN_ON_NEQ(ArmLayerAcc::Init(context, param, resource, inputs, outputs), TNN_OK);

    return allocateBufferParam(inputs, outputs);
}

// SUPPORTED DATATYPES
bool ArmBinaryLayerAcc::DataTypeSupported(DataType data_type) {
    if (data_type == DATA_TYPE_FLOAT)
        return true;
    else
        return false;
}

ArmBinaryLayerAcc::~ArmBinaryLayerAcc() {}

Status ArmBinaryLayerAcc::allocateBufferParam(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    auto layer_param = dynamic_cast<MultidirBroadcastLayerParam *>(param_);
    CHECK_PARAM_NULL(layer_param);

    auto layer_res = dynamic_cast<EltwiseLayerResource *>(resource_);

    if (layer_res && broadcast_.GetBytesSize() == 0) {
        RawBuffer element_handle = layer_res->element_handle;
        auto dims                = layer_res->element_shape;
        if (element_handle.GetDataType() == DATA_TYPE_HALF)
            element_handle = ConvertHalfHandle(element_handle);

        auto layer_res_size = element_handle.GetDataCount();
        auto data_byte_size = DataTypeUtils::GetBytesSize(element_handle.GetDataType());
        auto layer_data     = element_handle.force_to<void *>();
        if (element_handle.GetDataType() == DATA_TYPE_FLOAT) {
            if (layer_res_size == 1) {
                // broadcast single, just memcpy
                RawBuffer temp(layer_res_size * data_byte_size);
                memcpy(temp.force_to<void *>(), layer_data, layer_res_size * data_byte_size);
                broadcast_ = temp;
            } else {
                // pack bias from nchw to nc4hw4
                int count = dims[0] * ROUND_UP(dims[1], 4) * dims[2] * dims[3];
                RawBuffer temp(count * data_byte_size);
                DataFormatConverter::ConvertFromNCHWToNCHW4Float(
                    static_cast<float *>(layer_data), temp.force_to<float *>(), dims[0], dims[1], dims[2], dims[3]);
                broadcast_ = temp;
            }

            if (outputs[0]->GetBlobDesc().data_type == DATA_TYPE_BFP16) {
                RawBuffer bfp16_temp(broadcast_.GetBytesSize() / 2);
                bfp16_temp.SetDataType(DATA_TYPE_BFP16);
                auto src = broadcast_.force_to<float *>();
                auto dst = bfp16_temp.force_to<bfp16_t *>();
                if (broadcast_.GetDataCount() == 1) {
                    dst[0] = src[0];
                } else {
                    FloatConvert(src, dst, broadcast_.GetDataCount() / 4);
                }
            }
        } else {
            // Todo
        }
    }

    return TNN_OK;
}

template <typename T>
Status ArmBinaryLayerAcc::Exec(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    auto layer_param = dynamic_cast<MultidirBroadcastLayerParam *>(param_);
    CHECK_PARAM_NULL(layer_param);
    auto layer_res = dynamic_cast<EltwiseLayerResource *>(resource_);
    if (!layer_res && broadcast_.GetBytesSize() > 0) {
        LOGE("Error: layer param is nil\n");
        return Status(TNNERR_PARAM_ERR, "Error: layer param is nil");
    }

    std::vector<T *> input_ptrs;
    std::vector<DimsVector> input_shapes;
    input_ptrs.reserve(4);
    input_shapes.reserve(4);
    auto output = outputs[0];
    auto dims   = output->GetBlobDesc().dims;

    if (broadcast_.GetBytesSize() > 0) {
        DimsVector input_shape0 = inputs[0]->GetBlobDesc().dims;
        // prepare input ptrs and shapes
        if (layer_param->weight_input_index == 0) {
            // bias as another input
            input_ptrs.push_back(broadcast_.force_to<T *>());
            input_shapes.push_back(layer_res->element_shape);

            input_ptrs.push_back(reinterpret_cast<T *>(GetBlobHandlePtr(inputs[0]->GetHandle())));
            input_shapes.push_back(input_shape0);
        } else {
            input_ptrs.push_back(reinterpret_cast<T *>(GetBlobHandlePtr(inputs[0]->GetHandle())));
            input_shapes.push_back(input_shape0);

            input_ptrs.push_back(broadcast_.force_to<T *>());
            input_shapes.push_back(layer_res->element_shape);
        }
    } else {
        if (inputs.size() == 1) {
            input_ptrs.push_back(reinterpret_cast<T *>(GetBlobHandlePtr(inputs[0]->GetHandle())));
            input_ptrs.push_back(reinterpret_cast<T *>(GetBlobHandlePtr(inputs[0]->GetHandle())));
            input_shapes.push_back(inputs[0]->GetBlobDesc().dims);
            input_shapes.push_back(inputs[0]->GetBlobDesc().dims);
        } else {
            for (size_t inid = 0; inid < inputs.size(); inid++) {
                input_ptrs.push_back(reinterpret_cast<T *>(GetBlobHandlePtr(inputs[inid]->GetHandle())));
                input_shapes.push_back(inputs[inid]->GetBlobDesc().dims);
            }
        }
    }

    BroadcastType btype = BroadcastTypeUnknown;
    // check broadcast type is general or other optimized ncxhwx types
    // if type is general, go to nchw general impl
    for (int i = 0; i < input_shapes.size(); i++) {
        BroadCastTypeFilter(dims, input_shapes[i], btype);
        if (btype == BroadcastTypeGeneral) {
            break;
        }
    }

    if (btype == BroadcastTypeUnknown) {
        LOGE("Error: unknown broadcast type\n");
        return Status(TNNERR_LAYER_ERR, "Error: Binary layer's unknown broadcast type");
    } else if (btype == BroadcastTypeGeneral) {
        auto output_ptr = reinterpret_cast<T *>(GetBlobHandlePtr(output->GetHandle()));
        BinaryGeneralFunc<T>(output_ptr, input_ptrs, dims, input_shapes);
    } else {
        auto output_ptr = reinterpret_cast<T *>(GetBlobHandlePtr(output->GetHandle()));
        auto input0_ptr = reinterpret_cast<T *>(input_ptrs[0]);
        auto input1_ptr = reinterpret_cast<T *>(input_ptrs[1]);

        BinaryFunc(output_ptr, input0_ptr, input1_ptr, input_shapes[0], input_shapes[1]);

        for (int i = 2; i < input_ptrs.size(); i++) {
            auto input_ptr = reinterpret_cast<T *>(input_ptrs[i]);
            BinaryFunc(output_ptr, output_ptr, input_ptr, dims, input_shapes[i]);
        }
    }

    return TNN_OK;
}

Status ArmBinaryLayerAcc::DoForward(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    auto data_type = outputs[0]->GetBlobDesc().data_type;
    if (data_type == DATA_TYPE_FLOAT) {
        return Exec<float>(inputs, outputs);
    } else if (data_type == DATA_TYPE_BFP16) {
        return Exec<bfp16_t>(inputs, outputs);
    } else {
        return TNNERR_LAYER_ERR;
    }
}

}  // namespace TNN_NS
