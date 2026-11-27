<script setup lang="ts">
// Isometric hero scene as SVG: the editing surface is sceneData.ts; this
// file projects the world to 2D and paints it back to front. Painter's
// order replaces all 3D depth rules: there are no transform origins,
// z-fighting fixes or billboard rules to worry about.
// One exception: the operator figure stands BEHIND the desk, so it joins
// the sorted solids with a sort bias instead of the on-top billboard layer.
import { computed, ref } from 'vue'
import {
  arrows,
  cuboids,
  cyls,
  marks,
  pills,
  zones,
  type CuboidPart
} from './sceneData'

// Same projection as the CSS scene's rotateX(60deg) rotateZ(-45deg):
// u = (x + y)/sqrt(2), v = (y - x)/(2 sqrt(2)) - z * sin(60deg)
const C = Math.SQRT1_2
const K = Math.SQRT2 / 4
const S = Math.sqrt(3) / 2

const f = (n: number) => +n.toFixed(2)

const pt = (x: number, y: number, z: number): [number, number] => [
  f(C * (x + y)),
  f(K * (y - x) - S * z)
]

const poly = (pts: [number, number, number][]) =>
  pts.map((p) => pt(p[0], p[1], p[2]).join(',')).join(' ')

// Grays are pulled toward white so the world stays bright and elegant;
// tune this single factor to soften or deepen the whole gray family.
const GRAY_SOFT = 0.65

const mix = (kind: 'brand' | 'gray' | 'green' | 'yellow', pct: number) => {
  const v =
    kind === 'brand'
      ? 'var(--vp-c-brand-1)'
      : kind === 'green'
        ? 'var(--vp-c-green-1)'
        : kind === 'yellow'
          ? 'var(--led-yellow)'
          : 'var(--vp-c-text-1)'
  const p = Math.max(kind === 'gray' ? pct * GRAY_SOFT : pct, 2).toFixed(1)
  return `color-mix(in srgb, ${v} ${p}%, var(--vp-c-bg))`
}

const EDGE = 'color-mix(in srgb, var(--vp-c-text-1) 10%, transparent)'

interface Prim {
  k: 'poly' | 'fin' | 'side' | 'cap' | 'led' | 'ledTop' | 'text' | 'circle' | 'srect' | 'shadowE'
  key: number // larger is farther; painted first
  clipId?: string
  delay?: number
  pts?: string
  x1?: number
  y1?: number
  x2?: number
  y2?: number
  x?: number
  y?: number
  w?: number
  h?: number
  rx?: number
  ry?: number
  cx?: number
  cy?: number
  fill?: string
  stroke?: string
  sw?: number
  grad?: string
  blink?: boolean
  t?: string
  size?: number
  weight?: number
  transform?: string
}

// Painter key: the camera sits at (-x, +y, +z), so farther means larger
// x - y and smaller z.
const depth = (cx: number, cy: number, cz: number) => cx - cy - 0.8165 * cz

// Auto shadows: one ground ellipse per ground-level assembly (the cuboid's
// footprint plus any elevated parts touching it), squashed to the approved
// flat look and offset toward the lower-right; one ellipse per standing
// figure. Editing a solid moves its shadow automatically; no shadow table.
interface ShadowEllipse {
  zone: number
  wx: number // world center, for support lookup
  wy: number
  cx: number // projected center
  cy: number
  rx: number
  ry: number
  z: number
  furniture?: boolean // figure standing on a solid: sorts with the solids
}

const autoShadows = computed((): ShadowEllipse[] => {
  const boxes = cuboids.map((c) => ({
    zone: c.zone,
    x: c.box.x,
    y: c.box.y,
    w: c.box.w,
    d: c.box.d,
    z0: c.box.z ?? 0
  }))
  const out: ShadowEllipse[] = []
  for (const g of boxes.filter((b) => b.z0 === 0)) {
    let x0 = g.x
    let x1 = g.x + g.w
    let y0 = g.y
    let y1 = g.y + g.d
    const claimed = new Set<object>([g])
    let grew = true
    while (grew) {
      grew = false
      for (const b of boxes) {
        if (claimed.has(b) || b.z0 === 0) continue
        if (b.x < x1 + 2 && b.x + b.w > x0 - 2 && b.y < y1 + 2 && b.y + b.d > y0 - 2) {
          claimed.add(b)
          x0 = Math.min(x0, b.x)
          x1 = Math.max(x1, b.x + b.w)
          y0 = Math.min(y0, b.y)
          y1 = Math.max(y1, b.y + b.d)
          grew = true
        }
      }
    }
    const rw = (x1 - x0 + 12) / 2
    const rh = ((y1 - y0) * 0.66) / 2
    const r = Math.sqrt(rw * rw + rh * rh)
    const [u, v] = pt((x0 + x1) / 2 + 2, (y0 + y1) / 2 + 3, 1)
    out.push({ zone: g.zone, wx: (x0 + x1) / 2 + 2, wy: (y0 + y1) / 2 + 3, cx: u, cy: v, rx: f(C * r), ry: f(K * r), z: 1 })
  }
  for (const m of marks) {
    if (m.kind === 'person' || m.kind === 'bot') {
      const r = Math.sqrt(12 * 12 + 5.5 * 5.5)
      const fu = m.z > 0
      // furniture shadows center on the figure and get clipped to the
      // tabletop; ground shadows keep the lower-right offset
      const ox = fu ? 0 : 2
      const zz = fu ? m.z - 3 : 1
      const [u, v] = pt(m.x + ox, m.y + ox, zz)
      out.push({
        zone: m.zone, wx: m.x + ox, wy: m.y + ox, cx: u, cy: v,
        rx: f(C * r), ry: f(K * r), z: zz, furniture: fu
      })
    }
  }
  return out
})

