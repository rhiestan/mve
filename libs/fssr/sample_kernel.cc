/*
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 */

#include "fssr/sample_kernel.h"

#ifdef FSSR_USE_HIGHWAY

#include <cstring>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "fssr/sample_kernel.cc"
#include "hwy/foreach_target.h"  // IWYU pragma: keep
#include "hwy/highway.h"
#include "hwy/contrib/math/math-inl.h"

HWY_BEFORE_NAMESPACE();
namespace fssr {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

/*
 * Mirrors IsoOctree::sample_ifn() (derivative variant) and evaluate():
 * the rotation into the LCS of the sample and the back-rotation of the
 * derivatives are done in single precision (as in the scalar code),
 * everything else in double precision. Lane i of every vector is one sample.
 */
void
AccumulateImpl (double const* voxel_pos, SampleBatch const& b,
    SampleTotals* out)
{
    using DD = hn::ScalableTag<double>;
    using DF = hn::Rebind<float, DD>;
    DD const dd;
    DF const df;
    std::size_t const N = hn::Lanes(dd);

    auto Pd = [&](double v) { return hn::Set(dd, v); };
    auto Pf = [&](float v) { return hn::Set(df, v); };

    double const kPi = 3.14159265358979323846;
    double const kSqrt2Pi = 2.50662827463100050242;
    float const eps = 0.001f;

    hn::Vec<DF> const vxf[3] = { Pf(static_cast<float>(voxel_pos[0])),
        Pf(static_cast<float>(voxel_pos[1])),
        Pf(static_cast<float>(voxel_pos[2])) };
    hn::Vec<DD> const vxd[3] = { Pd(voxel_pos[0]), Pd(voxel_pos[1]),
        Pd(voxel_pos[2]) };

    hn::Vec<DD> acc_value = Pd(0), acc_weight = Pd(0), acc_scale = Pd(0);
    hn::Vec<DD> acc_cw = Pd(0);
    hn::Vec<DD> acc_vd[3] = { Pd(0), Pd(0), Pd(0) };
    hn::Vec<DD> acc_wd[3] = { Pd(0), Pd(0), Pd(0) };
    hn::Vec<DD> acc_col[3] = { Pd(0), Pd(0), Pd(0) };

    /* v - eps <= ref <= v + eps, as math::algo::predicate_epsilon_equal. */
    auto similar = [&](hn::Vec<DF> v, float ref) {
        auto r = Pf(ref);
        return hn::And(hn::Le(hn::Sub(v, Pf(eps)), r),
            hn::Le(r, hn::Add(v, Pf(eps))));
    };

    for (std::size_t i = 0; i < b.size; i += N)
    {
        auto const pxf = hn::LoadU(df, &b.pos[0][i]);
        auto const pyf = hn::LoadU(df, &b.pos[1][i]);
        auto const pzf = hn::LoadU(df, &b.pos[2][i]);
        auto const nx = hn::LoadU(df, &b.normal[0][i]);
        auto const ny = hn::LoadU(df, &b.normal[1][i]);
        auto const nz = hn::LoadU(df, &b.normal[2][i]);
        auto const scale_f = hn::LoadU(df, &b.scale[i]);
        auto const conf = hn::PromoteTo(dd, hn::LoadU(df, &b.confidence[i]));

        /* Rotation matrix from the normal (rotation_from_normal). */
        auto const sim_ref = hn::And(similar(nx, 1.0f),
            hn::And(similar(ny, 0.0f), similar(nz, 0.0f)));
        auto const sim_mir = hn::And(similar(nx, -1.0f),
            hn::And(similar(ny, 0.0f), similar(nz, 0.0f)));

        /* axis1 = normalize(normal x (1,0,0)) = normalize(0, nz, -ny). */
        auto const c1 = nz;
        auto const c2 = hn::Neg(ny);
        auto const cnorm = hn::Sqrt(hn::Add(hn::Mul(c1, c1), hn::Mul(c2, c2)));
        auto const a0 = Pf(0.0f);
        auto const a1 = hn::Div(c1, cnorm);
        auto const a2 = hn::Div(c2, cnorm);
        /* axis2 = normal x axis1 */
        auto const b0 = hn::Sub(hn::Mul(ny, a2), hn::Mul(nz, a1));
        auto const b1 = hn::Sub(hn::Mul(nz, a0), hn::Mul(nx, a2));
        auto const b2 = hn::Sub(hn::Mul(nx, a1), hn::Mul(ny, a0));

        /* Rows of the rotation matrix: general case, identity, mirror. */
        hn::Vec<DF> const gen[9] = { nx, ny, nz, a0, a1, a2, b0, b1, b2 };
        float const ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        float const mirr[9] = { -1, 0, 0, 0, -1, 0, 0, 0, 1 };
        hn::Vec<DF> rot[9];
        for (int k = 0; k < 9; ++k)
            rot[k] = hn::IfThenElse(sim_ref, Pf(ident[k]),
                hn::IfThenElse(sim_mir, Pf(mirr[k]), gen[k]));

        /* tpos = rot * (voxel_pos - sample.pos), in single precision. */
        auto const dx = hn::Sub(vxf[0], pxf);
        auto const dy = hn::Sub(vxf[1], pyf);
        auto const dz = hn::Sub(vxf[2], pzf);
        hn::Vec<DD> t[3];
        for (int r = 0; r < 3; ++r)
        {
            auto tf = hn::Add(hn::Add(hn::Mul(rot[3 * r + 0], dx),
                hn::Mul(rot[3 * r + 1], dy)), hn::Mul(rot[3 * r + 2], dz));
            t[r] = hn::PromoteTo(dd, tf);
        }

        auto const s = hn::PromoteTo(dd, scale_f);
        auto const s2 = hn::Mul(s, s);
        auto const xx = hn::Add(hn::Add(hn::Mul(t[0], t[0]),
            hn::Mul(t[1], t[1])), hn::Mul(t[2], t[2]));

        /* Basis function and derivative. */
        auto const g = hn::Exp(dd, hn::Div(hn::Neg(xx), hn::Mul(Pd(2.0), s2)));
        auto const value_norm = hn::Mul(Pd(2.0 * kPi), hn::Mul(s2, s2));
        auto const deriv_norm = hn::Mul(hn::Mul(value_norm, Pd(2.0)), s2);
        auto const value = hn::Div(hn::Mul(t[0], g), value_norm);
        hn::Vec<DD> vd[3];
        vd[0] = hn::Div(hn::Mul(hn::Mul(Pd(2.0), hn::Sub(s2,
            hn::Mul(t[0], t[0]))), g), deriv_norm);
        vd[1] = hn::Div(hn::Mul(hn::Mul(hn::Mul(Pd(-2.0), t[0]), t[1]), g),
            deriv_norm);
        vd[2] = hn::Div(hn::Mul(hn::Mul(hn::Mul(Pd(-2.0), t[0]), t[2]), g),
            deriv_norm);

        /* Weight function and derivative (zero outside of radius 3). */
        auto const sq = hn::Div(xx, s2);
        auto const inside = hn::Lt(sq, Pd(9.0));
        auto const sq_root = hn::Sqrt(sq);
        auto const deriv_factor = hn::Sub(hn::Add(Pd(-4.0 / 3.0),
            hn::Mul(Pd(48.0 / 54.0), sq_root)), hn::Mul(Pd(4.0 / 27.0), sq));
        hn::Vec<DD> wd[3];
        for (int k = 0; k < 3; ++k)
            wd[k] = hn::IfThenElseZero(inside,
                hn::Div(hn::Mul(deriv_factor, t[k]), s));
        auto const weight = hn::IfThenElseZero(inside, hn::Sub(hn::Add(
            hn::Sub(Pd(1.0), hn::Mul(Pd(2.0 / 3.0), sq)),
            hn::Mul(Pd(8.0 / 27.0), hn::Mul(sq, sq_root))),
            hn::Mul(Pd(1.0 / 27.0), hn::Mul(sq, sq))));

        /* Rotate derivatives back (single precision, transposed matrix). */
        for (int pass = 0; pass < 2; ++pass)
        {
            hn::Vec<DD>* d = (pass == 0) ? vd : wd;
            hn::Vec<DF> f[3];
            for (int k = 0; k < 3; ++k)
                f[k] = hn::DemoteTo(df, d[k]);
            for (int r = 0; r < 3; ++r)
            {
                auto rf = hn::Add(hn::Add(hn::Mul(rot[r], f[0]),
                    hn::Mul(rot[3 + r], f[1])), hn::Mul(rot[6 + r], f[2]));
                d[r] = hn::PromoteTo(dd, rf);
            }
        }

        /* Accumulate. */
        acc_value = hn::Add(acc_value,
            hn::Mul(hn::Mul(value, weight), conf));
        acc_weight = hn::Add(acc_weight, hn::Mul(weight, conf));
        for (int k = 0; k < 3; ++k)
        {
            auto term = hn::Add(hn::Mul(vd[k], weight), hn::Mul(wd[k], value));
            acc_vd[k] = hn::Add(acc_vd[k], hn::Mul(term, conf));
            acc_wd[k] = hn::Add(acc_wd[k], hn::Mul(wd[k], conf));
        }

        /* Color weight: normalized Gaussian with sigma = scale / 5. */
        auto const sigma = hn::PromoteTo(dd, hn::Div(scale_f, Pf(5.0f)));
        auto const ex = hn::Sub(vxd[0], hn::PromoteTo(dd, pxf));
        auto const ey = hn::Sub(vxd[1], hn::PromoteTo(dd, pyf));
        auto const ez = hn::Sub(vxd[2], hn::PromoteTo(dd, pzf));
        auto const ee = hn::Add(hn::Add(hn::Mul(ex, ex), hn::Mul(ey, ey)),
            hn::Mul(ez, ez));
        auto const cg = hn::Exp(dd, hn::Div(hn::Neg(ee),
            hn::Mul(Pd(2.0), hn::Mul(sigma, sigma))));
        auto const cw = hn::Mul(hn::Div(cg, hn::Mul(sigma, Pd(kSqrt2Pi))),
            conf);
        acc_scale = hn::Add(acc_scale, hn::Mul(s, cw));
        acc_cw = hn::Add(acc_cw, cw);
        for (int k = 0; k < 3; ++k)
        {
            auto col = hn::PromoteTo(dd, hn::LoadU(df, &b.color[k][i]));
            acc_col[k] = hn::Add(acc_col[k], hn::Mul(col, cw));
        }
    }

    out->value += hn::ReduceSum(dd, acc_value);
    out->weight += hn::ReduceSum(dd, acc_weight);
    out->scale += hn::ReduceSum(dd, acc_scale);
    out->color_weight += hn::ReduceSum(dd, acc_cw);
    for (int k = 0; k < 3; ++k)
    {
        out->value_deriv[k] += hn::ReduceSum(dd, acc_vd[k]);
        out->weight_deriv[k] += hn::ReduceSum(dd, acc_wd[k]);
        out->color[k] += hn::ReduceSum(dd, acc_col[k]);
    }
}

int64_t
CurrentTarget (void)
{
    return HWY_TARGET;
}

}  // namespace HWY_NAMESPACE
}  // namespace fssr
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

