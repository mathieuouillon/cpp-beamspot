#include "ahdc_beam_spot.h"

#include <cmath>

#include <TMath.h>

#include <hipo4/bank.h>

namespace beamspot {

// --------------------------------------------------------------------------
config ahdc_default_config() {
  config cfg;
  // central tracks: a 40-80 deg band (upper edge kept below 90 deg, see the
  // tan(theta) caveat in the header). Tune on real data.
  cfg.theta_bins = {40, 45, 50, 55, 60, 65, 70, 75, 80};
  // z window (cm) around the nominal target; the bank z is in mm (converted in
  // process()). Placeholder centred on z = 0 -- adjust to the real vertex peak.
  cfg.target_z  = 0.0;
  cfg.z_map_min = -10.0;
  cfg.z_map_max = 10.0;
  cfg.z_1d_min  = -10.0;
  cfg.z_1d_max  = 10.0;
  return cfg;
}

// --------------------------------------------------------------------------
std::vector<std::string> ahdc_selector::bank_names() const {
  return {"AHDC::kftrack"};
}

void ahdc_selector::bind(hipo::banklist& banks) const {
  i_kftrack_ = hipo::getBanklistIndex(banks, "AHDC::kftrack");
}

void ahdc_selector::process(const hipo::banklist& banks, const config& cfg,
                            const emit_fn& emit) const {
  const hipo::bank& trk = banks[i_kftrack_];

  const int n_tracks = trk.getRows();
  if (n_tracks == 0) return;

  for (int i = 0; i < n_tracks; ++i) {
    // track-quality cuts
    if (trk.getInt("n_hits", i) < cuts_.min_n_hits) continue;
    if (trk.getFloat("chi2", i) >= cuts_.max_chi2) continue;

    const float px = trk.getFloat("px", i);
    const float py = trk.getFloat("py", i);
    const float pz = trk.getFloat("pz", i);

    // phi/theta from the momentum (MeV; units cancel), with zero-momentum guards
    const double phi_raw = (px == 0.0F && py == 0.0F)
                               ? 0.0
                               : TMath::ATan2(py, px) * 180.0 / TMath::Pi();
    const double p = std::sqrt(static_cast<double>(px) * px +
                               static_cast<double>(py) * py +
                               static_cast<double>(pz) * pz);
    const double theta = (pz == 0.0F || p == 0.0)
                             ? TMath::PiOver2() * 180.0 / TMath::Pi()
                             : TMath::ACos(pz / p) * 180.0 / TMath::Pi();

    const long bin = theta_bin_of(cfg, theta);
    if (bin < 0) continue;

    const float phi  = wrap_phi_deg(static_cast<float>(phi_raw));
    const float z_cm = trk.getFloat("z", i) * 0.1F;  // mm -> cm

    emit(static_cast<std::size_t>(bin), z_cm, phi);
  }
}

}  // namespace beamspot
