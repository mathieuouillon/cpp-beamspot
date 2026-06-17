#ifndef TRACK_SELECTOR_H
#define TRACK_SELECTOR_H

// ==========================================================================
// track_selector.h -- the one piece of the beam-spot pipeline that differs
// between analyses: how an event's tracks become (theta_bin, z, phi) triples.
//
// Everything downstream (histograms, slice/modulation fits, ROOT/text output)
// is observable-agnostic and shared. A concrete selector decides which banks to
// read and which tracks to keep:
//
//   - electron_selector : forward-detector electrons (REC::Particle + REC::Track)
//   - ahdc_selector     : AHDC::kftrack central tracks
//
// The fill is multi-process (one forked worker per file group); each worker
// builds its own hipo::chain, so bank-index resolution must happen per-chain.
// That is the bind() step: getBanks() -> bind(banks) once, then process() per
// event. A polymorphic interface (not a template) keeps the fork/merge infra in
// one translation unit and shared by both binaries.
// ==========================================================================

#include <functional>
#include <string>
#include <vector>

#include <hipo4/bank.h>

namespace beamspot {

struct config;  // defined in beam_spot.h

// called once per accepted track: (theta bin index, z in cm, phi in degrees)
using emit_fn = std::function<void(std::size_t bin, float z_cm, float phi_deg)>;

struct track_selector {
  virtual ~track_selector() = default;

  // bank names to request from the chain (passed to hipo::chain::getBanks)
  [[nodiscard]] virtual std::vector<std::string> bank_names() const = 0;

  // resolve and cache bank indices against THIS chain's dictionary; called once
  // per chain (so each forked worker re-binds against its own banklist). const
  // with mutable cached indices, so the fill path can hold the selector by
  // const ref while each forked copy binds independently.
  virtual void bind(hipo::banklist& banks) const = 0;

  // run the per-event selection, invoking emit(bin, z_cm, phi_deg) per track.
  virtual void process(const hipo::banklist& banks, const config& cfg,
                       const emit_fn& emit) const = 0;
};

}  // namespace beamspot

#endif  // TRACK_SELECTOR_H
