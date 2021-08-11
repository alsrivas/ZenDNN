﻿/*******************************************************************************
* Modifications Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
* Notified per clause 4(b) of the license.
*******************************************************************************/

/*******************************************************************************
* Copyright 2019-2020 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_X64_JIT_AVX512_CORE_BF16_1X1_CONVOLUTION_HPP
#define CPU_X64_JIT_AVX512_CORE_BF16_1X1_CONVOLUTION_HPP

#include "common/c_types_map.hpp"
#include "common/zendnn_thread.hpp"
#include "common/primitive.hpp"
#include "common/primitive_hashing.hpp"
#include "common/utils.hpp"

#include "cpu/cpu_convolution_pd.hpp"
#include "cpu/cpu_engine.hpp"
#include "cpu/dw_convolution_utils.hpp"
#include "cpu/platform.hpp"

#include "cpu/x64/cpu_reducer.hpp"
#include "cpu/x64/jit_avx512_core_bf16_1x1_conv_kernel.hpp"
#include "cpu/x64/jit_transpose_src_utils.hpp"
#include "cpu/x64/jit_uni_1x1_conv_utils.hpp"
#include "cpu/x64/jit_uni_dw_convolution.hpp"

namespace zendnn {
namespace impl {
namespace cpu {
namespace x64 {

template <impl::data_type_t dst_type>
struct jit_avx512_core_bf16_1x1_convolution_fwd_t : public primitive_t {
    struct pd_t : public cpu_convolution_fwd_pd_t {
        using dw_conv_pd_type = cpu_convolution_fwd_pd_t;
        pd_t(const convolution_desc_t *adesc, const primitive_attr_t *attr,
                const typename pd_t::base_class *hint_fwd_pd)
            : cpu_convolution_fwd_pd_t(adesc, attr, hint_fwd_pd)
            , jcp_()
            , rtus_()
            , jcp_dw_(nullptr) {}

        pd_t(const pd_t &other) : cpu_convolution_fwd_pd_t(other) {
            if (copy(other) != status::success) is_initialized_ = false;
        }

        DECLARE_COMMON_PD_T(JIT_IMPL_NAME_HELPER("jit_bf16_1x1:", jcp_.isa, ""),
                jit_avx512_core_bf16_1x1_convolution_fwd_t);

        status_t init(engine_t *engine) {
            bool ok = true && mayiuse(avx512_core) && is_fwd()
                    && set_default_alg_kind(alg_kind::convolution_direct)
                    && expect_data_types(data_type::bf16, data_type::bf16,
                            data_type::undef, dst_type, data_type::undef)
                    && IMPLICATION(with_bias(),
                            utils::one_of(weights_md(1)->data_type,
                                    data_type::f32, data_type::bf16))
                    && attr()->has_default_values(
                            primitive_attr_t::skip_mask_t::post_ops, dst_type)
                    && !has_zero_dim_memory() && set_default_formats();

            if (!ok) return status::unimplemented;

            const convolution_desc_t *conv_d = desc();
            const memory_desc_t *src_d = src_md();
            rtus_prepare(this, conv_d, src_d, dst_md(), weights_md());

            status_t status = jit_avx512_core_bf16_1x1_conv_kernel::init_conf(
                    jcp_, *conv_d, *src_d, *weights_md(), *dst_md(), *attr(),
                    zendnn_get_max_threads(), rtus_.reduce_src_);
            if (status != status::success) return status;

            if (jcp_.with_dw_conv) {
                status = depthwise_po_init(engine);
                if (status != status::success) return status;
            }

            auto scratchpad = scratchpad_registry().registrar();
            status = jit_avx512_core_bf16_1x1_conv_kernel::init_scratchpad(
                    scratchpad, jcp_);
            if (status != status::success) return status;

            rtus_prepare_space_info(this, scratchpad, jcp_.nthr);

            return status::success;
        }

        const memory_desc_t *dst_md(int index = 0) const override {
            return jcp_.with_dw_conv ? dw_conv_pd_->dst_md(index) : &dst_md_;
        }

        const memory_desc_t *arg_md(int index = 0) const override {
            if (jcp_.with_dw_conv) {
                switch (index) {
                    case ZENDNN_ARG_ATTR_POST_OP_DW | ZENDNN_ARG_WEIGHTS:
                        return dw_conv_pd_->weights_md(0);
                    case ZENDNN_ARG_ATTR_POST_OP_DW | ZENDNN_ARG_BIAS:
                        return dw_conv_pd_->weights_md(1);
                    default: break;
                }
            }
            return convolution_fwd_pd_t::arg_md(index);
        }

        arg_usage_t arg_usage(int arg) const override {
            if (arg == (ZENDNN_ARG_ATTR_POST_OP_DW | ZENDNN_ARG_WEIGHTS))
                return arg_usage_t::input;

            if (arg == (ZENDNN_ARG_ATTR_POST_OP_DW | ZENDNN_ARG_BIAS)
                    && attr_post_op_dw_inputs() > 1)
                return arg_usage_t::input;

            return convolution_fwd_pd_t::arg_usage(arg);
        }

        jit_1x1_conv_conf_t jcp_;
        reduce_to_unit_stride_t rtus_;
        jit_conv_conf_t *jcp_dw_; // doesn't own a resource
        std::unique_ptr<cpu_convolution_fwd_pd_t> dw_conv_pd_;

    protected:
        template <data_type_t ddt>
        using dw_pd_t = typename jit_uni_dw_convolution_fwd_t<avx512_core,
                data_type::bf16, ddt>::pd_t;

        bool set_default_formats() {
            using namespace format_tag;
            auto dat_tag = utils::pick(ndims() - 3, nCw16c, nChw16c, nCdhw16c);
            auto wei_tag = utils::pick(2 * ndims() - 6 + with_groups(),
                    OIw8i16o2i, gOIw8i16o2i, OIhw8i16o2i, gOIhw8i16o2i,
                    OIdhw8i16o2i, gOIdhw8i16o2i);

            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }

        status_t copy(const pd_t &other) {
            jcp_ = other.jcp_;
            rtus_ = other.rtus_;
            jcp_dw_ = nullptr;
            using namespace data_type;
            if (other.dw_conv_pd_) {
                dw_conv_pd_.reset(static_cast<cpu_convolution_fwd_pd_t *>(
                        other.dw_conv_pd_->clone()));
                if (!dw_conv_pd_) return status::out_of_memory;
                auto dw_dst_dt = dw_conv_pd_->dst_md()->data_type;

                switch (dw_dst_dt) {
                    case bf16:
                        jcp_dw_ = &(
                                static_cast<dw_pd_t<bf16> *>(dw_conv_pd_.get())
                                        ->jcp_);
                        break;
                    case f32:
                        jcp_dw_ = &(
                                static_cast<dw_pd_t<f32> *>(dw_conv_pd_.get())
                                        ->jcp_);
                        break;
                    default: assert(!"unreachable");
                }
            }
            return status::success;
        }

        status_t depthwise_po_init(engine_t *engine) {
            using namespace memory_tracking;
            auto &jcp_1x1 = jcp_;
            jit_conv_conf_t *jcp_dw = nullptr;
            primitive_attr_t attr_1x1(*attr());
            if (!attr_1x1.is_initialized()) return status::out_of_memory;
            attr_1x1.set_scratchpad_mode(scratchpad_mode::user);

            const auto &src_md = dst_md_;
            const memory_desc_wrapper src_d(src_md);
            const auto nthr = zendnn_get_max_threads();
            auto l2_cache = platform::get_per_core_cache_size(2) * nthr;

            // Note: A robust fusion implementation would be to check if both
            // 1x1 conv and dw conv that are considered here for fusion are
            // optimal independently. This would require creating a new
            // primitive_desc through primitive_iterator & check if they match.
            // Due to concern that these creations and/or checks could be heavy,
            // for 1x1: Check that no better ISA is available.
            // for dw: Always fuse with same ISA.
            // Caveat: May be a better dw conv exists.

            bool ok = !mayiuse(avx512_core_bf16_amx_bf16)
                    && (attr_1x1.post_ops_.find(primitive_kind::sum) == -1)
                    // TODO: Below may be further tuned.
                    && (l2_cache * 2 < src_d.size())
                    // load_grp_count check can be redundant due to l2 check
                    // above. Adding it explicitly as the current driver doesn't
                    // work if this condition fails.
                    && (jcp_1x1.load_grp_count < 2);
            if (!ok) return status::unimplemented;

            int dw_po_index
                    = attr_1x1.post_ops_.find(primitive_kind::convolution);

            convolution_desc_t cd_dw;
            primitive_attr_t attr_dw;
            CHECK(get_depthwise_conv_desc(
                    cd_dw, src_md, attr_1x1, attr_dw, dw_po_index));

            auto dw_dst_dt = cd_dw.dst_desc.data_type;

#define CASE(dt) \
    case dt: { \
        std::unique_ptr<dw_pd_t<dt>> fusable_pd( \
                new dw_pd_t<dt>(&cd_dw, &attr_dw, nullptr)); \
        CHECK(fusable_pd->init(engine)); \
        jcp_dw = &(fusable_pd->jcp_); \
        dw_conv_pd_ = std::move(fusable_pd); \
        break; \
    }
            if (jcp_1x1.dst_dt == data_type::bf16) {
                switch (dw_dst_dt) {
                    CASE(data_type::bf16);
                    CASE(data_type::f32);
                    default: return status::unimplemented;
                }
            } else
                return status::unimplemented;
#undef CASE

            ok = true
                    && (zendnn_memory_desc_equal(&src_md, dw_conv_pd_->src_md(0)))
                    && (jcp_1x1.oc_without_padding % jcp_1x1.oc_block == 0)
                    && IMPLICATION(
                            jcp_dw->ow_block, jcp_dw->ow_block == jcp_dw->ow);
            if (!ok) return status::unimplemented;

            assert(dw_conv_pd_->dst_md(0)->format_kind != format_kind::any);
            assert(dw_conv_pd_->weights_md(0)->format_kind != format_kind::any);
            assert(IMPLICATION(
                    dw_conv_pd_->weights_md(1)->data_type != data_type::undef,
                    dw_conv_pd_->weights_md(1)->format_kind
                            != format_kind::any));

            jcp_dw->is_fused_conv = true;
            // TODO: Support/experiment arbitary oc_work in dw conv.
            // Until then we keep ch_work perfectly divisible.
            while (jcp_1x1.nb_load % jcp_1x1.nb_load_blocking != 0)
                --jcp_1x1.nb_load_blocking;
            jcp_1x1.nb_load_blocking_max = jcp_1x1.nb_load_blocking;

            while (jcp_1x1.nb_load_blocking % jcp_dw->nb_ch_blocking != 0)
                --jcp_dw->nb_ch_blocking;

            jcp_dw->dw_conv_buffer_oc
                    = jcp_1x1.nb_load_blocking * jcp_1x1.oc_block;

            registrar_t scratchpad(scratchpad_registry_);
            registrar_t dw_scratchpad(scratchpad, names::prefix_fusion);

            size_t dw_conv_buffer_size_ = (size_t)nthr * jcp_dw->kh * jcp_dw->iw
                    * jcp_dw->dw_conv_buffer_oc;
            assert(dw_conv_buffer_size_);
            dw_scratchpad.book(memory_tracking::names::key_fusion_inout_buffer,
                    dw_conv_buffer_size_,
                    types::data_type_size(dw_conv_pd_->src_md()->data_type));

            dw_conv_kernel_t::init_scratchpad(dw_scratchpad, *jcp_dw);

            return status::success;
        }
    };

    template <cpu_isa_t isa, typename conv_t>
    friend status_t init_rtus_driver(conv_t *self);
    jit_avx512_core_bf16_1x1_convolution_fwd_t(const pd_t *apd)
        : primitive_t(apd) {}

    typedef typename prec_traits<data_type::bf16>::type src_data_t;
    typedef typename prec_traits<data_type::bf16>::type wei_data_t;
    typedef typename prec_traits<dst_type>::type dst_data_t;
    // Note: In case of fused depthwise convolution, the final output datatype
    // may not be dst_data_t.
    typedef typename prec_traits<dst_type>::type dw_wei_data_t;

    status_t init(engine_t *engine) override {
        CHECK(safe_ptr_assign(kernel_,
                new jit_avx512_core_bf16_1x1_conv_kernel(
                        pd()->jcp_, *pd()->attr(), *pd()->dst_md(0))));
        CHECK(kernel_->create_kernel());

        if (pd()->jcp_.with_dw_conv) {
            CHECK(safe_ptr_assign(kernel_dw_,
                    new dw_conv_kernel_t(*(pd()->jcp_dw_), *pd()->dst_md(0))));
            CHECK(kernel_dw_->create_kernel());
        }

        CHECK(init_rtus_driver<avx512_common>(this));
        return status::success;
    }

    status_t execute(const exec_ctx_t &ctx) const override {
        execute_forward(ctx);
        return status::success;
    }

private:
    void execute_forward(const exec_ctx_t &ctx) const;
    void execute_forward_thr(const int ithr, const int nthr,
            const src_data_t *src, const wei_data_t *weights, const char *bias,
            const dw_wei_data_t *weights_dw, const float *bias_dw,
            const char *dst, const memory_tracking::grantor_t &scratchpad,
            const void *post_ops_binary_rhs_arg_vec,
            const void *post_ops_binary_rhs_arg_vec_dw) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }

    std::unique_ptr<jit_avx512_core_bf16_1x1_conv_kernel> kernel_;
    std::unique_ptr<rtus_driver_t<avx512_common>> rtus_driver_;
    using dw_conv_kernel_t
            = jit_uni_dw_conv_fwd_kernel<avx512_core, data_type::bf16>;
    std::unique_ptr<dw_conv_kernel_t> kernel_dw_;
};

template <impl::data_type_t diff_src_type>
struct jit_avx512_core_bf16_1x1_convolution_bwd_data_t : public primitive_t {
    struct pd_t : public cpu_convolution_bwd_data_pd_t {
        pd_t(const convolution_desc_t *adesc, const primitive_attr_t *attr,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_data_pd_t(adesc, attr, hint_fwd_pd)
            , jcp_()
            , rtus_() {}

        DECLARE_COMMON_PD_T(JIT_IMPL_NAME_HELPER("jit_bf16_1x1:", jcp_.isa, ""),
                jit_avx512_core_bf16_1x1_convolution_bwd_data_t);

        status_t init(engine_t *engine) {
            bool ok = true && mayiuse(avx512_core) && is_bwd_d()
                    && set_default_alg_kind(alg_kind::convolution_direct)
                    && expect_data_types(diff_src_type, data_type::bf16,
                            data_type::undef, data_type::bf16, data_type::undef)
                    && attr()->has_default_values() && !has_zero_dim_memory()
                    && set_default_formats();
            if (!ok) return status::unimplemented;

            const convolution_desc_t *conv_d = desc();
            const memory_desc_t *diff_src_d = diff_src_md();
            rtus_prepare(this, conv_d, diff_src_d, diff_dst_md(), weights_md());

            status_t status = jit_avx512_core_bf16_1x1_conv_kernel::init_conf(
                    jcp_, *conv_d, *diff_src_d, *weights_md(), *diff_dst_md(),
                    *attr(), zendnn_get_max_threads(), rtus_.reduce_src_);
            if (status != status::success) return status;

            auto scratchpad = scratchpad_registry().registrar();
            status = jit_avx512_core_bf16_1x1_conv_kernel::init_scratchpad(
                    scratchpad, jcp_);
            if (status != status::success) return status;
            rtus_prepare_space_info(this, scratchpad, jcp_.nthr);

            return status::success;
        }

        // TODO (Roma): structs conf header cleanup
        jit_1x1_conv_conf_t jcp_;
        reduce_to_unit_stride_t rtus_;

    protected:
        bool set_default_formats() {
            using namespace format_tag;

            auto dat_tag = utils::pick(ndims() - 3, nCw16c, nChw16c, nCdhw16c);
            auto wei_tag = utils::pick(2 * ndims() - 6 + with_groups(),
                    IOw8o16i2o, gIOw8o16i2o, IOhw8o16i2o, gIOhw8o16i2o,
                    IOdhw8o16i2o, gIOdhw8o16i2o);

            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }
    };

    template <cpu_isa_t isa, typename conv_t>
    friend status_t init_rtus_driver(conv_t *self);

    jit_avx512_core_bf16_1x1_convolution_bwd_data_t(const pd_t *apd)
        : primitive_t(apd) {}

    typedef typename prec_traits<data_type::bf16>::type diff_dst_data_t;
    typedef typename prec_traits<data_type::bf16>::type wei_data_t;
    typedef typename prec_traits<diff_src_type>::type diff_src_data_t;

    status_t init(engine_t *engine) override {
        CHECK(safe_ptr_assign(kernel_,
                new jit_avx512_core_bf16_1x1_conv_kernel(
                        pd()->jcp_, *pd()->attr(), *pd()->dst_md(0))));
        CHECK(kernel_->create_kernel());
        CHECK(init_rtus_driver<avx512_common>(this));
        return status::success;
    }

    status_t execute(const exec_ctx_t &ctx) const override {
        execute_backward_data(ctx);
        return status::success;
    }

private:
    void execute_backward_data(const exec_ctx_t &ctx) const;
    void execute_backward_data_thr(const int, const int,
            const diff_dst_data_t *, const wei_data_t *, diff_src_data_t *,
            const memory_tracking::grantor_t &scratchpad) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }

    std::unique_ptr<jit_avx512_core_bf16_1x1_conv_kernel> kernel_;
    /* reduction to unit stride */
    std::unique_ptr<rtus_driver_t<avx512_common>> rtus_driver_;
};

