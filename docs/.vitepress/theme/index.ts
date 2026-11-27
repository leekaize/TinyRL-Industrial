import { h } from 'vue'
import DefaultTheme from 'vitepress/theme'
import HeroScene from './components/HeroScene.vue'

export default {
  extends: DefaultTheme,
  Layout() {
    return h(DefaultTheme.Layout, null, {
      'home-hero-image': () => h(HeroScene)
    })
  }
}