FSSR_NAMESPACE_BEGIN

HWY_EXPORT(AccumulateImpl);
HWY_EXPORT(CurrentTarget);

void
accumulate_samples (double const* voxel_pos, SampleBatch const& batch,
    SampleTotals* totals)
{
    HWY_DYNAMIC_DISPATCH(AccumulateImpl)(voxel_pos, batch, totals);
}

bool
limit_sample_kernel_target (char const* name)
{
    int64_t const all = hwy::SupportedTargets();
    int64_t limit = 0;
    for (int64_t t = all; t != 0; t &= t - 1)
    {
        int64_t const bit = t & -t;
        if (std::strcmp(hwy::TargetName(bit), name) == 0)
            limit = bit;
    }
    if (limit == 0)
        return false;
    /* Keep the requested target and all less capable ones (higher bits). */
    hwy::SetSupportedTargetsForTest(all & ~(limit - 1));
    return true;
}

char const*
sample_kernel_target_name (void)
{
    return hwy::TargetName(HWY_DYNAMIC_DISPATCH(CurrentTarget)());
}

void
SampleBatch::assign (std::vector<Sample const*> const& samples,
    float max_scale, double const* voxel_pos)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < samples.size(); ++i)
        count += (samples[i]->scale <= max_scale);
    std::size_t const padded = (count + 7) & ~static_cast<std::size_t>(7);

    for (int k = 0; k < 3; ++k)
    {
        pos[k].resize(padded);
        normal[k].resize(padded);
        color[k].resize(padded);
    }
    scale.resize(padded);
    confidence.resize(padded);
    size = padded;

    std::size_t n = 0;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        Sample const& s = *samples[i];
        if (s.scale > max_scale)
            continue;
        for (int k = 0; k < 3; ++k)
        {
            pos[k][n] = s.pos[k];
            normal[k][n] = s.normal[k];
            color[k][n] = s.color[k];
        }
        scale[n] = s.scale;
        confidence[n] = s.confidence;
        n += 1;
    }

    /* Inert padding samples: zero confidence, located at the voxel. */
    for (; n < padded; ++n)
    {
        for (int k = 0; k < 3; ++k)
        {
            pos[k][n] = static_cast<float>(voxel_pos[k]);
            normal[k][n] = (k == 0 ? 1.0f : 0.0f);
            color[k][n] = 0.0f;
        }
        scale[n] = 1.0f;
        confidence[n] = 0.0f;
    }
}

FSSR_NAMESPACE_END

#endif  // HWY_ONCE
#endif  // FSSR_USE_HIGHWAY
