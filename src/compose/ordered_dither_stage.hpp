/* `me::compose::register_ordered_dither_kind` —
 * `RenderOrderedDither = 0x3012`. M12 §156 (5/5).
 * Two Int64 properties (matrix_size + levels);
 * `time_invariant=true`. */
#pragma once

namespace me::compose {

void register_ordered_dither_kind();

}  // namespace me::compose
