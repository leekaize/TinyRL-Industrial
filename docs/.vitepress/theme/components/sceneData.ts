// Scene data for the isometric hero: one table of solids per story beat.
// This file is the editing surface for the scene: move a solid by editing
// its numbers, add one by adding an entry. World coords: x to the
// upper-right, y to the lower-right, z up. Ground 250x250, quadrants
// 110x110 at (8,8) learn, (132,8) publish, (132,132) ask, (8,132) feedback.
import { type CylTone, type Tone, T } from './isoTones'

export interface Box {
  x: number
  y: number
  w: number
  d: number
  h: number
  z?: number
}

export interface CuboidPart {
  zone: number
  box: Box
  tone: Tone
  fins?: boolean
  led?: boolean // brand LED on the box's right (+y) face, blinks
  leds?: boolean // three static brand LEDs on the box's left (-x) face
  screen?: boolean // ask dashboard: alarm + bars on the left (-x) face
}

export interface CylPart {
  zone: number
  cx: number
  cy: number
  d: number // diameter
  h: number
  z?: number // base height
  tone: CylTone
  led?: boolean // yellow LED on the side billboard, blinks
}

export interface ArrowPart {
  from: [number, number] // ground coords, drawn at z = 1
  to: [number, number]
  zones: [number, number] // the segments it connects; both light it on hover
}

export interface ZoneDef {
  key: string
  ox: number
  oy: number
  tint: number // resting tint ratio (%)
  title: string
  text: string
  link: string
}

export const zones: ZoneDef[] = [
  {
    key: 'learn',
    ox: 8,
    oy: 8,
    tint: 12,
    title: 'Self-contained intelligence',
    text: 'Each node learns its machine\'s normal states on-device, at 1 Hz. No buffers, no cloud round-trip.',
    link: '/system/architecture'
  },
  {
    key: 'publish',
    ox: 132,
    oy: 8,
    tint: 8,
    title: 'It speaks conditions, not data',
    text: 'Bounded events instead of raw streams: about 22x fewer messages on the recorded tour.',
    link: '/system/contract'
  },
  {
    key: 'ask',
    ox: 132,
    oy: 132,
    tint: 9,
    title: 'It asks when it doesn\'t know',
    text: 'An unmatched alarm opens an episode and asks by id. One label teaches the whole fleet.',
    link: '/guide/lifecycle'
  },
  {
    key: 'feedback',
    ox: 8,
    oy: 132,
    tint: 9,
    title: 'Humans and agents stay in the loop',
    text: 'One contract serves the dashboard and the agent. Humans keep the irreversible moves.',
    link: '/guide/agent'
  }
]

// LEARN: motor with finned body and sensor probe, wired to an MCU mounted
// on a cabinet panel whose back is at the FAR edge (y = 2): the MCU face
// points +y, toward the feedback quadrant. Sensor + MCU are the only
// coloured solids (page-accent blue): they pair with the blue "Edge
// nodes" words in the homepage subtitle.
export const cuboids: CuboidPart[] = [
  // motor
  { zone: 0, box: { x: 28, y: 57, w: 54, d: 36, h: 6 }, tone: T.steel },
  { zone: 0, box: { x: 26, y: 63, w: 6, d: 24, h: 22, z: 8 }, tone: T.bell },
  { zone: 0, box: { x: 30, y: 59, w: 44, d: 32, h: 26, z: 6 }, tone: T.steel, fins: true },
  { zone: 0, box: { x: 74, y: 71, w: 8, d: 8, h: 6, z: 14 }, tone: T.dark },
  { zone: 0, box: { x: 36, y: 61, w: 18, d: 10, h: 8, z: 32 }, tone: T.steelHi },
  // cabinet panel + two MCU chips (brand blue, blinking yellow LEDs)
  { zone: 0, box: { x: 50, y: 12, w: 44, d: 5, h: 40 }, tone: T.stand },
  { zone: 0, box: { x: 54, y: 22, w: 8, d: 5, h: 6, z: 25 }, tone: T.accent, led: true },
  { zone: 0, box: { x: 74, y: 22, w: 8, d: 5, h: 6, z: 25 }, tone: T.accent, led: true },
  // sensor puck on the housing's right face, same blue as the MCUs
  { zone: 0, box: { x: 44, y: 91, w: 8, d: 5, h: 6, z: 22 }, tone: T.accent, led: true },
  // PUBLISH: gateway with lid, status dots on top, two rear antennas
  { zone: 1, box: { x: 160, y: 48, w: 32, d: 22, h: 11 }, tone: T.router },
  { zone: 1, box: { x: 162, y: 50, w: 28, d: 18, h: 3, z: 11 }, tone: T.dark, leds: true },
  // ASK: thin dashboard slab, wide screen on its left face toward the operator
  { zone: 2, box: { x: 166, y: 163, w: 26, d: 48, h: 8 }, tone: T.stand },
  { zone: 2, box: { x: 178, y: 165, w: 10, d: 44, h: 34, z: 8 }, tone: T.monitor, screen: true },
  // FEEDBACK: desk with keyboard facing the dashboard (+x direction)
  { zone: 3, box: { x: 67, y: 168, w: 22, d: 40, h: 14 }, tone: T.steel },
  { zone: 3, box: { x: 70, y: 188, w: 8, d: 18, h: 3, z: 14 }, tone: T.dark }
]

export const cyls: CylPart[] = [
  // one small sensor on the motor roof, blue like the MCUs
  { zone: 0, cx: 63, cy: 64, d: 5, h: 5, z: 32, tone: T.probe, led: true },
  { zone: 0, cx: 77, cy: 14, d: 2, h: 10, z: 40, tone: T.rod },
  // antennas: short thick rod planted mid-lid, never at the far edge
  { zone: 1, cx: 165, cy: 54, d: 4, h: 16, z: 14, tone: T.rod },
  { zone: 1, cx: 187, cy: 54, d: 4, h: 16, z: 14, tone: T.rod },
]

// Ground arrows showing the story flow: learn <-> publish <-> ask <-> feedback.
// Arrows keep one constant style: they never react to hovered zones.
export const arrows: ArrowPart[] = [
  { from: [100, 60], to: [140, 60], zones: [0, 1] },
  { from: [185, 105], to: [185, 142], zones: [1, 2] },
  { from: [145, 185], to: [105, 185], zones: [2, 3] }
]

// Upright labels (pill badges) anchored at ground points.
export const pills: { zone: number; x: number; y: number; z: number; label: string }[] = [
  { zone: 0, x: 12, y: 12, z: 2, label: 'Learn' },
  { zone: 1, x: 238, y: 12, z: 2, label: 'Publish' },
  { zone: 2, x: 238, y: 238, z: 2, label: 'Ask' },
  { zone: 3, x: 6, y: 250, z: 2, label: 'Feedback' }
]

// Upright figures/icons anchored at ground points, z = anchor height.
export const marks: { zone: number; kind: 'sheet' | 'value' | 'person' | 'bot'; x: number; y: number; z: number }[] = [
  { zone: 1, kind: 'sheet', x: 196, y: 96, z: 2 }, // standard contract
  { zone: 3, kind: 'person', x: 35, y: 190, z: 0 }, // operator behind the desk
  { zone: 3, kind: 'bot', x: 75, y: 185, z: 18 } // agent on the tabletop
]