// Furniture shadows join the sorted solids right after their support solid
// (a plain pre-solids layer would hide them under the table top), clipped
// to the support's top face so they never spill past the edge.
const furnitureShadows = computed(() =>
  autoShadows.value
    .filter((s) => s.furniture)
    .map((s, i) => {
      const support = cuboids.find((c) => {
        const b = c.box
        return (
          c.zone === s.zone &&
          (b.z ?? 0) === 0 &&
          b.h <= s.z + 2 &&
          s.wx > b.x - 8 && s.wx < b.x + b.w + 8 &&
          s.wy > b.y - 8 && s.wy < b.y + b.d + 8
        )
      })
      if (!support) return null
      const b = support.box
      const key = depth(b.x + b.w / 2, b.y + b.d / 2, b.h / 2) - 0.01
      const clip = poly([
        [b.x, b.y, s.z],
        [b.x + b.w, b.y, s.z],
        [b.x + b.w, b.y + b.d, s.z],
        [b.x, b.y + b.d, s.z]
      ])
      return { ...s, key, clipId: `fsh${i}`, clip }
    })
    .filter((s) => s !== null)
)

const active = ref(-1)
const dim = (zone: number) => active.value >= 0 && active.value !== zone
const ratio = (zone: number, pct: number) => pct * (dim(zone) ? 0.4 : 1)

// The hovered segment shifts from grey to the brand hue
const hue = (zone: number, kind: 'brand' | 'gray') => (active.value === zone ? 'brand' : kind)

// Status dots flat on the lid top: three grey LEDs blinking in a chase
// (staggered SMIL begin), like a real network device
const lidDots = (c: CuboidPart, key: number): Prim[] => {
  const { x, y, d } = c.box
  const zt = (c.box.z ?? 0) + c.box.h
  return [0, 1, 2].map((i) => {
    const [cx, cy] = pt(x + 4 + i * 4, y + d - 6, zt)
    return {
      k: 'ledTop',
      key,
      cx,
      cy,
      rx: 1.6,
      ry: 0.8,
      blink: true,
      delay: i * 0.53,
      fill: mix(hue(c.zone, 'gray'), ratio(c.zone, 60))
    }
  })
}

// Ask dashboard art on the monitor's left face (x = 168 plane): repaint the
// face dark, then bars growing +z and the flashing alarm triangle.
const screenArt = (c: CuboidPart, key: number): Prim[] => {
  const { x, y } = c.box
  const z = c.box.z ?? 0
  const zone = c.zone
  const bar = (t0: number, hgt: number) =>
    poly([[x, y + t0, z], [x, y + t0 + 8, z], [x, y + t0 + 8, z + hgt], [x, y + t0, z + hgt]])
  const [tx, ty] = pt(x, y, z + 34)
  return [
    { k: 'poly', key, pts: poly([[x, y, z + 34], [x, y, z], [x, y + 44, z], [x, y + 44, z + 34]]), fill: mix(hue(zone, 'gray'), ratio(zone, 30)) },
    { k: 'poly', key, pts: bar(5, 10), fill: mix(hue(zone, 'gray'), ratio(zone, 55)) },
    { k: 'poly', key, pts: bar(16, 16), fill: mix(hue(zone, 'gray'), ratio(zone, 55)) },
    { k: 'poly', key, pts: bar(27, 22), fill: mix(hue(zone, 'gray'), ratio(zone, 55)) },
    {
      k: 'poly', key,
      pts: poly([[x, y + 22, z + 31], [x, y + 13, z + 17], [x, y + 31, z + 17]]),
      fill: dim(zone) ? mix(hue(zone, 'gray'), 30) : mix(hue(zone, 'gray'), 65),
      blink: true, stroke: 'none'
    },
    {
      // y-down face frame (origin at the face top edge) so the glyph is
      // upright while skewing with the face
      k: 'text', key, t: '!', x: 22, y: 16, size: 9, weight: 700,
      fill: 'var(--vp-c-text-1)',
      transform: `matrix(${C} ${K} 0 ${S} ${tx} ${ty})`
    }
  ]
}