template <impl::data_type_t diff_weights_type>
struct jit_avx512_core_bf16_1x1_convolution_bwd_weights_t : public primitive_t {
    struct pd_t : public cpu_convolution_bwd_weights_pd_t {
        pd_t(const convolution_desc_t *adesc, const primitive_attr_t *attr,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_weights_pd_t(adesc, attr, hint_fwd_pd)
            , jcp_()
            , rtus_() {}

        DECLARE_COMMON_PD_T(JIT_IMPL_NAME_HELPER("jit_bf16_1x1:", jcp_.isa, ""),
                jit_avx512_core_bf16_1x1_convolution_bwd_weights_t);

        status_t init(engine_t *engine) {
            using namespace prop_kind;
            assert(engine->kind() == engine_kind::cpu);
            bool ok = true && mayiuse(avx512_core) && is_bwd_w()
                    && set_default_alg_kind(alg_kind::convolution_direct)
                    && expect_data_types(data_type::bf16, diff_weights_type,
                            data_type::undef, data_type::bf16, data_type::undef)
                    && IMPLICATION(with_bias(),
                            utils::one_of(diff_weights_md(1)->data_type,
                                    data_type::f32, data_type::bf16))
                    && attr()->has_default_values() && !has_zero_dim_memory()
                    && set_default_formats();
            if (!ok) return status::unimplemented;

            const convolution_desc_t *conv_d = desc();
            const memory_desc_t *src_d = src_md();
            rtus_prepare(
                    this, conv_d, src_d, diff_dst_md(), diff_weights_md(0));

            status_t status = jit_avx512_core_bf16_1x1_conv_kernel::init_conf(
                    jcp_, *conv_d, *src_d, *diff_weights_md(0), *diff_dst_md(),
                    *attr(), zendnn_get_max_threads(), rtus_.reduce_src_);
            if (status != status::success) return status;

            auto scratchpad = scratchpad_registry().registrar();
            status = jit_avx512_core_bf16_1x1_conv_kernel::init_scratchpad(
                    scratchpad, jcp_);
            if (status != status::success) return status;

            rtus_prepare_space_info(this, scratchpad, jcp_.nthr);

            return status::success;
        }

        // TODO (Roma): structs conf header cleanup
        jit_1x1_conv_conf_t jcp_;
        reduce_to_unit_stride_t rtus_;

    protected:
        bool set_default_formats() {
            using namespace format_tag;

            auto dat_tag = utils::pick(ndims() - 3, nCw16c, nChw16c, nCdhw16c);
            auto wei_tag = utils::pick(2 * ndims() - 6 + with_groups(),
                    OIw16i16o, gOIw16i16o, OIhw16i16o, gOIhw16i16o, OIdhw16i16o,
                    gOIdhw16i16o);

            bool ok = set_default_formats_common(dat_tag, wei_tag, dat_tag);
            return ok;
        }
    };

