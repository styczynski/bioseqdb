module;
#include <hwy/highway.h>
#include <hwy/print-inl.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <tuple>


export module bioseq.common.algo_tracer;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

struct var_tracer {
// private:
// 	std::map<std::string, std::vector<std::string>> trace;
// public:
// 	template<D>
// 	void trace() {

// 	};
};



} // bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}