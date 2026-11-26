# The AI agent

The agent does the labelling, tuning and coordination a human would otherwise do per node, and gets better as more nodes come online. `agent/skills.md` is the contract that makes that safe: a paste-ready skill sheet for any LLM (local or cloud) that runs next to the broker. This page summarizes it; the sheet itself is authoritative.

## What the agent is told

**Identity.** It orchestrates semantic condition nodes that publish `State/condition`, `State/event`, `State/node_info`, `State/keepalive` and accept `Action/feedback`. Nodes are self-contained; they publish conditions, NOT telemetry. Never ask a node for raw streams. Discovery: subscribe `+/+/+/+/+/State/node_info` (retained).

**The six skills** (agent/skills.md):

1. **Intake & registry**: on every `node_info` retain, register the node (dev_id, node_type, features, UNS base); keep a node-to-asset registry; read the retained `State/condition` for the node's phase.
2. **Answer open episodes** (the core loop): an episode with `condition_id: "unknown"` is the node asking. Gather context first (asset, plausible fault modes, cross-node signatures, maintenance log), then label BY BATCH ID with a short snake_case slug, reusing an existing slug across nodes for the same physical phenomenon. Discard batches that no longer mean anything. Name faults WITH their operating point (`speed_1_imbalance`). One physical event usually opens asks on several nodes of the same machine: answer every open batch of that machine with the SAME slug.
3. **Threshold & lifecycle governance**: `normal` raises theta (+10%), `fault` lowers it (-5%); apply sparingly and alert a human above ~5 normal corrections per week. Never label during `state=learn`. Commission on a representative machine state (RUNNING with the PM1125H meter).
4. **Cross-node orchestration**: keep a graph of node-condition and condition-condition relations; nodes never consult each other, the correlation is the agent's job. The same slug on 2+ related nodes in one window is usually a shared root cause: escalate with both summaries.
5. **Bandwidth discipline** (system invariant): the network carries state changes, episode transitions, labels. It must NEVER carry periodic telemetry, raw waveforms, or heartbeat dumps.
6. **UNS/MES/ERP bridging**: the topic path maps to enterprise/site/area/line/workcell; episode close events with condition slugs are the natural maintenance-request triggers.

## Non-negotiables

Verbatim from agent/skills.md:

- Never fabricate a label without meas evidence + context.
- Never restart learning on a monitored node without human approval.
- Never send `reset` (factory wipe) without explicit human approval; the dashboard's RESET button (press once to arm, again within 10 s) is the human path.
- Never widen the MQTT contract beyond `uns/uns.json` unilaterally.
- Log every feedback sent (action, label, by, episode timestamp); the site audit trail is part of the product.

## Running one

Point the agent runtime at the broker with the sheet plus `uns/`; it needs nothing per node. The contract supports it end to end: `State/condition` carries the ask and its batch id, `Action/feedback` answers it, `skills.md` is the behavioural contract. Open design questions (which runtime, how the agent surfaces questions to operators, whether it also reads event history) are tracked in the session handoff of 2026-11-24.
