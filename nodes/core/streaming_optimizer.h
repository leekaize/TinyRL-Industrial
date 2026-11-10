// =====================================================================
// streaming_optimizer.h -- StreamingOptimizer (Stream-X v3, Appendix A).
//
// Per-coordinate bounded update. With u_i the full increment (-delta*z_i):
//
//     v_i = max(beta * v_i, |u_i|);      w_i += lr * u_i / (v_i + eps)
//
// v is a leaky (decaying) element-wise running maximum of the increments.
// Because v_i >= |u_i| at update time, |w_i move| < lr for ANY increment —
// no global |z|_1 coupling. (v2 ObGD divided by the summed trace norm, so
// per-parameter steps shrink ~1/d as networks grow.)
//
// Default node optimizer (design decision 2026-11-03); ObGD remains
// available behind -DMONITOR_USE_OBGD for ablation.
//
// eps = 1.0 is a unit-signal floor, not just numerical stability: our
// signals are z-scored, so |u| ~ O(1). Without it, arbitrarily tiny
// gradients would still take ~lr-sized steps (sign-SGD pathology).
//
// Interface mirrors obgd.h (add_parameters / zero_grad / step(delta,
// reset)) so the monitor swaps optimizers via a typedef.
// =====================================================================
#pragma once

#include <cmath>
#include <vector>
#include <matrix.h>
#include <module.h>

class StreamingOptimizer {
public:
    StreamingOptimizer(ag::Float learning_rate = 2e-3, ag::Float gamma = 0.9,
                       ag::Float lambda = 0.8, ag::Float beta = 0.99995,
                       ag::Float eps = 1.0)
        : lr_(learning_rate), gamma_(gamma), lambda_(lambda), beta_(beta),
          eps_(eps), gamma_lambda_(gamma * lambda) {}

    void add_parameters(const std::vector<std::shared_ptr<nn::Layer>>& layers) {
        for (const auto& layer : layers) {
            if (!layer->has_parameters()) continue;
            for (auto* param : layer->get_parameters()) {
                if (param && param->requires_grad()) {
                    parameters_.push_back(param);
                    etraces_.push_back(ag::Matrix::Zeros(param->shape()));
                    vmax_.push_back(ag::Matrix::Zeros(param->shape()));
                }
            }
        }
    }

    void zero_grad() {
        for (auto* param : parameters_) param->zero_grad();
    }

    void step(ag::Float delta, const bool reset) {
        for (size_t i = 0; i < parameters_.size(); ++i) {
            ag::Float* z = etraces_[i].data();
            const ag::Float* g = parameters_[i]->grad().data();
            ag::Float* v = vmax_[i].data();
            ag::Matrix& w = const_cast<ag::Matrix&>(parameters_[i]->value());
            ag::Float* p = w.data();
            const size_t n = etraces_[i].size();
            for (size_t j = 0; j < n; ++j) {
                z[j] = gamma_lambda_ * z[j] + g[j];   // trace update
                const ag::Float u = (-delta) * z[j];  // descent increment
                const ag::Float au = (u < 0 ? -u : u);
                const ag::Float decayed = beta_ * v[j];
                v[j] = (decayed > au) ? decayed : au; // leaky running max
                p[j] += lr_ * u / (v[j] + eps_);      // bounded: |move| < lr
            }
        }
        if (reset) {                                   // regime change:
            for (auto& e : etraces_) e.zero();         // forget old gradients
            for (auto& m : vmax_)    m.zero();         // and old scale
        }
    }

private:
    ag::Float lr_, gamma_, lambda_, beta_, eps_, gamma_lambda_;
    std::vector<ag::Tensor*> parameters_;
    std::vector<ag::Matrix>  etraces_;
    std::vector<ag::Matrix>  vmax_;
};
