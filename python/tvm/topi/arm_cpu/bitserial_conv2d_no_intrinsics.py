# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
# pylint: disable=invalid-name,unused-variable,invalid-name,unused-argument
"""Bitserial conv2d schedule on arm cpu"""
from __future__ import absolute_import as _abs
import tvm
from tvm import te
from tvm import autotvm
from ..nn.pad import pad
from ..nn.bitserial_util import bitpack, binary_op_multiplier
from ..nn.utils import get_pad_tuple
from ..utils import get_const_int, get_const_tuple, traverse_inline
from .bitserial_conv2d import _kernel_vec_spatial_pack_nhwc


@autotvm.register_topi_compute("bitserial_conv2d_nhwc_no_intrinsics.arm_cpu")
def bitserial_conv2d_nhwc_no_intrinsics(
    cfg,
    data,
    kernel,
    stride,
    padding,
    activation_bits,
    weight_bits,
    pack_dtype,
    out_dtype,
    unipolar,
):
    """Compute convolution with pack on spatial axes."""
    assert data.shape[0].value == 1, "spatial pack convolution only support batch size=1"
    assert pack_dtype == "uint8", "only support packing into uint8 bits"
    assert out_dtype == "int16", "only support output type of int16"

    N, H, W, CI = get_const_tuple(data.shape)
    if len(kernel.shape) == 4:
        KH, KW, _, CO = get_const_tuple(kernel.shape)
        CI_packed = CI // 8
    else:
        KH, KW, KB, CI_packed, CO = get_const_tuple(kernel.shape)

    if isinstance(padding, int) or (isinstance(padding, (tuple, list)) and len(padding) == 2):
        TPAD, LPAD, DPAD, RPAD = get_pad_tuple(padding, kernel)
    else:
        TPAD, LPAD, DPAD, RPAD = padding

    if isinstance(stride, (tuple, list)):
        HSTR, WSTR = stride
    else:
        HSTR, WSTR = stride, stride
    HCAT, WCAT = KH - 1, KW - 1

    PAD_H = H + (TPAD + DPAD)
    PAD_W = W + (LPAD + RPAD)
    OH = (PAD_H - KH) // HSTR + 1
    OW = (PAD_W - KW) // WSTR + 1
    oshape = (1, OH, OW, CO)

    idxd = tvm.tir.indexdiv
    idxm = tvm.tir.indexmod

    # Pad input channels of weights and data when it is not a multiple of 8
    if CI_packed % 8 != 0:
        CI_PAD = CI_packed % 8
        CI_packed += CI_PAD
    else:
        CI_PAD = 0

    # ==================== define configuration space ====================
    n, oh, ow, co = cfg.axis(N), cfg.axis(OH), cfg.axis(OW), cfg.axis(CO)
    ci, kh, kw = cfg.reduce_axis(CI_packed), cfg.reduce_axis(KH), cfg.reduce_axis(KW)
    ib, kb = cfg.reduce_axis(activation_bits), cfg.reduce_axis(weight_bits)

    co, vc = cfg.define_split("tile_co", co, num_outputs=2, filter=lambda x: x.size[-1] == 8)
    oh, vh = cfg.define_split("tile_oh", oh, num_outputs=2, filter=lambda x: x.size[-1] >= 2)
    ow, vw = cfg.define_split("tile_ow", ow, num_outputs=2, filter=lambda x: x.size[-1] >= 2)
    ci_o, ci_i = cfg.define_split(
        "tile_ci", ci, num_outputs=2, filter=lambda x: x.size[-1] == 8 or x.size[-1] == 16
    )
    re_axes = cfg.define_reorder(
        "reorder_0",
        [n, oh, ow, co, vh, vw, kh, kw, ci_o, kb, ib, vc, ci_i],
        policy="candidate",
        candidate=[
            [n, oh, ow, co, vh, vw, kh, kw, ci_o, kb, ib, vc, ci_i],
            [n, oh, ow, co, vh, vw, kw, kh, ci_o, kb, ib, vc, ci_i],
        ],
    )
    # binary ops
    cfg.add_flop(2 * N * OH * OW * CO * CI * KH * KW * binary_op_multiplier(pack_dtype))
    # ====================

    VC = cfg["tile_co"].size[-1]
    VH = cfg["tile_oh"].size[-1]
    VW = cfg["tile_ow"].size[-1]

    data_q = bitpack(data, activation_bits, pack_axis=3, bit_axis=3, pack_type="uint8")

    kernel_vec = _kernel_vec_spatial_pack_nhwc(kernel, weight_bits, VC, len(kernel.shape) == 4)
    idxm = tvm.tir.indexmod
    if idxm(kernel_vec.shape[-1], 8) != 0 and CI_PAD != 0:
        kernel_vec = pad(kernel_vec, [0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, CI_PAD])

    N, H, W, IB, CI = data_q.shape
    OCO, KH, KW, KB, VC, CI = kernel_vec.shape

    dvshape = (
        N,
        PAD_H // (VH * HSTR),
        PAD_W // (VW * WSTR),
        VH * HSTR + HCAT,
        VW * WSTR + WCAT,
        IB,
        CI,
    )
    ovshape = (1, OH // VH, OW // VW, CO // VC, VH, VW, VC)

    if TPAD != 0 and RPAD != 0:
        data_pad = pad(data_q, (0, TPAD, LPAD, 0, 0), (0, DPAD, RPAD, 0, CI_PAD), name="data_pad")
    elif CI_PAD != 0:
        data_pad = pad(data_q, (0, 0, 0, 0, 0), (0, 0, 0, 0, CI_PAD), name="data_pad")
    else:
        data_pad = data_q

    data_vec = te.compute(
        dvshape,
        lambda n, h, w, vh, vw, b, ci: data_pad[n][h * VH * HSTR + vh][w * VW * WSTR + vw][b][ci],
        name="data_vec",
    )
    ci = te.reduce_axis((0, CI), name="ci")
    dh = te.reduce_axis((0, KH), name="dh")
    dw = te.reduce_axis((0, KW), name="dw")
    ib = te.reduce_axis((0, IB), name="ib")
    kb = te.reduce_axis((0, KB), name="kb")

    def _bipolar_conv(n, h, w, co, vh, vw, vc):
        return te.sum(
            (
                tvm.tir.popcount(
                    kernel_vec[co, dh, dw, kb, vc, ci].astype("uint16")
                    & data_vec[n, h, w, vh * HSTR + dh, vw * WSTR + dw, ib, ci].astype("uint16")
                )
                << (kb + ib).astype("uint16")
            ),
            axis=[dh, dw, kb, ib, ci],
        )

    def _unipolar_conv(n, h, w, co, vh, vw, vc):
        return te.sum(
            (
                (
                    tvm.tir.popcount(
                        kernel_vec[co, dh, dw, kb, vc, ci].astype("int16")
                        & data_vec[n, h, w, vh * HSTR + dh, vw * WSTR + dw, ib, ci].astype("int16")
                    )
                    - tvm.tir.popcount(
                        ~kernel_vec[co, dh, dw, kb, vc, ci].astype("int16")
                        & data_vec[n, h, w, vh * HSTR + dh, vw * WSTR + dw, ib, ci]
                    ).astype("int16")
                )
                << (kb + ib).astype("int16")
            ),
            axis=[dh, dw, kb, ib, ci],
        )

    if unipolar:
        conv_vec = te.compute(ovshape, _unipolar_conv, name="conv_vec", tag="unipolar")
    else:
        conv_vec = te.compute(ovshape, _bipolar_conv, name="conv_vec", tag="bipolar")

    conv = te.compute(
        oshape,
        lambda n, h, w, co: conv_vec[
            n, idxd(h, VH), idxd(w, VW), idxd(co, VC), idxm(h, VH), idxm(w, VW), idxm(co, VC)
        ].astype(out_dtype),
        name="conv",
        tag="spatial_bitserial_conv_nhwc",
    )

    return conv


# ARM specific schedule that is not using arm32 custom microkernel
def _schedule_spatial_conv2d_nhwc_no_intrinsics(
    cfg, s, data_pad, data_vec, kernel_vec, conv_out, output, last, unipolar
):
    _, _, _, _, _, IB, CI = data_vec.shape
    _, KH, KW, KB, _, _ = kernel_vec.shape
    KB = get_const_int(KB)
    IB = get_const_int(IB)

    VC = cfg["tile_co"].size[-1]
    VH = cfg["tile_oh"].size[-1]
    VW = cfg["tile_ow"].size[-1]

    ##### Schedule data padding and  packing
    if data_pad is not None:
        s[data_pad].compute_inline()

    _, h, _, _, _, _, _ = s[data_vec].op.axis
    cfg.define_split("tile_ah", cfg.axis(h), num_outputs=2, max_factor=32)
    oh, ih = cfg["tile_ah"].apply(s, data_vec, h)
    s[data_vec].parallel(oh)

    #### Schedule kernel packing
    co, _, _, _, _, _ = s[kernel_vec].op.axis
    cfg.define_split("tile_bco", cfg.axis(co), num_outputs=2, max_factor=32)
    oco, ico = cfg["tile_bco"].apply(s, kernel_vec, co)
    s[kernel_vec].parallel(oco)

    ##### Schedule Convolution
    n, oh, ow, co, vh, vw, vc = s[conv_out].op.axis
    kh, kw, kb, ib, ci = s[conv_out].op.reduce_axis

    ci_o, ci_i = cfg["tile_ci"].apply(s, conv_out, ci)
    re_axes = cfg["reorder_0"].apply(
        s, conv_out, [n, oh, ow, co, vh, vw, kh, kw, ci_o, kb, ib, vc, ci_i]
    )

    n, h, w, co = s[last].op.axis
    co, vc = cfg["tile_co"].apply(s, last, co)
    oh, vh = cfg["tile_oh"].apply(s, last, h)
    ow, vw = cfg["tile_ow"].apply(s, last, w)
    s[last].reorder(n, oh, ow, co, vh, vw, vc)
    s[last].vectorize(vc)
    if last != output:
        s[output].compute_inline()

    s[conv_out].compute_at(s[last], co)
    s[last].parallel(oh)
    return s


@autotvm.register_topi_schedule("bitserial_conv2d_nhwc_no_intrinsics.arm_cpu")
def schedule_bitserial_conv2d_nhwc_no_intrinsics(cfg, outs):
    """Arm cpu schedule for bitserial conv2d without the arm32 intrinsics"""
    s = te.create_schedule([x.op for x in outs])

    def _callback(op):
        if "spatial_bitserial_conv_nhwc" in op.tag:
            output = op.output(0)
            conv_out = op.input_tensors[0]
            kernel_vec = conv_out.op.input_tensors[0]
            data_vec = conv_out.op.input_tensors[1]
            data_q = data_vec.op.input_tensors[0]
            data = data_q.op.input_tensors[0]
            data_pad = None
            if isinstance(data_q.op, te.tensor.ComputeOp) and "pad" in data_q.op.tag:
                data_pad = data_q
                data_q = data
                data = data.op.input_tensors[0]
            unipolar = "unipolar" in conv_out.op.tag
            _schedule_spatial_conv2d_nhwc_no_intrinsics(
                cfg, s, data_pad, data_vec, kernel_vec, conv_out, output, outs[0], unipolar
            )

    traverse_inline(s, outs[0].op, _callback)
    return s