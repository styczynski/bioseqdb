module;

export module bioseq.simd.utils;

import bioseq.common.templating;

export namespace bioseq::simd {

/// DOC0261 §4.5 / §12.6 — `for_each_index<W>` is the unified primitive
/// living in `bioseq_common.templating.pack_for_each`. Re-exported under
/// the SIMD namespace so existing callers (`vector_stream.cppm`) keep the
/// unqualified name without forcing Highway templates into this module BMI.
using ::bioseq::common::templating::for_each_index;

}
