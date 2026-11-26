# References

## Primary

- Stream-X (streaming deep RL, TinyRL's algorithm lineage): [arXiv:2410.14606](https://arxiv.org/abs/2410.14606)
- TinyRL repository: [github.com/mohmdelsayed/TinyRL](https://github.com/mohmdelsayed/TinyRL) (analyzed at commit `82f0774`)
- TinyRL documentation: [tinyrl.ai/docs](https://www.tinyrl.ai/docs/) - ESP32 guide: [tinyrl.ai/docs/esp32](https://www.tinyrl.ai/docs/esp32/)
- Reference board: YD ESP32-S3 WROOM N16R8 - [espboards.dev/esp32/yd-esp32-s3](https://www.espboards.dev/esp32/yd-esp32-s3/)

## The literature map

The analytical map with per-paper "what it informs" rows lives on [literature anchors](/research/literature). The per-paper snapshots (one file each, a fixed template) live in [`literature/`](https://github.com/leekaize/TinyRL-Industrial/tree/main/literature) in the repo:

- elsayed-2024-stream-x.md
- ren-2021-tinyol.md
- dohare-2024-loss-plasticity.md
- kearney-2019-idbd-td.md
- deasis-2026-differential-td.md
- islam-2018-glad.md
- mueller-2025-ts-ad-industry.md
- settles-2012-active-learning.md
- zhou-2022-goal-oriented-semcom.md

(plus `rltools`/IDBD-adjacent anchors listed on the literature page; `_template.md` is the snapshot template.)

## Standards

- IEC 63278 (asset administration shell), IEC 62541 (OPC UA), OASIS MQTT 5.0: versioned machine-readable industrial contracts; MQTT is the transport of record here.
