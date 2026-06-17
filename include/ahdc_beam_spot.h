#ifndef AHDC_BEAM_SPOT_H
#define AHDC_BEAM_SPOT_H

// ==========================================================================
// ahdc_beam_spot.h -- beam-spot selection from AHDC::kftrack central tracks.
//
// Reuses the entire electron beam-spot engine (slice fits -> z(phi) modulation
// -> r = A*tan(theta) -> x/y -> pol0 over theta) defined in beam_spot.h. Only
// the per-event selection changes: instead of forward-detector electrons we
// take Kalman-filter tracks from the ALERT drift chamber, using the bank's own
// (x, y, z) position (mm -> cm) and (px, py, pz) momentum direction.
//
// CAVEAT (method is intentionally unchanged): the engine recovers the transverse
// offset as r = A*tan(theta). That is well-behaved for forward electrons
// (theta <~ 30 deg) but DEGENERATE as theta -> 90 deg, where tan(theta) diverges
// and amplifies the amplitude error into r. AHDC central tracks live near
// 40-90 deg, so keep the upper theta-bin edge well below 90 deg (the default
// band stops at 80) and treat the highest-theta bins with suspicion; the pol0
// average over theta is the mitigation. Tune the theta bins and the z window on
// a real file before trusting the numbers.
// ==========================================================================

#include <string>
#include <vector>

#include <hipo4/bank.h>

#include "beam_spot.h"
#include "track_selector.h"

namespace beamspot {

// AHDC::kftrack track-quality cuts (configurable; not compile-time constants
// because good values depend on the data set and are worth scanning).
struct ahdc_cuts {
  int    min_n_hits = 4;     // require at least this many hits on the track
  double max_chi2   = 50.0;  // drop tracks above this fit chi2 (mm^2)
};

// central-tracker defaults for the shared `config`: a 40-80 deg theta band and a
// z window centred near the nominal target, all in cm and all CLI-overridable.
[[nodiscard]] config ahdc_default_config();

// AHDC::kftrack selection: bank (x,y,z) in mm -> z in cm as the modulation
// observable; phi/theta from (px,py,pz); n_hits/chi2 quality cuts.
class ahdc_selector final : public track_selector {
 public:
  explicit ahdc_selector(ahdc_cuts cuts) : cuts_(cuts) {}

  [[nodiscard]] std::vector<std::string> bank_names() const override;
  void bind(hipo::banklist& banks) const override;
  void process(const hipo::banklist& banks, const config& cfg, const emit_fn& emit) const override;

 private:
  ahdc_cuts           cuts_;
  mutable std::size_t i_kftrack_ = 0;
};

}  // namespace beamspot

#endif  // AHDC_BEAM_SPOT_H