const prims = computed<Prim[]>(() => {
  const out: Prim[] = []

  // Solid specs with raw painter keys, then a support pass: any elevated
  // solid paints right after the solid it stands on. Raw center keys can
  // otherwise order a big solid behind-small-object combinations wrongly
  // (an antenna on a lid, a keyboard on a desk).
  type Spec = {
    part: CuboidPart | CylPart
    zone: number
    x0: number
    x1: number
    y0: number
    y1: number
    z0: number
    top: number
    key: number
  }
  const specs: Spec[] = []
  for (const c of cuboids) {
    const { x, y, w, d, h } = c.box
    const z0 = c.box.z ?? 0
    specs.push({
      part: c,
      zone: c.zone,
      x0: x,
      x1: x + w,
      y0: y,
      y1: y + d,
      z0,
      top: z0 + h,
      key: depth(x + w / 2, y + d / 2, z0 + h / 2)
    })
  }
  for (const c of cyls) {
    const z0 = c.z ?? 0
    specs.push({
      part: c,
      zone: c.zone,
      x0: c.cx - c.d / 2,
      x1: c.cx + c.d / 2,
      y0: c.cy - c.d / 2,
      y1: c.cy + c.d / 2,
      z0,
      top: z0 + c.h,
      key: depth(c.cx, c.cy, z0 + c.h / 2)
    })
  }
  for (const s of [...specs].sort((a, b) => a.z0 - b.z0)) {
    if (s.z0 <= 0) continue
    const cx = (s.x0 + s.x1) / 2
    const cy = (s.y0 + s.y1) / 2
    for (const t of specs) {
      if (t === s || t.zone !== s.zone || t.top > s.z0 + 2) continue
      if (cx > t.x0 - 8 && cx < t.x1 + 8 && cy > t.y0 - 8 && cy < t.y1 + 8) {
        s.key = Math.min(s.key, t.key - 0.01)
      }
    }
  }
  const keyOf = new Map(specs.map((s) => [s.part, s.key]))

  for (const c of cuboids) {
    const { x, y, w, d, h } = c.box
    const z = c.box.z ?? 0
    const t = z + h
    const key = keyOf.get(c)!
    out.push(
      // top faces are pure white (the sheet convention); hue lives on the sides
      { k: 'poly', key, pts: poly([[x, y, t], [x + w, y, t], [x + w, y + d, t], [x, y + d, t]]), fill: 'var(--vp-c-bg)', stroke: EDGE },
      { k: 'poly', key, pts: poly([[x, y, t], [x, y, z], [x, y + d, z], [x, y + d, t]]), fill: mix(hue(c.zone, c.tone.kind), ratio(c.zone, c.tone.left)), stroke: EDGE },
      { k: 'poly', key, pts: poly([[x, y + d, t], [x, y + d, z], [x + w, y + d, z], [x + w, y + d, t]]), fill: mix(hue(c.zone, c.tone.kind), ratio(c.zone, c.tone.right)), stroke: EDGE }
    )
    if (c.fins) {
      const finStroke = mix(hue(c.zone, 'gray'), 14)
      for (let yi = y + 6; yi < y + d; yi += 7) {
        const [x1, y1] = pt(x, yi, z)
        const [x2, y2] = pt(x, yi, t)
        out.push({ k: 'fin', key, x1, y1, x2, y2, stroke: finStroke })
      }
      for (let xi = x + 6; xi < x + w; xi += 7) {
        const [x1, y1] = pt(xi, y + d, z)
        const [x2, y2] = pt(xi, y + d, t)
        out.push({ k: 'fin', key, x1, y1, x2, y2, stroke: finStroke })
      }
    }
    if (c.led) {
      const [cx, cy] = pt(x + 5.5, y + d, z + h - 3.5)
      out.push({ k: 'led', key, cx, cy, rx: 1.1, blink: true, fill: mix('yellow', ratio(c.zone, 100)) })
    }
    if (c.leds) out.push(...lidDots(c, key))
    if (c.screen) out.push(...screenArt(c, key))
  }

  // Cylinders replicate the CSS cheat: billboard side rect + small flat
  // caps (a plane ellipse d x d/2 projects to 0.395d x 0.198d).
  cyls.forEach((c, ci) => {
    const key = keyOf.get(c)!
    const z = c.z ?? 0
    const [u, v] = pt(c.cx, c.cy, z)
    const [tu, tv] = pt(c.cx, c.cy, z + c.h)
    const cap = { rx: f(0.3953 * c.d), ry: f(0.1976 * c.d) }
    out.push(
      { k: 'cap', key, cx: u, cy: f(v - 0.43), ...cap, fill: mix(hue(c.zone, c.tone.kind), ratio(c.zone, c.tone.bottom)) },
      { k: 'side', key, x: f(u - c.d / 2), y: tv, w: c.d, h: f(S * c.h), grad: `cyl${ci}` },
      { k: 'cap', key, cx: u, cy: tv, ...cap, fill: 'var(--vp-c-bg)', stroke: 'color-mix(in srgb, var(--vp-c-text-1) 12%, transparent)' }
    )
    if (c.led) {
      // LED dot on the middle of the billboard side rect
      out.push({ k: 'led', key, cx: u, cy: f(tv + (S * c.h) / 2), rx: 1.1, blink: true, fill: mix('yellow', ratio(c.zone, 100)) })
    }
  })

  // The operator figure: screen-space shapes, but sorted among the solids
  // (bias pushes it behind the desk so the desk front hides the legs).
  const person = marks.find((m) => m.kind === 'person')
  if (person) {
    const [au, av] = pt(person.x, person.y, person.z)
    const key = depth(person.x, person.y, person.z + 28) + 60
    const pstroke = mix(hue(person.zone, 'gray'), 25)
    out.push(
      { k: 'circle', key, cx: au, cy: f(av - 49.5), rx: 6.5, fill: mix(hue(person.zone, 'gray'), ratio(person.zone, 55)), stroke: pstroke, sw: 0.75 },
      { k: 'srect', key, x: f(au - 10), y: f(av - 41), w: 20, h: 24, rx: 6, fill: mix(hue(person.zone, 'gray'), ratio(person.zone, 40)), stroke: pstroke, sw: 0.75 },
      { k: 'srect', key, x: f(au - 7), y: f(av - 16), w: 6, h: 16, fill: mix(hue(person.zone, 'gray'), ratio(person.zone, 58)), stroke: pstroke, sw: 0.75 },
      { k: 'srect', key, x: f(au + 1), y: f(av - 16), w: 6, h: 16, fill: mix(hue(person.zone, 'gray'), ratio(person.zone, 58)), stroke: pstroke, sw: 0.75 }
    )
  }

  // Furniture shadows: see furnitureShadows
  for (const s of furnitureShadows.value) {
    out.push({ k: 'shadowE', key: s.key, cx: s.cx, cy: s.cy, rx: s.rx, ry: s.ry, zone: s.zone, clipId: s.clipId })
  }

  return out.sort((a, b) => b.key - a.key)
})

