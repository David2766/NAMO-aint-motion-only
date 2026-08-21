<script lang="ts">
  import type { Messages } from "../i18n";
  import DashboardPanel from "../components/DashboardPanel.svelte";
  import type { PlatformSiteConfigurationStatus } from "../api/platform-pairing";
  import type { WebControlStatus, WebDeviceConfig, WebDeviceState, WebSystemStatus } from "../types";

  type TabTarget = "zones" | "floorplan" | "stats" | "backup";
  type IntegrationMode = "unknown" | "edge" | "ha";

  type Props = {
    config: WebDeviceConfig | null;
    state: WebDeviceState | null;
    systemStatus: WebSystemStatus | null;
    systemStatusLoading: boolean;
    systemStatusError: string;
    platformSiteConfiguration: PlatformSiteConfigurationStatus | null;
    controlStatus: WebControlStatus | null;
    controlStatusLoading: boolean;
    controlStatusError: string;
    controlActionBusy: boolean;
    integrationMode: IntegrationMode;
    integrationModeActionBusy: boolean;
    messages: Messages;
    updatedText: string;
    floorplanStorageBaseUrl: string;
    floorplanStorageFetcher?: typeof fetch;
    onNavigate: (tab: TabTarget) => void;
    onSetStatusLed: (enabled: boolean) => void | Promise<void>;
    onSetLedBlinkDuration: (seconds: number) => void | Promise<void>;
    onSetEnvironmentCorrection: (enabled: boolean) => void | Promise<void>;
    onSetLegacyPresenceFallback: (enabled: boolean) => void | Promise<void>;
    onSetTemperatureOffset: (value: number) => void | Promise<void>;
    onSetHumidityOffset: (value: number) => void | Promise<void>;
    onSetTimezone: (timezone: string) => boolean | Promise<boolean>;
    onChangeIntegrationMode: () => void | Promise<void>;
  };

  let {
    config,
    state,
    systemStatus,
    systemStatusLoading,
    systemStatusError,
    platformSiteConfiguration,
    controlStatus,
    controlStatusLoading,
    controlStatusError,
    controlActionBusy,
    integrationMode,
    integrationModeActionBusy,
    messages,
    updatedText,
    floorplanStorageBaseUrl,
    floorplanStorageFetcher,
    onNavigate,
    onSetStatusLed,
    onSetLedBlinkDuration,
    onSetEnvironmentCorrection,
    onSetLegacyPresenceFallback,
    onSetTemperatureOffset,
    onSetHumidityOffset,
    onSetTimezone,
    onChangeIntegrationMode
  }: Props = $props();
</script>

<section class="tab-page">
  <DashboardPanel
    {config}
    {state}
    {systemStatus}
    {systemStatusLoading}
    {systemStatusError}
    {platformSiteConfiguration}
    {controlStatus}
    {controlStatusLoading}
    {controlStatusError}
    {controlActionBusy}
    {integrationMode}
    {integrationModeActionBusy}
    {messages}
    {updatedText}
    {floorplanStorageBaseUrl}
    {floorplanStorageFetcher}
    {onNavigate}
    {onSetStatusLed}
    {onSetLedBlinkDuration}
    {onSetEnvironmentCorrection}
    {onSetLegacyPresenceFallback}
    {onSetTemperatureOffset}
    {onSetHumidityOffset}
    {onSetTimezone}
    {onChangeIntegrationMode}
  />
</section>
