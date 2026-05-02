/* `me::compose::register_chromatic_aberration_kind` —
 * register `TaskKindId::RenderChromaticAberration`.
 *
 * M12 §156 (3/5 stylized effects). Four Int64 properties
 * (red_dx / red_dy / blue_dx / blue_dy). `time_invariant=true`. */
#pragma once

namespace me::compose {

void register_chromatic_aberration_kind();

}  // namespace me::compose
