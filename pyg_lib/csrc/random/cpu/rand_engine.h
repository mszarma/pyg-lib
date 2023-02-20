#pragma once
#include <iostream>
#include <ATen/ATen.h>
#include <limits.h>
#include "pyg_lib/csrc/config.h"
#if WITH_MKL_BLAS()
#include "mkl_vsl.h"
#endif
namespace pyg {
namespace random {
const int RAND_PREFETCH_SIZE = 128;
// Use at::randint to generate 64-bit random numbers
const int RAND_PREFETCH_BITS = 64;
/**
 * Amortized O(1) uniform random within a range.
 *
 * Reduced torch random overheads by prefetching large random bits in advance.
 * Prefetched random bits are consumed on demand.
 *
 */
// typedef unsigned long long int moje64;
class PrefetchedRandint {
 public:
  PrefetchedRandint()
      : PrefetchedRandint(RAND_PREFETCH_SIZE, RAND_PREFETCH_BITS) {}
  PrefetchedRandint(int size, int bits) {
    #if WITH_MKL_BLAS()
    //std::cout << "hello there\n";
      vslNewStream(&stream, VSL_BRNG_MT19937, 1);
      prefetched_randint_.resize(size);
      //uint64_t prefetched_randint_[size];
      //array_size = size;
    #endif
    prefetch(size, bits);
    }
  ~PrefetchedRandint()
    {
      #if WITH_MKL_BLAS()
      //std::cout << "*dying*\n";
      vslDeleteStream(&stream);
      #endif
    }
  /**
   * Generate next random number in a range uniformly
   *
   * @tparam T user type of random numbers
   *
   * @param range the range of uniform distribution
   *
   * @returns the chosen number in that range
   */
  template <typename T>  T next(T range) {
    unsigned needed = 64;
    // Mutiple levels of range to save prefetched bits.
    if (range < (1 << 16)) {
      needed = 16;
    } else if (range < (1UL << 32)) {
      needed = 32;
    }
    // Consume some bits
    if (bits_ < needed) {
      if (size_ > 0) {
        size_--;
        bits_ = RAND_PREFETCH_BITS;
      } else {
        // Prefetch if no enough bits
        #if WITH_MKL_BLAS()
        prefetch(prefetched_randint_.size(), RAND_PREFETCH_BITS);
        #else
        prefetch(prefetched_randint_.size(0), RAND_PREFETCH_BITS);
        #endif
      }
    }
    // Currently torch could only make 64-bit signed random numbers.
    #if WITH_MKL_BLAS()
    //uint64_t* prefetch_ptr = reinterpret_cast<uint64_t*>(prefetched_randint_);
    uint64_t* prefetch_ptr = prefetched_randint_.data();
    #else
    uint64_t* prefetch_ptr =
        reinterpret_cast<uint64_t*>(prefetched_randint_.data_ptr<int64_t>());
    #endif
    // Take the lower bits of current 64-bit number to fit in the range.
    uint64_t mask = (needed == 64) ? std::numeric_limits<uint64_t>::max()
                                   : ((1ULL << needed) - 1);
    uint64_t res = (prefetch_ptr[size_] & mask) % range;
    // Update the state
    prefetch_ptr[size_] >>= needed;
    bits_ -= needed;
    return (T)res;
  }
 private:
  // Prefetch random bits. In-place random if prefetching size is the same.
  void prefetch(int size, int bits) {
    std::cout <<"sizeof(uint64_t):" << sizeof(uint64_t) << std::endl;
    //std::cout << "BLBLALLALALALAL " <<(sizeof(moje64) != sizeof(uint64_t)) << std::endl;
    #if WITH_MKL_BLAS()

      if constexpr(sizeof(uint64_t) == 8) {
         std::cout << "sizeof(uint64_t) == 8" << std::endl;
      // if constexpr(sizeof(moje64) != sizeof(uint64_t)) {
      //   std::cout << "BLBLALLALALALAL" << std::endl;
      // }
      // int d = prefetched_randint_;
            //unsigned long long int * prefetch_ptr = static_cast<unsigned long long int *>(prefetched_randint_.data());
        viRngUniformBits64(VSL_RNG_METHOD_UNIFORMBITS64_STD, stream, size, prefetched_randint_.data());
      } else {
         unsigned long int * prefetch_ptr = prefetched_randint_.data();
        viRngUniformBits32(VSL_RNG_METHOD_UNIFORMBITS32_STD, stream, size, prefetch_ptr);
      }
    #else
    if (prefetched_randint_.size(0) != size) {
      prefetched_randint_ =
          at::randint(std::numeric_limits<int64_t>::min(),
                      std::numeric_limits<int64_t>::max(), {size},
                      at::TensorOptions().dtype(at::kLong));
    } else {
      prefetched_randint_.random_(std::numeric_limits<int64_t>::min(),
                                  std::numeric_limits<int64_t>::max());
    }
    #endif
    size_ = size - 1;
    bits_ = bits;
  }

