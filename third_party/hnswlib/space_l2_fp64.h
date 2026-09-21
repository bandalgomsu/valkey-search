#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "hnswlib.h"

namespace hnswlib {

static double L2SqrFP64(const void *pVect1v, const void *pVect2v,
                        const void *qty_ptr,
                        [[maybe_unused]] double product_magnitude) {
  const double *pVect1 = static_cast<const double *>(pVect1v);
  const double *pVect2 = static_cast<const double *>(pVect2v);
  const size_t qty = *static_cast<const size_t *>(qty_ptr);
  // Scale before subtracting: `1e308 - -1e308` and squaring a finite
  // difference can overflow even though each stored element is valid FP64.
  double scale = 0.0;
  for (size_t i = 0; i < qty; ++i) {
    scale = std::max(scale, std::abs(pVect1[i]));
    scale = std::max(scale, std::abs(pVect2[i]));
  }
  if (scale == 0.0) return 0.0;

  double scaled_sum_sq = 0.0;
  for (size_t i = 0; i < qty; ++i) {
    const double scaled_difference = pVect1[i] / scale - pVect2[i] / scale;
    scaled_sum_sq += scaled_difference * scaled_difference;
  }
  const double max_distance = std::numeric_limits<double>::max();
  if (scaled_sum_sq > max_distance / scale / scale) return max_distance;
  // Keep the intermediate bounded as well. The check above proves this
  // ordering is finite, whereas `(scale * scale) * scaled_sum_sq` can
  // overflow before it is multiplied by a sufficiently small sum.
  return scale * (scale * scaled_sum_sq);
}

class L2SpaceFP64 : public SpaceInterface<double> {
  DISTFUNC<double> fstdistfunc_;
  size_t data_size_;
  size_t dim_;

 public:
  explicit L2SpaceFP64(size_t dim)
      : fstdistfunc_(L2SqrFP64), data_size_(dim * sizeof(double)), dim_(dim) {}

  size_t get_data_size() { return data_size_; }
  DISTFUNC<double> get_dist_func() { return fstdistfunc_; }
  void *get_dist_func_param() { return &dim_; }
};

}  // namespace hnswlib