// One gradient per cylinder, hue-aware: the hovered segment's cylinders
// turn brand blue together with the rest of its solids
const cylDefs = computed(() =>
  cyls.map((c, i) => ({
    id: `cyl${i}`,
    stops: [
      { o: 0, c: mix(hue(c.zone, c.tone.kind), ratio(c.zone, c.tone.side[2])) },
      { o: 0.3, c: mix(hue(c.zone, c.tone.kind), ratio(c.zone, c.tone.side[0])) },
      { o: 0.55, c: mix(hue(c.zone, c.tone.kind), ratio(c.zone, c.tone.side[1])) },
      { o: 1, c: mix(hue(c.zone, c.tone.kind), ratio(c.zone, c.tone.side[2])) }
    ]
  }))
)

const G = 250
const ground = poly([[0, 0, 0], [G, 0, 0], [G, G, 0], [0, G, 0]])

// Low rim lips rising from the two near edges of the ground slab
const lipGy = poly([[0, 0, 8], [0, 0, 0], [0, G, 0], [0, G, 8]])
const lipGx = poly([[0, G, 8], [0, G, 0], [G, G, 0], [G, G, 8]])

const grid = computed(() => {
  const lines: { x1: number; y1: number; x2: number; y2: number }[] = []
  for (let i = 0; i <= G; i += 25) {
    const [ax, ay] = pt(i, 0, 0)
    const [bx, by] = pt(i, G, 0)
    lines.push({ x1: ax, y1: ay, x2: bx, y2: by })
    const [cx, cy] = pt(0, i, 0)
    const [dx, dy] = pt(G, i, 0)
    lines.push({ x1: cx, y1: cy, x2: dx, y2: dy })
  }
  return lines
})

const tintPts = (i: number) => {
  const z = zones[i]
  return poly([[z.ox, z.oy, 0], [z.ox + 110, z.oy, 0], [z.ox + 110, z.oy + 110, 0], [z.ox, z.oy + 110, 0]])
}

const tintStyle = (i: number) => {
  const z = zones[i]
  // idle floor is grey; the hovered segment gets the brand accent
  return {
    fill: `color-mix(in srgb, var(--vp-c-${active.value === i ? 'brand-1' : 'text-1'}) ${
      active.value === i ? 18 : dim(i) ? 4 : z.tint * GRAY_SOFT
    }%, transparent)`,
    stroke:
      active.value === i
        ? 'var(--vp-c-brand-1)'
        : dim(i)
          ? 'color-mix(in srgb, var(--vp-c-divider) 50%, transparent)'
          : 'color-mix(in srgb, var(--vp-c-text-1) 35%, var(--vp-c-bg))'
  }
}

// Ground-layer shadows: everything except furniture shadows
const groundShadows = computed(() => autoShadows.value.filter((s) => !s.furniture))