  #if WITH_MKL_BLAS()
  VSLStreamStatePtr stream;
  std::vector<uint64_t> prefetched_randint_;
  typedef long long unsigned int typ;
  static_assert(std::is_same<typename std::vector<uint64_t>::value_type,typ>::value,"IS NOT THE SAME");
  // uint64_t array_size;
  // uint64_t prefetched_randint_[];
  #else
  at::Tensor prefetched_randint_;
  #endif
  int size_;
  int bits_;
};
/**
 * Randint functor for uniform integer distribution.
 * Wrapped PrefetchedRandint as its efficient core implementation.
 */
template <typename T>class RandintEngine {
 public:
  RandintEngine() : prefetched_(RAND_PREFETCH_SIZE, RAND_PREFETCH_BITS) {}
  // Uniform random number within range [beg, end)
  T operator()(T beg, T end) {
    TORCH_CHECK(beg < end, "Randint engine illegal range");
    T range = end - beg;
    return prefetched_.next(range) + beg;
  }
 private:
  PrefetchedRandint prefetched_;
};
/**
 * Amortized O(1) uniform random reals within [0,1).
 *
 * Reduced torch random overheads by prefetching large random numbers in
 * advance. Prefetched random reals are consumed on demand.
 *
 */
class PrefetchedRandreal {
 public:
  PrefetchedRandreal() : PrefetchedRandreal(RAND_PREFETCH_SIZE) {}
  PrefetchedRandreal(int size) { prefetch(size); }
  /**
   * Generate next random real number in [0,1) uniformly
   *
   * @tparam T user type of random numbers
   *
   * @returns the chosen number in [0,1)
   */
  template <typename T>  T next() {
    // Consume some bits
    if (size_ > 0) {
      size_--;
    } else {
      // Prefetch if no enough real numbers
      prefetch(prefetched_randreal_.size(0));
    }
    // torch::rand gives floats by default.
    float* prefetch_ptr = prefetched_randreal_.data_ptr<float>();
    float res = prefetch_ptr[size_];
    // Safe conversion because the result is in range [0,1)
    return (T)res;
  }
 private:
  // Prefetch random real numbers. In-place random if prefetching size is the
  // same. FP32 precision is sufficient for all types of random real numbers.
  void prefetch(int size) {
    if (prefetched_randreal_.size(0) != size) {
      prefetched_randreal_ = at::rand({size});
    } else {
      prefetched_randreal_.uniform_();
    }
    size_ = size - 1;
  }
  at::Tensor prefetched_randreal_;
  int size_;
};
/**
 * Randreal functor for uniform real distribution.
 * Wrapped PrefetchedRandreal as its efficient core implementation.
 */
template <typename T>class RandrealEngine {
 public:
  RandrealEngine() : prefetched_(RAND_PREFETCH_SIZE) {}
  // Uniform random number within range [beg, end)
  T operator()() { return prefetched_.next<T>(); }
 private:
  PrefetchedRandreal prefetched_;
};
}  // namespace random
}  // namespace pyg