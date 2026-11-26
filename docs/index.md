---
layout: home

hero:
  name: TinyRL-Industrial
  text: 'Edge nodes that learn machine conditions'
  tagline: 'Intelligent sensing nodes monitor a machine and learn its normal states on-device with streaming reinforcement learning. They work with a human or an AI agent: context flows down, conditions flow up, and alarms go out only when something needs attention.'
  actions:
    - theme: brand
      text: Get started
      link: /guide/
    - theme: alt
      text: How the node works
      link: /system/architecture
    - theme: alt
      text: GitHub
      link: https://github.com/leekaize/TinyRL-Industrial

features:
  - title: Self-contained intelligence
    details: Each node learns its machine's normal states on-device, at 1 Hz, with a 1,348-parameter network and bounded updates. No buffers, no cloud round-trip. Weights persist to flash, so a rebooted node resumes with zero relearning.
    link: /system/architecture
    linkText: Node architecture
  - title: It speaks conditions, not data
    details: The network carries state changes, episode summaries and discovery documents, never raw streams. Over the recorded 1,357-step two-node tour the fleet published 121 bounded events where raw streaming would carry 2,714 vectors, about 22x fewer (RQ2, run-20261122-203729).
    link: /system/contract
    linkText: MQTT contract
  - title: It asks when it doesn't know
    details: An alarm that matches no learned condition opens an episode, matures a 15-sample batch, and asks by id. One label teaches the node and joins a fleet-wide vocabulary, so the next occurrence is answered on-device.
    link: /guide/lifecycle
    linkText: First-run lifecycle
  - title: Humans and agents stay in the loop
    details: One versioned contract serves a dashboard, a local LLM agent, or both. Labels, threshold governance and resets flow through a single topic; the agent skill sheet and the armed-reset dashboard keep the human in charge of the irreversible moves.
    link: /guide/agent
    linkText: The AI agent
---