// Ground arrows, two-way, one constant style: very light semi-transparent
// blue, so they read as flow hints without competing with the solids
const arrowIdle = 'color-mix(in srgb, var(--vp-c-text-1) 30%, transparent)'
const arrowLit = 'color-mix(in srgb, var(--vp-c-brand-1) 45%, transparent)'

const arrowView = computed(() =>
  arrows.map((a) => {
    const [x1, y1] = pt(a.from[0], a.from[1], 1)
    const [x2, y2] = pt(a.to[0], a.to[1], 1)
    const len = Math.hypot(x2 - x1, y2 - y1)
    const ux = (x2 - x1) / len
    const uy = (y2 - y1) / len
    const head = (x: number, y: number, du: number, dv: number) => {
      const bx = x - du * 8
      const by = y - dv * 8
      const px = -dv * 3
      const py = du * 3
      return {
        pts: `${f(x)},${f(y)} ${f(bx + px)},${f(by + py)} ${f(bx - px)},${f(by - py)}`,
        lx: f(bx),
        ly: f(by)
      }
    }
    const ha = head(x1, y1, -ux, -uy)
    const hb = head(x2, y2, ux, uy)
    return {
      x1: ha.lx,
      y1: ha.ly,
      x2: hb.lx,
      y2: hb.ly,
      headA: ha.pts,
      headB: hb.pts,
      lit: a.zones.includes(active.value)
    }
  })
)

