// Parse-check every mermaid block in docs/**/*.md. Run from docs/: node parse_diagrams.mjs
import { readdirSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { JSDOM } from 'jsdom'

// mermaid (via DOMPurify) needs DOM globals before it is imported
const dom = new JSDOM('<!DOCTYPE html><body></body>')
globalThis.window = dom.window
globalThis.document = dom.window.document
Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true })

function* mdFiles(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    if (e.name === 'node_modules' || e.name === '.vitepress' || e.name === 'public') continue
    const p = join(dir, e.name)
    if (e.isDirectory()) yield* mdFiles(p)
    else if (e.name.endsWith('.md')) yield p
  }
}

const { default: mermaid } = await import('mermaid')
mermaid.initialize({ startOnLoad: false })

let blocks = 0
let bad = 0
for (const f of mdFiles('.')) {
  const src = readFileSync(f, 'utf8')
  const re = /```mermaid\n([\s\S]*?)```/g
  let m
  while ((m = re.exec(src)) !== null) {
    blocks++
    try {
      await mermaid.parse(m[1])
      console.log('OK  ', f, `block ${blocks}`)
    } catch (e) {
      bad++
      console.log('FAIL', f, String(e).split('\n')[0])
    }
  }
}
console.log(`\n${blocks} blocks, ${bad} failing`)
process.exit(bad ? 1 : 0)
