#ifndef BEAM_SPOT_H
#define BEAM_SPOT_H

// ==========================================================================
// Beam spot analysis  (C++/ROOT port of the Java/groot BeamSpot)
//
// From the previous work by S. Stepanyan, CLAS12 Note 2020-003.
//
// The analysis is headless and written in a functional style: plain data
// records (config / histograms / analysis) flow through free functions
//
//     histograms h = fill_dst(files, cfg, n_jobs);      // or merge_files(...)
//     analysis   a = analyze(std::move(h), cfg);
//     beam_spot_result r = results(a);
//     write_root(a, path); write_results_txt(a, path); write_ccdb_table(a, path);
//
// It fills histograms from CLAS12 DSTs, fits the target-window position as a
// function of phi in theta bins, extracts the beam-spot (x0, y0, z0) and writes
// a single self-describing ROOT file plus the physics/CCDB text deliverables.
// All plotting lives in scripts/plot_beamspot.py.
//
// Parallel filling spawns one worker *process* per file group (each running its
// own single-threaded chain), then merges their partial histograms with
// TH1::Add. On the JLab farm this scales better than in-process threads, which
// contend on shared ROOT/heap state. Original author: fbossu (at jlab.org).
// ==========================================================================

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <hipo4/bank.h>

#include <TF1.h>
#include <TGraphErrors.h>
#include <TH1.h>
#include <TH1F.h>
#include <TH2F.h>

#include "track_selector.h"

namespace beamspot {

// --------------------------------------------------------------------------
// Analysis cuts and constants (documented; were magic numbers in the Java).
// --------------------------------------------------------------------------
namespace cuts {
// track selection
inline constexpr int dc_detector_id = 6;        // DetectorType.DC id: keep FD (DC) tracks only

// particle selection
inline constexpr int   electron_pid    = 11;    // require electrons
inline constexpr float min_momentum    = 1.5F;  // GeV/c, electron momentum threshold
inline constexpr float min_momentum_sq = min_momentum * min_momentum;  // compare p^2 to skip a sqrt

// z-slice gaussian fit acceptance (per phi slice)
inline constexpr double min_slice_integral = 10.0;  // skip near-empty phi slices
inline constexpr double min_fit_integral   = 50.0;  // need enough entries inside the fit window
inline constexpr double peak_half_window   = 6.0;   // peak must sit within target_z +/- this (cm)
inline constexpr double min_amplitude      = 8.0;   // gaussian amplitude floor
inline constexpr double min_sigma          = 0.1;   // |sigma| floor (cm)
inline constexpr double max_sigma          = 2.0;   // |sigma| ceiling (cm)
inline constexpr double min_chi2_ndf       = 0.05;  // reduced chi2 acceptance band ...
inline constexpr double max_chi2_ndf       = 10.0;  // ... empirical, not strictly physical
}  // namespace cuts

// --------------------------------------------------------------------------
// Data records
// --------------------------------------------------------------------------

// run configuration
struct config {
  std::vector<double> theta_bins{10, 11, 12, 13, 14, 16, 18, 22, 30};
  double fit_range_scale = 1.0;
  double target_z        = 19.6;
  int    bins_per_sector = 10;

  // z-axis ranges (cm) of the filled histograms
  double z_map_min = 15.0;   // 2D z-vs-phi map per theta bin (drives the slice fits)
  double z_map_max = 25.0;
  double z_1d_min  = 15.0;    // 1D z-vertex QA histogram (not used by the fit)
  double z_1d_max  = 25.0;

  [[nodiscard]] std::size_t n_theta() const noexcept { return theta_bins.size() - 1; }
};

// --------------------------------------------------------------------------
// Shared selection helpers (identical for every observable, so kept here once)
// --------------------------------------------------------------------------

// map a phi in [-180, 180) onto the histogram's [-30, 330) convention: shift the
// negative half up by 360, then pop the topmost split sector back together.
[[nodiscard]] inline float wrap_phi_deg(float phi) noexcept {
  if (phi < 0)   phi += 360.0F;
  if (phi > 330) phi -= 360.0F;
  return phi;
}

// find the theta bin for `theta`: the first edge greater than theta, minus one.
// Returns -1 when theta falls outside [theta_bins.front(), theta_bins.back()).
[[nodiscard]] inline long theta_bin_of(const config& cfg, double theta) noexcept {
  const auto upper = std::upper_bound(cfg.theta_bins.begin(), cfg.theta_bins.end(), theta);
  const auto bin   = std::distance(cfg.theta_bins.begin(), upper) - 1;
  if (bin < 0 || static_cast<std::size_t>(bin) >= cfg.n_theta()) return -1;
  return bin;
}

// the filled (and, for the MT path, already merged) histograms
struct histograms {
  std::unique_ptr<TH1F>              h1_z;       // z vertex distribution
  std::unique_ptr<TH1F>              h1_phi;     // phi distribution at vertex
  std::vector<std::unique_ptr<TH2F>> h2_z_phi;   // phi vs z, one per theta bin
};

// the analysis products; owns the histograms it was built from
struct analysis {
  histograms                                     histos;
  std::vector<std::unique_ptr<TGraphErrors>>     g_results;  // target z vs phi, + modulation fit
  std::vector<std::vector<std::unique_ptr<TH1>>> z_slices;   // kept z slices, each with its fit
  std::unique_ptr<TGraphErrors>                  g_x, g_y, g_z, g_r, g_p;  // vs theta, + pol0 fit
};

struct value {
  double v = 0.0;
  double e = 0.0;
};

struct beam_spot_result {
  value x, y, z, r, phi;
};

// --------------------------------------------------------------------------
// Selectors
// --------------------------------------------------------------------------

// forward-detector electrons: REC::Track (DC, negative) -> REC::Particle
// (electron above a momentum threshold). The original beam-spot selection.
class electron_selector final : public track_selector {
 public:
  [[nodiscard]] std::vector<std::string> bank_names() const override;
  void bind(hipo::banklist& banks) const override;
  void process(const hipo::banklist& banks, const config& cfg, const emit_fn& emit) const override;

 private:
  mutable std::size_t i_particle_ = 0;
  mutable std::size_t i_track_    = 0;
};

// --------------------------------------------------------------------------
// Pipeline (free functions)
// --------------------------------------------------------------------------

// allocate empty, directory-detached histograms for one accumulation
[[nodiscard]] histograms make_histograms(const config& cfg);

// fill from CLAS12 DST files; n_jobs > 1 forks one worker process per file group
// and merges their partial histograms (n_jobs <= 1 streams a single chain). The
// selector decides which banks to read and which tracks become (bin, z, phi).
[[nodiscard]] histograms fill_dst(const std::vector<std::string>& files, const config& cfg,
                                  const track_selector& sel, int n_jobs);

// the -m path: sum the per-theta TH2F from prior results ROOT files
[[nodiscard]] histograms merge_files(const std::vector<std::string>& root_files, const config& cfg);

// slice fits -> modulation fit -> theta extraction -> pol0; consumes the histograms
[[nodiscard]] analysis analyze(histograms histos, const config& cfg);

// final fitted beam-spot values, read off the summary graphs' pol0 fits
[[nodiscard]] beam_spot_result results(const analysis& a);

// outputs (paths assembled by the caller; nothing is built by hand here)
void write_root(const analysis& a, const std::string& path);
void write_results_txt(const analysis& a, const std::string& path);
void write_ccdb_table(const analysis& a, const std::string& path);

}  // namespace beamspot

#endif  // BEAM_SPOT_H