// Invisible hit polygon per zone: convex hull of every cuboid's projected
// corners, so tall solids like the cabinet panel stay hoverable even where
// they rise above the quadrant tint diamond.
const hull = (pts: [number, number][]) => {
  const p = [...pts].sort((a, b) => a[0] - b[0] || a[1] - b[1])
  if (p.length < 3) return ''
  const cross = (o: [number, number], a: [number, number], b: [number, number]) =>
    (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
  const lower: [number, number][] = []
  for (const q of p) {
    while (lower.length >= 2 && cross(lower[lower.length - 2], lower[lower.length - 1], q) <= 0) lower.pop()
    lower.push(q)
  }
  const upper: [number, number][] = []
  for (let i = p.length - 1; i >= 0; i--) {
    const q = p[i]
    while (upper.length >= 2 && cross(upper[upper.length - 2], upper[upper.length - 1], q) <= 0) upper.pop()
    upper.push(q)
  }
  lower.pop()
  upper.pop()
  return lower.concat(upper).map((q) => q.join(',')).join(' ')
}

const zoneHits = computed(() =>
  zones.map((z, i) => {
    const pts: [number, number][] = []
    for (const c of cuboids.filter((q) => q.zone === i)) {
      const { x, y, w, d, h } = c.box
      const z0 = c.box.z ?? 0
      for (const dz of [0, h])
        for (const dx of [0, w])
          for (const dy of [0, d]) pts.push(pt(x + dx, y + dy, z0 + dz))
    }
    return hull(pts)
  })
)

const anchor = (x: number, y: number, z: number) => {
  const [u, v] = pt(x, y, z)
  return `translate(${u}, ${v})`
}

// Billboards rendered in the on-top layer (the person is in the solids).
const billboardMarks = computed(() => marks.filter((m) => m.kind !== 'person'))
const person = computed(() => marks.find((m) => m.kind === 'person')!)
const personAnchor = computed(() => pt(person.value.x, person.value.y, person.value.z))

// Hover via event delegation: the tint <a> and the zone's billboards carry
// data-zone, so moving between them does not flicker the state.
const onOver = (e: Event) => {
  const t = (e.target as Element).closest('[data-zone]')
  active.value = t ? Number((t as SVGElement).getAttribute('data-zone')) : -1
}

// Pill colors
const pillBg = (zone: number) =>
  dim(zone)
    ? 'color-mix(in srgb, var(--vp-c-bg-soft) 45%, transparent)'
    : 'var(--vp-c-bg-soft)'
const pillStroke = (zone: number) =>
  active.value === zone
    ? 'var(--vp-c-brand-1)'
    : dim(zone)
      ? 'transparent'
      : 'var(--vp-c-divider)'
const pillText = (zone: number) =>
  active.value === zone
    ? 'var(--vp-c-brand-1)'
    : dim(zone)
      ? 'var(--vp-c-text-3)'
      : 'var(--vp-c-text-1)'
</script>

<template>
  <div class="hero-scene">
    <div class="scaler">
      <svg
        class="scene-svg"
        viewBox="-20 -112 392 210"
        role="img"
        aria-label="Isometric scene of the system: learn, publish, ask, feedback"
        @pointerover="onOver"
        @pointerleave="active = -1"
      >
        <defs>
          <radialGradient id="iso-shadow-g">
            <stop offset="0" stop-color="color-mix(in srgb, var(--vp-c-text-1) 18%, transparent)" />
            <stop offset="1" stop-color="transparent" />
          </radialGradient>
          <linearGradient v-for="g in cylDefs" :key="g.id" :id="g.id" x1="0" y1="0" x2="1" y2="0">
            <stop v-for="(s, i) in g.stops" :key="i" :offset="s.o" :stop-color="s.c" />
          </linearGradient>
          <clipPath id="sheet-clip">
            <rect x="-15" y="-24" width="30" height="24" rx="3" />
          </clipPath>
          <clipPath v-for="c in furnitureShadows" :key="c.clipId" :id="c.clipId">
            <polygon :points="c.clip" />
          </clipPath>
        </defs>

        <!-- ground + grid + rim lips: inert, hover passes through -->
        <g class="no-hit">
          <polygon :points="ground" :style="{ fill: 'var(--vp-c-bg-alt)' }" />
          <polygon
            :points="lipGy"
            :style="{ fill: 'color-mix(in srgb, var(--vp-c-text-1) 9%, var(--vp-c-bg))' }"
            stroke="none"
          />
          <polygon
            :points="lipGx"
            :style="{ fill: 'color-mix(in srgb, var(--vp-c-text-1) 5%, var(--vp-c-bg))' }"
            stroke="none"
          />
          <line
            v-for="(l, i) in grid"
            :key="'gl' + i"
            v-bind="l"
            :style="{ stroke: 'color-mix(in srgb, var(--vp-c-text-1) 8%, transparent)' }"
            stroke-width="1"
          />
        </g>

        <!-- quadrant tints: the hover/click targets -->
        <a
          v-for="(z, i) in zones"
          :key="z.key"
          :href="z.link"
          :data-zone="i"
          class="zone-a"
          :aria-label="z.title"
          @focus="active = i"
          @blur="active = -1"
        >
          <polygon :points="zoneHits[i]" fill="transparent" stroke="none" />
          <polygon :points="tintPts(i)" :style="tintStyle(i)" stroke-width="1" />
        </a>

        <!-- auto contact shadows on the ground -->
        <g class="no-hit">
          <ellipse
            v-for="(s, i) in groundShadows"
            :key="'sh' + i"
            :cx="s.cx"
            :cy="s.cy"
            :rx="s.rx"
            :ry="s.ry"
            fill="url(#iso-shadow-g)"
            :opacity="dim(s.zone) ? 0.35 : 1"
          />
        </g>

        <!-- flow arrows on the ground -->
        <g class="no-hit">
          <g v-for="(a, i) in arrowView" :key="'ar' + i">
            <line
              :x1="a.x1"
              :y1="a.y1"
              :x2="a.x2"
              :y2="a.y2"
              :style="{ stroke: a.lit ? arrowLit : arrowIdle }"
              stroke-width="2"
            />
            <polygon :points="a.headA" :style="{ fill: a.lit ? arrowLit : arrowIdle }" stroke="none" />
            <polygon :points="a.headB" :style="{ fill: a.lit ? arrowLit : arrowIdle }" stroke="none" />
          </g>
        </g>

        <!-- solids, back to front (inert: hover passes through to the zone) -->
        <g class="no-hit">
        <template v-for="(pr, i) in prims" :key="i">
          <polygon
            v-if="pr.k === 'poly'"
            :points="pr.pts"
            :style="{ fill: pr.fill, stroke: pr.stroke }"
            stroke-width="1"
            stroke-linejoin="round"
          >
            <animate
              v-if="pr.blink"
              attributeName="opacity"
              values="1;1;0.25;0.25"
              keyTimes="0;0.5;0.55;1"
              dur="1.1s"
              repeatCount="indefinite"
            />
          </polygon>
          <line
            v-else-if="pr.k === 'fin'"
            :x1="pr.x1"
            :y1="pr.y1"
            :x2="pr.x2"
            :y2="pr.y2"
            :style="{ stroke: pr.stroke }"
            stroke-width="1"
          />
          <rect
            v-else-if="pr.k === 'side'"
            :x="pr.x"
            :y="pr.y"
            :width="pr.w"
            :height="pr.h"
            :fill="`url(#${pr.grad})`"
          />
          <ellipse
            v-else-if="pr.k === 'cap'"
            :cx="pr.cx"
            :cy="pr.cy"
            :rx="pr.rx"
            :ry="pr.ry"
            :fill="pr.fill"
            :stroke="pr.stroke"
            stroke-width="1"
          />
          <circle
            v-else-if="pr.k === 'led' || pr.k === 'circle'"
            :cx="pr.cx"
            :cy="pr.cy"
            :r="pr.rx"
            :fill="pr.fill"
            :stroke="pr.stroke"
            :stroke-width="pr.sw"
          >
            <!-- SMIL blink: runs even when prefers-reduced-motion gates CSS animations -->
            <animate
              v-if="pr.blink"
              attributeName="opacity"
              values="1;1;0.15;0.15"
              keyTimes="0;0.55;0.65;1"
              dur="1.6s"
              repeatCount="indefinite"
            />
          </circle>
          <rect
            v-else-if="pr.k === 'srect'"
            :x="pr.x"
            :y="pr.y"
            :width="pr.w"
            :height="pr.h"
            :rx="pr.rx"
            :fill="pr.fill"
            :stroke="pr.stroke"
            :stroke-width="pr.sw"
          />
          <ellipse
            v-else-if="pr.k === 'shadowE'"
            :cx="pr.cx"
            :cy="pr.cy"
            :rx="pr.rx"
            :ry="pr.ry"
            fill="url(#iso-shadow-g)"
            :opacity="dim(pr.zone) ? 0.35 : 1"
            :clip-path="pr.clipId ? `url(#${pr.clipId})` : undefined"
          />
          <ellipse
            v-else-if="pr.k === 'ledTop'"
            :cx="pr.cx"
            :cy="pr.cy"
            :rx="pr.rx"
            :ry="pr.ry"
            :fill="pr.fill"
          >
            <animate
              v-if="pr.blink"
              attributeName="opacity"
              values="1;1;0.2;0.2"
              keyTimes="0;0.2;0.35;1"
              dur="1.6s"
              :begin="pr.delay ? `-${pr.delay}s` : '0s'"
              repeatCount="indefinite"
            />
          </ellipse>
          <text
            v-else-if="pr.k === 'text'"
            :x="pr.x"
            :y="pr.y"
            :transform="pr.transform"
            :font-size="pr.size"
            :font-weight="pr.weight"
            :fill="pr.fill"
            text-anchor="middle"
          >{{ pr.t }}</text>
        </template>
        </g>

        <!-- upright figures and icons, per zone (on top) -->
        <a
          v-for="m in billboardMarks"
          :key="m.kind"
          :href="zones[m.zone].link"
          :data-zone="m.zone"
          tabindex="-1"
          class="zone-a"
        >
          <g :transform="anchor(m.x, m.y, m.z)">
            <!-- contract sheet: stacked pages, header band, ruled grid -->
            <g v-if="m.kind === 'sheet'">
              <rect
                x="-13" y="-27" width="30" height="24" rx="3"
                :style="{
                  fill: dim(m.zone) ? 'color-mix(in srgb, var(--vp-c-bg) 70%, transparent)' : 'var(--vp-c-bg)',
                  stroke: dim(m.zone) ? mix(hue(m.zone, 'gray'), 35) : mix(hue(m.zone, 'gray'), 45)
                }"
                stroke-width=".5"
              />
              <rect
                x="-15" y="-24" width="30" height="24" rx="3"
                :style="{
                  fill: dim(m.zone) ? 'color-mix(in srgb, var(--vp-c-bg) 70%, transparent)' : 'var(--vp-c-bg)',
                  stroke: dim(m.zone) ? mix(hue(m.zone, 'gray'), 35) : mix(hue(m.zone, 'gray'), 70)
                }"
                stroke-width=".5"
              />
              <g clip-path="url(#sheet-clip)">
                <rect x="-15" y="-24" width="30" height="7" :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 25)) }" />
                <circle cx="-10" cy="-20.5" r="1.8" :style="{ fill: mix(hue(m.zone, 'gray'), 65) }" />
                <line x1="-4" y1="-24" x2="-4" y2="0" :style="{ stroke: mix(hue(m.zone, 'gray'), ratio(m.zone, 25)) }" />
                <line x1="7" y1="-24" x2="7" y2="0" :style="{ stroke: mix(hue(m.zone, 'gray'), ratio(m.zone, 25)) }" />
                <line x1="-15" y1="-14" x2="15" y2="-14" :style="{ stroke: mix(hue(m.zone, 'gray'), ratio(m.zone, 25)) }" />
                <line x1="-15" y1="-5" x2="15" y2="-5" :style="{ stroke: mix(hue(m.zone, 'gray'), ratio(m.zone, 25)) }" />
              </g>
            </g>
            <!-- rising value -->
            <g v-else-if="m.kind === 'value'">
              <rect x="-15" y="-5.5" width="30" height="1.5" :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 40)) }" />
              <rect x="-14" y="-14" width="6" height="8" rx="1" :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 55)) }" />
              <rect x="-6" y="-20" width="6" height="14" rx="1" :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 55)) }" />
              <rect x="2" y="-26" width="6" height="20" rx="1" :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 55)) }" />
              <text x="17" y="-23" text-anchor="end" font-size="14" font-weight="700" :style="{ fill: dim(m.zone) ? mix(hue(m.zone, 'gray'), 40) : mix(hue(m.zone, 'gray'), 70) }">$</text>
            </g>
            <!-- agent, bobbing and blinking (eyes); gold is reserved for sensor/MCU -->
            <g v-else-if="m.kind === 'bot'" class="bob">
              <rect
                x="-1" y="-36" width="2" height="5"
                :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 32)), stroke: mix(hue(m.zone, 'gray'), 60) }"
                stroke-width=".25"
              />
              <rect
                x="-10" y="-31" width="20" height="16" rx="5"
                :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 44)), stroke: mix(hue(m.zone, 'gray'), 60) }"
                stroke-width=".25"
              />
              <circle cx="-4" cy="-23" r="2" :style="{ fill: dim(m.zone) ? mix('gray', 35) : 'var(--vp-c-text-1)' }">
                <animate
                  attributeName="opacity"
                  values="1;1;0.1;0.1;1;1"
                  keyTimes="0;0.4;0.46;0.54;0.6;1"
                  dur="3.2s"
                  repeatCount="indefinite"
                />
              </circle>
              <circle cx="4" cy="-23" r="2" :style="{ fill: dim(m.zone) ? mix('gray', 35) : 'var(--vp-c-text-1)' }">
                <animate
                  attributeName="opacity"
                  values="1;1;0.1;0.1;1;1"
                  keyTimes="0;0.4;0.46;0.54;0.6;1"
                  dur="3.2s"
                  repeatCount="indefinite"
                />
              </circle>
              <rect
                x="-7" y="-13" width="14" height="10" rx="3"
                :style="{ fill: mix(hue(m.zone, 'gray'), ratio(m.zone, 28)), stroke: mix(hue(m.zone, 'gray'), 60) }"
                stroke-width=".25"
              />
            </g>
          </g>
        </a>

        <!-- hit area for the operator figure (drawn among the solids) -->
        <a
          :href="zones[3].link"
          data-zone="3"
          tabindex="-1"
          class="zone-a"
          aria-label="Operator"
        >
          <circle
            :cx="personAnchor[0]"
            :cy="f(personAnchor[1] - 28)"
            r="28"
            fill="transparent"
          />
        </a>

        <!-- upright pills, on top -->
        <a
          v-for="p in pills"
          :key="p.label"
          :href="zones[p.zone].link"
          :data-zone="p.zone"
          tabindex="-1"
          class="zone-a"
        >
          <g :transform="anchor(p.x, p.y, p.z)">
            <rect
              x="-30"
              y="-20"
              width="60"
              height="20"
              rx="10"
              :style="{ fill: pillBg(p.zone), stroke: pillStroke(p.zone) }"
            />
            <text y="-6" text-anchor="middle" class="pill-text" :style="{ fill: pillText(p.zone) }">{{ p.label }}</text>
          </g>
        </a>
      </svg>
    </div>

    <div class="caption">
      <Transition name="pop" mode="out-in">
        <div v-if="active >= 0" :key="active" class="caption-card">
          <p class="caption-title">{{ zones[active].title }}</p>
          <p class="caption-text">{{ zones[active].text }}</p>
        </div>
        <p v-else class="caption-hint">Hover for description</p>
      </Transition>
    </div>
  </div>
