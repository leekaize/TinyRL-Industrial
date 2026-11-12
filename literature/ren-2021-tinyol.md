# TinyOL: TinyML with Online-Learning on Microcontrollers

| | |
|---|---|
| Source | arXiv:2103.08295 (v3, 2021-04-10) |
| Authors | Haoyu Ren, Darko Anicic, Thomas Runkler (Siemens AG / TU Munich) · 2021 |
| Venue | IJCNN 2021 |
| Last viewed | 2026-11-11 (arXiv HTML full text v3) |
| Pillar | 1-machine-learning |
| Informs | On-node MCU training feasible; runtime class growth without replay storage |
| Status | ✅ snapshot 2026-11-11 |

## Key concepts

- **TinyML today = inference-only**: train on a powerful machine, flash the model as a C array to the MCU. Static model: costly to update across a distributed fleet, doesn't transfer machine-to-machine, no field data stored or shipped, degrades under **concept drift** (input distribution changes over time).
- **TinyOL**: a C++ layer attached to (or replacing) an existing NN's output layer. Frozen graph lives in flash; the TinyOL layer trains in RAM via online SGD. Similar to transfer learning: fixed body, trainable head.
- **Pipeline (their Alg. 1)**: per sample → running mean/variance update → standardize input → predict → if a label is available, update metrics and weights → **discard the sample pair**. One sample at a time; no historical data storage (no replay).
- **Runtime structure growth**: a multi-class softmax head initializes extra neurons automatically when a new class label appears.
- **Concept**: on-device post-training turns a static unsupervised autoencoder into (a) a field-adapted detector and (b) an anomaly classifier.

## Methods

- Hardware: Arduino Nano 33 BLE Sense (Cortex-M4 @ 64 MHz, 256 KB SRAM), 3-axis accelerometer @ 119 Hz, USB fan as simulated rotating machine; classes: normal / stuck / tilted (5 min each).
- Pre-trained autoencoder: 3-axis PCA → 1-d, input window (40,1) ≈ 0.25 s, 4-d embedding; anomaly = high MSE reconstruction error.
- Use case 1 (fine-tune): replace the last layer with TinyOL; 2000 Bluetooth-triggered online iterations.
- Use case 2 (multi-anomaly classification): features = encoder embedding + reconstruction error; softmax head; sequential (unshuffled) training; F1 / macro-F1 vs offline batch training.
- Timing (3000 samples): inference avg 1748 µs/iter vs online learning 1921 µs/iter (~10% overhead).

## Snapshot

TinyOL attacks the static-model gap in TinyML: shipped networks only infer, so every field mismatch (different machine, mounting position, drift) needs retraining off-device. The system appends a trainable layer to a frozen network and trains it online, one streaming sample at a time, discarding each sample after the weight update so no replay buffer exists. Input is standardized by running mean and variance maintained on the fly.

Two experiments on a Cortex-M4 board with a USB fan. First, fine-tuning: the pre-trained autoencoder's reconstruction-error distribution shifted when the board was remounted, so a fixed threshold produced too many false alarms; 2000 online fine-tune iterations realigned the field distribution to the training one. Second, classification: embedding plus reconstruction error feed a softmax head that classifies normal/stuck/tilted, growing a new neuron per newly seen class; sequential online training is competitive with offline batching, though offline wins given enough data and epochs. Training costs ~10% over inference per sample.

Limitations (authors'): frozen encoder limits feature extraction for abnormal classes (only the head adapts); online loses to offline when resources are unconstrained; no forgetting analysis despite one-pass updates.

## Schema implications (TinyRL-Industrial)

- **Same standardization ingredient, independently**: TinyOL's running mean/variance per sample is our `SampleMeanStd` Welford normalizer (verified: `tinyrl_monitor.h` includes `data_normalizer.h`, `obsnorm_` "frozen normalizer after theta fit"). Two systems converging on it = keep it node-local (RQ1 decision stands).
- **Runtime class growth verified in our code**: TinyOL auto-initializes a neuron per new class; our `ConditionLibrary::label()` creates a new condition entry when the slug is unseen (up to `LIB_MAX`), and `episode.cpp` refines centroids via EMA on labeled episodes. Same function, no schema change (RQ1).
- **Their false-alarm mechanism is our θ story**: remounting shifted the MSE distribution → fixed threshold floods false alarms → on-device adaptation fixed it. Our equivalent split: CAL + θ-fit handle mounting/site shift on-node; `Action/feedback` (`normal`/`fault`) governs θ after deployment (schema-exposed, RQ1).
- **Labels arrive as live supervision** in TinyOL (Bluetooth-triggered training runs) = a phase/start control + labels; our schema exposes exactly those knobs (`Action/start`, `label`). Their "2000 iterations via Bluetooth" is the traffic pattern our one-sided ask loop avoids (RQ2: they need many labeled samples; we ask only on unmatched alarms).
- **On-node training cost evidence for RQ2**: ~10% overhead on a weaker MCU (M4 @ 64 MHz) at their sample rates; our fwd+bwd+update @ 1 Hz ≪ 0.1% CPU @ 240 MHz. Feasibility of incremental on-node training is no longer the open question - bandwidth is.
- **Contrast to record**: TinyOL freezes the body and adapts the head (transfer learning); our node trains the whole small net (Stream-TD). Their frozen-encoder limitation (poor abnormal features) argues for our full-net streaming choice on the prediction task.

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Online fine-tune fixes field distribution shift | CAL + θ-fit on first power-up; `normal`/`fault` feedback afterwards |
| Auto-grow output layer per new class | `ConditionLibrary::label()` append + centroid refine (already implemented) |
| Batch-labeled sequential training via Bluetooth | One-sided asks: label only unmatched alarm episodes |
| Full-size on-device training (future work) | Stream-TD full-net training at 1 Hz (done, needs field validation) |

## Open questions

- [ ] One-pass updates without replay risk forgetting past conditions on our node too; Dohare/Kearney rows address plasticity - is our flash library the full answer, or do we need bounded replay for the net (RQ2 tension: buffer-free claim)?
- [ ] Measure our actual per-episode CPU/memory deltas vs TinyOL's 10% figure to put a number in the RQ2 table.
- [ ] Does the agent ever need to trigger a TinyOL-style supervised training burst (multi-sample label session), or is one label per episode enough? (RQ1: possible new `Action` type, default: no.)
