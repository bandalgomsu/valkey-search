#pragma once

#include <algorithm>
#include <cmath>

#include "hnswlib.h"

namespace hnswlib {

static double InnerProductFP64(const void *pVect1v, const void *pVect2v,
                               const void *qty_ptr) {
  const double *pVect1 = static_cast<const double *>(pVect1v);
  const double *pVect2 = static_cast<const double *>(pVect2v);
  const size_t qty = *static_cast<const size_t *>(qty_ptr);
  double res = 0.0;
  for (size_t i = 0; i < qty; ++i) {
    res += pVect1[i] * pVect2[i];
  }
  return res;
}

// Computes cosine similarity without forming either a raw dot product or a
// reciprocal-magnitude product. Both can overflow/underflow for valid FP64
// vectors (e.g. 1e200 * 1e200 and 1e-200 * 1e-200 respectively).
static double CosineSimilarityFP64(const double *pVect1, const double *pVect2,
                                   size_t qty) {
  double max_abs_1 = 0.0;
  double max_abs_2 = 0.0;
  for (size_t i = 0; i < qty; ++i) {
    max_abs_1 = std::max(max_abs_1, std::abs(pVect1[i]));
    max_abs_2 = std::max(max_abs_2, std::abs(pVect2[i]));
  }
  if (max_abs_1 == 0.0 || max_abs_2 == 0.0) return 0.0;

  double dot = 0.0;
  double sum_sq_1 = 0.0;
  double sum_sq_2 = 0.0;
  for (size_t i = 0; i < qty; ++i) {
    const double normalized_1 = pVect1[i] / max_abs_1;
    const double normalized_2 = pVect2[i] / max_abs_2;
    dot += normalized_1 * normalized_2;
    sum_sq_1 += normalized_1 * normalized_1;
    sum_sq_2 += normalized_2 * normalized_2;
  }
  return dot / std::sqrt(sum_sq_1) / std::sqrt(sum_sq_2);
}

static double InnerProductDistanceFP64(const void *pVect1, const void *pVect2,
                                       const void *qty_ptr,
                                       double reciprocal_mag_product) {
  // An unnormalized IP query supplies 1.0. A cosine query whose reciprocal
  // product is 1.0 already has a bounded raw dot product, so its existing path
  // is safe too. All other cosine queries use scaled accumulation.
  if (reciprocal_mag_product == 1.0) {
    return 1.0 - InnerProductFP64(pVect1, pVect2, qty_ptr);
  }
  const size_t qty = *static_cast<const size_t *>(qty_ptr);
  return 1.0 - CosineSimilarityFP64(static_cast<const double *>(pVect1),
                                    static_cast<const double *>(pVect2), qty);
}

class InnerProductSpaceFP64 : public SpaceInterface<double> {
  DISTFUNC<double> fstdistfunc_;
  size_t data_size_;
  size_t dim_;

 public:
  explicit InnerProductSpaceFP64(size_t dim)
      : fstdistfunc_(InnerProductDistanceFP64),
        data_size_(dim * sizeof(double)),
        dim_(dim) {}

  size_t get_data_size() { return data_size_; }
  DISTFUNC<double> get_dist_func() { return fstdistfunc_; }
  void *get_dist_func_param() { return &dim_; }
};

}  // namespace hnswlib
