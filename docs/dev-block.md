---
layout: page
---

<script setup>
import HeroScene from './.vitepress/theme/components/HeroScene.vue'
</script>

<!-- Dev harness for visual iteration on HeroScene. Kept on purpose:
     screenshot this page to inspect the scene without page chrome. -->

<div class="harness">
  <HeroScene />
</div>

<style>
.harness {
  width: 480px;
  height: 480px;
}

/* zoom override for close-up inspection; set to 1 to check true rendering */
.harness .scaler {
  transform: scale(1) !important;
}
</style>