</template>

<style scoped>
.hero-scene {
  /* bright yellow for the blinking node/sensor LEDs */
  --led-yellow: #f59e0b;
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
}

:global(.dark) .hero-scene {
  --led-yellow: #fbbf24;
}

/* Responsive: scale the whole scene, caption stays full size */
.scaler {
  transform: scale(0.55);
}

@media (min-width: 640px) {
  .scaler { transform: scale(0.8); }
}

@media (min-width: 960px) {
  .scaler { transform: scale(1); }
}

.scene-svg {
  width: 520px;
  height: 279px;
  overflow: visible;
}

@media (prefers-reduced-motion: no-preference) {
  .scene-svg {
    animation: hero-float 7s ease-in-out infinite;
  }
}

@keyframes hero-float {
  0%,
  100% {
    transform: translateY(0);
  }

  50% {
    transform: translateY(-8px);
  }
}

.zone-a {
  cursor: pointer;
  outline: none;
  text-decoration: none;
}

/* Ground, shadows, arrows and solids never take the pointer: hit-testing
   passes through to the quadrant tint underneath, matching the old CSS-3D
   pointer-events behaviour. */
.no-hit {
  pointer-events: none;
}

@media (prefers-reduced-motion: no-preference) {
  .bob {
    animation: bot-bob 2.8s ease-in-out infinite;
  }
}

@keyframes bot-bob {
  0%,
  100% {
    transform: translateY(0);
  }

  50% {
    transform: translateY(-4px);
  }
}

.pill-text {
  font-size: 11px;
  font-weight: 600;
}

/* Caption card below the scene */
.caption {
  margin-top: 10px;
  height: 96px;
  width: min(300px, 80vw);
  display: flex;
  align-items: flex-start;
  justify-content: center;
  text-align: center;
}

.caption-hint {
  margin: 8px 0 0;
  font-size: 12px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--vp-c-text-3);
}

.caption-title {
  margin: 0;
  font-size: 15px;
  font-weight: 600;
  color: var(--vp-c-brand-1);
}

.caption-text {
  margin: 6px 0 0;
  font-size: 13px;
  line-height: 1.5;
  color: var(--vp-c-text-2);
}

.pop-enter-active,
.pop-leave-active {
  transition: opacity 0.15s ease, transform 0.15s ease;
}

.pop-enter-from,
.pop-leave-to {
  opacity: 0;
  transform: translateY(4px);
}
</style>
