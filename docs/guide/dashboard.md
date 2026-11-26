# Operator dashboard

The dashboard turns the nodes' condition stream into an operator control room: read both machines at a glance, answer asks with one click, and keep the irreversible move behind an armed button. It is a Node-RED flow (`dashboard/flows.json`): one sheet, 38 nodes, rebuilt 2026-11-24. It speaks exactly the [MQTT contract](/system/contract): the four `State/*` topics in, `Action/feedback` out.

## Layout

Three columns on one UI tab:

- **Machine**: both nodes as ISA-101 banners. Per node: a colored banner line (`g100-01 - OK`, `OPEN ASK P0001`, `ALARM`) and one detail line (condition, conf, episode, updated, rssi). Node identity is derived from the message topic; a 5 s heartbeat re-renders age and online state.
- **Events**: one shared 20-row log for both nodes with node and batch columns. Batch is tracked from the live ask: a label row shows the batch it answered, a close row shows a batch still pending for late labeling, open rows show `-` (truthful: the batch id matures only after 15 samples).
- **Label & control**: node dropdown, batch id, kind (fault default), label slug; buttons LABEL / DISCARD / START / NORMAL / FAULT / RESET. RESET arms on the first press and confirms only if pressed again within 10 s; the flow has two outputs (publish message, last-command line) so arm and error states display without publishing.

One shared mqtt-out; `msg.topic` is set per selected node.

## Deploying the flow

The flow file is the source of truth. Import it through the Node-RED UI into a fresh sheet, or manage a full replace by API. Two deployment facts from the 2026-11-24 session:

- UI imports ADD nodes and duplicate same-id ones into new sheets. Do not layer imports on a live sheet; start clean or do a full-replace deploy.
- Broker credentials: the flow's mqtt-broker node keeps credentials by node id. A scripted full replace reads the existing flow via `GET /flows`, preserves broker credential fields, and posts the new set back.

Validation status: the flow passes a structural lint (all wire, group, tab and broker references resolve). No Node-RED runtime or broker is reachable from the docs build machine, so the lint is structural only; the user verified the dashboard live on 2026-11-24 (receiving and commands) on the rig network.