    template <cpu_isa_t isa, typename conv_t>
    friend status_t init_rtus_driver(conv_t *self);

    jit_avx512_core_bf16_1x1_convolution_bwd_weights_t(const pd_t *apd)
        : primitive_t(apd) {}

    status_t init(engine_t *engine) override;

    status_t execute(const exec_ctx_t &ctx) const override {
        execute_backward_weights(ctx);
        return status::success;
    }

    typedef typename prec_traits<data_type::bf16>::type src_data_t;
    typedef typename prec_traits<data_type::bf16>::type diff_dst_data_t;

    typedef typename prec_traits<diff_weights_type>::type diff_wei_data_t;

private:
    void execute_backward_weights(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }

    std::unique_ptr<jit_avx512_core_bf16_1x1_conv_kernel> kernel_;
    std::unique_ptr<cpu_accumulator_1d_t<data_type::f32>> acc_ker_;

    /* reduction to unit stride */
    std::unique_ptr<rtus_driver_t<avx512_common>> rtus_driver_;

    std::unique_ptr<jit_avx512_core_bf16_reorder_s16c_to_S16c2s_t> tr_reorder_;
    std::unique_ptr<jit_avx512_core_bf16_reorder_s16c_to_S16c2s_t>
            tr_reorder_nhwc_src_;
    std::unique_ptr<jit_avx512_core_bf16_reorder_s16c_to_S16c2s_t>
            tr_reorder_nhwc_ddst_;
};

} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace zendnn
#endif
