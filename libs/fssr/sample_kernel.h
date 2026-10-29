/*
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 */

#ifndef FSSR_SAMPLE_KERNEL_HEADER
#define FSSR_SAMPLE_KERNEL_HEADER

#include <cstddef>
#include <vector>

#include "fssr/defines.h"
#include "fssr/sample.h"

FSSR_NAMESPACE_BEGIN

/**
 * Structure-of-arrays copy of the samples that influence a voxel. The arrays
 * are padded with inert samples (zero confidence) to a multiple of 8 so that
 * the SIMD kernel does not need a scalar tail.
 */
struct SampleBatch
{
    std::vector<float> pos[3];
    std::vector<float> normal[3];
    std::vector<float> color[3];
    std::vector<float> scale;
    std::vector<float> confidence;
    std::size_t size = 0;

    /**
     * Fills the batch with all samples of scale up to 'max_scale' and pads
     * the arrays with inert samples located at 'voxel_pos'.
     */
    void assign (std::vector<Sample const*> const& samples, float max_scale,
        double const* voxel_pos);
};

/** Sums over all samples, as accumulated by IsoOctree::sample_ifn(). */
struct SampleTotals
{
    double value = 0.0;
    double weight = 0.0;
    double scale = 0.0;
    double color_weight = 0.0;
    double value_deriv[3] = { 0.0, 0.0, 0.0 };
    double weight_deriv[3] = { 0.0, 0.0, 0.0 };
    double color[3] = { 0.0, 0.0, 0.0 };
};

/**
 * Evaluates basis and weight functions (and derivatives) as well as the
 * color weights for all samples in the batch, using the best SIMD
 * instruction set available at runtime.
 */
void
accumulate_samples (double const* voxel_pos, SampleBatch const& batch,
    SampleTotals* totals);

/**
 * Restricts the kernel to the given SIMD target ("SSE2", "SSSE3", "SSE4",
 * "AVX2", ...), for benchmarking. Returns false if the name is unknown.
 */
bool
limit_sample_kernel_target (char const* name);

/** Name of the SIMD target in use (for diagnostics). */
char const*
sample_kernel_target_name (void);

FSSR_NAMESPACE_END

#endif // FSSR_SAMPLE_KERNEL_HEADER
