// Module shims for the plain-TS language server: .vue single-file
// components resolve through Vite at build time, not through tsc.
/// <reference types="vite/client" />

declare module '*.vue' {
  import type { DefineComponent } from 'vue'
  const component: DefineComponent<Record<string, never>, Record<string, never>, unknown>
  export default component
}
