// Shared tone system for the isometric primitives.
//
// Light comes from the upper-left: top faces are lightest, left faces mid,
// right faces darkest. Tones are RATIO percentages (%), resolved by the
// primitive components against theme variables, so every part follows
// light/dark mode, and the dim state is just a ratio scale.
// The world is deliberately GRAYSCALE (gray tones mix against the text
// color); only the MCU chips and sensors carry colour, in the page-accent
// brand blue, so they read as the focus.

export type Kind = 'brand' | 'gray'

export interface Tone {
  kind: Kind
  top: number
  left: number
  right: number
}

// Cuboid tone with fixed lighting deltas: left = base +16, right = base +10.
export const brandTone = (base: number): Tone => ({
  kind: 'brand',
  top: base,
  left: base + 16,
  right: base + 10
})

const grayTone = (base: number): Tone => ({
  kind: 'gray',
  top: base,
  left: base + 16,
  right: base + 10
})

export interface CylTone {
  kind: Kind
  top: number
  // light / mid / dark stops of the side gradient, light band toward the left
  side: [number, number, number]
  bottom: number
}

const brandCyl = (base: number): CylTone => ({
  kind: 'brand',
  top: base + 8,
  side: [base + 16, base + 2, Math.max(base - 20, 4)],
  bottom: Math.max(base - 20, 4)
})

const grayCyl = (base: number): CylTone => ({
  kind: 'gray',
  top: base + 8,
  side: [base + 16, base + 2, Math.max(base - 20, 4)],
  bottom: Math.max(base - 20, 4)
})

// Scene palette. Names are semantic; solids reference one of these instead
// of picking ad-hoc colors (consumed from sceneData.ts).
export const T = {
  steel: grayTone(14), // motor body, base, desk, panel
  steelHi: grayTone(26), // junction box on the motor
  bell: grayTone(22), // motor rear bell
  router: grayTone(28),
  monitor: grayTone(16),
  stand: grayTone(10), // monitor foot
  accent: brandTone(78), // MCU chips + sensor puck: the blue focus
  probe: brandCyl(70), // roof sensor: top matches the chip tone
  dark: grayTone(26), // shaft, keyboard, gateway lid
  rod: grayCyl(28) // antenna rods
}
