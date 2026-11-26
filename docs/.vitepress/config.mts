import { defineConfig } from 'vitepress'
import { withMermaid } from 'vitepress-plugin-mermaid'

export default withMermaid(defineConfig({
  lang: 'en-US',
  title: 'TinyRL-Industrial',
  description:
    'ESP32-S3 edge nodes that learn what normal means on-device, publish conditions instead of telemetry, and ask a question only when an alarm matches nothing they know.',
  cleanUrls: true,
  ignoreDeadLinks: false,
  vite: {
    optimizeDeps: {
      include: ['mermaid']
    }
  },
  themeConfig: {
    nav: [
      { text: 'Guide', link: '/guide/', activeMatch: '/guide/' },
      { text: 'System', link: '/system/architecture', activeMatch: '/system/' },
      { text: 'Research', link: '/research/questions', activeMatch: '/research/' },
      { text: 'Reference', link: '/reference/tests', activeMatch: '/reference/' }
    ],
    sidebar: {
      '/guide/': [
        {
          text: 'Guide',
          items: [
            { text: 'What is TinyRL-Industrial', link: '/guide/' },
            { text: 'Quick start', link: '/guide/quickstart' },
            { text: 'First-run lifecycle', link: '/guide/lifecycle' },
            { text: 'Serial console and replay', link: '/guide/console' },
            { text: 'Adding a sensor', link: '/guide/adding-a-sensor' },
            { text: 'Operator dashboard', link: '/guide/dashboard' },
            { text: 'The AI agent', link: '/guide/agent' }
          ]
        }
      ],
      '/system/': [
        {
          text: 'System',
          items: [
            { text: 'Node architecture', link: '/system/architecture' },
            { text: 'The TinyRL stack on ESP32-S3', link: '/system/tinyrl-stack' },
            { text: 'MQTT contract (uns-v1.1)', link: '/system/contract' },
            { text: 'Algorithms', link: '/system/algorithms' },
            { text: 'Enhancements over the base core', link: '/system/enhancements' }
          ]
        }
      ],
      '/research/': [
        {
          text: 'Research',
          items: [
            { text: 'Research questions', link: '/research/questions' },
            { text: 'Design decisions', link: '/research/decisions' },
            { text: 'Results and figures', link: '/research/results' },
            { text: 'Literature anchors', link: '/research/literature' }
          ]
        }
      ],
      '/reference/': [
        {
          text: 'Reference',
          items: [
            { text: 'Tests and validation', link: '/reference/tests' },
            { text: 'Repository layout', link: '/reference/layout' },
            { text: 'References', link: '/reference/references' }
          ]
        }
      ]
    },
    socialLinks: [
      { icon: 'github', link: 'https://github.com/leekaize/TinyRL-Industrial' }
    ],
    outline: { level: [2, 3], label: 'On this page' },
    editLink: {
      pattern: 'https://github.com/leekaize/TinyRL-Industrial/edit/main/docs/:path',
      text: 'Edit this page on GitHub'
    },
    docFooter: { prev: 'Previous', next: 'Next' }
  },
  lastUpdated: true
}))
