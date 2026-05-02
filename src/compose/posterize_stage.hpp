/* `me::compose::register_posterize_kind` — register
 * `TaskKindId::RenderPosterize`. M12 §156 (4/5). One Int64
 * `levels` property; `time_invariant=true`. */
#pragma once

namespace me::compose {

void register_posterize_kind();

}  // namespace me::compose
