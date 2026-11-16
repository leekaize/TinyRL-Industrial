// =====================================================================
// plasticity.hpp -- Dohare 2024 (Nature 632) plasticity audit counters.
// HOST-TEST/BENCH LOGGING ONLY: never compiled into the node.
//
//   dead-unit fraction : hidden units whose pre-LayerNorm activation has
//                        ~zero variance across the audit window. The net
//                        uses LeakyReLU, so Dohare's zero-output ReLU test
//                        becomes a variance test (std < 1e-4 = dead).
//   mean |w|           : mean absolute Linear weight magnitude (biases
//                        excluded) -- Dohare's "weight magnitude grows".
//   effective rank     : exp(entropy) of the representation covariance
//                        spectrum (Roy & Vetterli; Dohare's metric), on
//                        the vector feeding the output Linear.
// =====================================================================
#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "tinyrl_monitor.h"

namespace plast {

constexpr int    WINDOW   = 256;   // audit window: recent raw samples
constexpr double DEAD_STD = 1e-4;  // activation std below this = dead unit

// Host replica of the monitor's Welford normalizer (SampleMeanStd: var =
// m2/(n-1), first sample var = 1). The test replays the same raw stream,
// so its frozen moments match the monitor's private nm_mean_/nm_var_ and
// its z-scores match frozenNorm().
struct ZReplica {
  double mu[cfg::MAX_FEATURES] = {0}, m2[cfg::MAX_FEATURES] = {0};
  long   n   = 0;
  void update(const float* x, int nf) {
    ++n;
    for (int f = 0; f < nf; ++f) {
      double d = x[f] - mu[f];
      mu[f] += d / (double)n;
      m2[f] += d * (x[f] - mu[f]);
    }
  }
  void z(const float* x, int nf, float* out) const {
    for (int f = 0; f < nf; ++f) {
      double var = (n >= 2) ? m2[f] / (double)(n - 1) : 1.0;
      out[f] = (float)((x[f] - mu[f]) / std::sqrt(var + 1e-8));
    }
  }
};

// Ring of recent raw samples, replayed oldest-first at audit time.
struct RawWindow {
  float buf[WINDOW][cfg::MAX_FEATURES];
  int head = 0, n = 0;
  void push(const float* x, int nf) {
    std::memcpy(buf[head], x, nf * sizeof(float));
    head = (head + 1) % WINDOW;
    if (n < WINDOW) ++n;
  }
  template <class F> void each(F f) const {
    const int start = (n < WINDOW) ? 0 : head;
    for (int k = 0; k < n; ++k) f(buf[(start + k) % WINDOW]);
  }
};

// Activations of one z-scored input: pre-LayerNorm hidden outputs (h1, h2)
// and the representation fed to the output layer (rep).
struct Acts {
  std::vector<float> h1, h2, rep;
};

// Walks the fixed core topology (tinyrl_monitor.cpp buildNet):
// Linear, LayerNorm, LeakyReLU, Linear, LayerNorm, LeakyReLU, Linear.
inline bool probe(const TinyRLConditionMonitor& mon, const float* z, int nf,
                  Acts& a) {
  const auto& L = mon.net().layers();
  if (L.size() != 7) return false;
  ag::Matrix m(1, nf);
  std::memcpy(m.data(), z, nf * sizeof(float));
  ag::Tensor t(m, false);
  t = L[0]->forward(t);                    // hidden 1, pre-LayerNorm
  a.h1.assign(t.value().data(), t.value().data() + t.value().numel());
  t = L[1]->forward(t);
  t = L[2]->forward(t);
  t = L[3]->forward(t);                    // hidden 2, pre-LayerNorm
  a.h2.assign(t.value().data(), t.value().data() + t.value().numel());
  t = L[4]->forward(t);
  t = L[5]->forward(t);                    // representation
  a.rep.assign(t.value().data(), t.value().data() + t.value().numel());
  return true;
}

inline double meanAbsW(const TinyRLConditionMonitor& mon) {
  double sum = 0;
  size_t n = 0;
  for (const auto& l : mon.net().layers()) {
    const auto* lin = dynamic_cast<const nn::Linear*>(l.get());
    if (!lin) continue;
    const ag::Matrix& w = lin->weights.value();
    for (int i = 0; i < w.numel(); ++i) sum += std::fabs((double)w.data()[i]);
    n += (size_t)w.numel();
  }
  return n ? sum / (double)n : 0.0;
}

// which: 1 = hidden 1, 2 = hidden 2
inline double deadFrac(const std::vector<Acts>& acts, int which) {
  if (acts.empty()) return 0.0;
  const std::vector<float>& s0 = (which == 1) ? acts[0].h1 : acts[0].h2;
  const int units = (int)s0.size();
  if (units == 0) return 0.0;
  int dead = 0;
  for (int u = 0; u < units; ++u) {
    double s = 0, s2 = 0;
    for (const auto& a : acts) {
      const float v = (which == 1) ? a.h1[u] : a.h2[u];
      s += v; s2 += (double)v * v;
    }
    const double n = (double)acts.size();
    const double var = s2 / n - (s / n) * (s / n);
    if (var < DEAD_STD * DEAD_STD) ++dead;
  }
  return (double)dead / (double)units;
}

// Eigenvalues of a symmetric matrix via cyclic Jacobi rotations.
inline void jacobiEigen(std::vector<std::vector<double>>& A,
                        std::vector<double>& eval) {
  const int n = (int)A.size();
  for (int sweep = 0; sweep < 60; ++sweep) {
    double off = 0;
    for (int i = 0; i < n; ++i)
      for (int j = i + 1; j < n; ++j) off += A[i][j] * A[i][j];
    if (off < 1e-20) break;
    for (int p = 0; p < n - 1; ++p)
      for (int q = p + 1; q < n; ++q) {
        const double apq = A[p][q];
        if (std::fabs(apq) < 1e-15) continue;
        const double theta = 0.5 * (A[q][q] - A[p][p]) / apq;
        const double t = (theta >= 0 ? 1.0 : -1.0) /
                         (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (int k = 0; k < n; ++k) {          // columns p, q
          const double akp = A[k][p], akq = A[k][q];
          A[k][p] = c * akp - s * akq;
          A[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < n; ++k) {          // rows p, q
          const double apk = A[p][k], aqk = A[q][k];
          A[p][k] = c * apk - s * aqk;
          A[q][k] = s * apk + c * aqk;
        }
      }
  }
  eval.resize(n);
  for (int i = 0; i < n; ++i) eval[i] = A[i][i];
}

// Effective rank of the representation over the audit window:
// exp(entropy) of the normalized covariance eigenvalues.
inline double effRank(const std::vector<Acts>& acts) {
  if (acts.empty()) return 0.0;
  const int units = (int)acts[0].rep.size();
  const int N = (int)acts.size();
  if (units == 0 || N < 2) return 0.0;
  std::vector<double> mu(units, 0.0);
  for (const auto& a : acts)
    for (int u = 0; u < units; ++u) mu[u] += a.rep[u];
  for (auto& m : mu) m /= (double)N;
  std::vector<std::vector<double>> C(units, std::vector<double>(units, 0.0));
  for (const auto& a : acts)
    for (int i = 0; i < units; ++i) {
      const double xi = a.rep[i] - mu[i];
      for (int j = i; j < units; ++j) C[i][j] += xi * (a.rep[j] - mu[j]);
    }
  for (int i = 0; i < units; ++i)
    for (int j = 0; j < i; ++j) C[i][j] = C[j][i];
  const double inv = 1.0 / (double)N;
  for (int i = 0; i < units; ++i)
    for (int j = 0; j < units; ++j) C[i][j] *= inv;
  std::vector<double> lam;
  jacobiEigen(C, lam);
  double sum = 0;
  for (double l : lam) if (l > 0) sum += l;
  if (sum <= 0) return 0.0;
  double H = 0;
  for (double l : lam) {
    if (l <= 0) continue;
    const double p = l / sum;
    H -= p * std::log(p);
  }
  return std::exp(H);
}

// One audit line: z-score the window, probe activations, print counters.
inline void audit(const TinyRLConditionMonitor& mon, const ZReplica& zr,
                  const RawWindow& win, int nf, long step) {
  if (win.n < 32) return;                    // too little history to say anything
  std::vector<Acts> acts;
  acts.reserve(win.n);
  Acts a;
  bool ok = true;
  win.each([&](const float* raw) {
    if (!ok) return;
    float z[cfg::MAX_FEATURES];
    zr.z(raw, nf, z);
    if (!probe(mon, z, nf, a)) { ok = false; return; }
    acts.push_back(a);
  });
  if (!ok) {
    printf("[PLAST] step=%-8ld unexpected net topology, audit skipped\n",
           (unsigned long)step);
    return;
  }
  printf("[PLAST] step=%-8ld dead=%.2f/%.2f mean|w|=%.4f eff_rank=%.2f (win=%d)\n",
         (unsigned long)step, deadFrac(acts, 1), deadFrac(acts, 2),
         meanAbsW(mon), effRank(acts), win.n);
}

} // namespace plast
