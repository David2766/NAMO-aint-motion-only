<script lang="ts">
  import App from "./App.svelte";
  import { mockApi, resetMockDeviceStorage } from "./api/mock-api";
  import SetupMockPanel from "./components/SetupMockPanel.svelte";
  import { mockFloorplanStorageFetch, resetMockFloorplanStorage } from "./floorplan/mock-floorplan-storage";
  import { createI18n } from "./i18n/useI18n.svelte";

  const DEMO_SETUP_COMPLETE_KEY = "presence-sensor-demo-setup-complete";
  const i18n = createI18n();

  let setupMockCompleted = $state(localStorage.getItem(DEMO_SETUP_COMPLETE_KEY) === "1");

  function completeSetupMock(): void {
    localStorage.setItem(DEMO_SETUP_COMPLETE_KEY, "1");
    setupMockCompleted = true;
  }

  function resetDemoStorage(): void {
    resetMockDeviceStorage();
    resetMockFloorplanStorage();
    localStorage.removeItem(DEMO_SETUP_COMPLETE_KEY);
    window.location.reload();
  }
</script>

{#if setupMockCompleted}
  <App
    api={mockApi}
    demoMode={true}
    mockMode={true}
    floorplanStorageFetcher={mockFloorplanStorageFetch}
    onResetDemoStorage={resetDemoStorage}
    haSetupHandoffRequired={false}
  />
{:else}
  <SetupMockPanel
    messages={i18n.msg}
    language={i18n.language}
    onLanguageChange={i18n.setLanguage}
    onComplete={completeSetupMock}
  />
{/if}
