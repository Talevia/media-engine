/*
 * io::demux kernel registration.
 *
 * Registers TaskKindId::IoDemux with the task registry. Call once at
 * engine startup; idempotent (re-register overwrites with same schema).
 *
 * Kernel schema:
 *   inputs:  (none)
 *   outputs: [source: DemuxCtx]
 *   params:  [uri: string]
 */
#pragma once

namespace me::io {
void register_demux_kind();
}
