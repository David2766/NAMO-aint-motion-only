<script lang="ts">
  import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
  import FloorplanPanel from "../components/FloorplanPanel.svelte";
  import type { Messages } from "../i18n/types";
  import type { PlatformSiteConfigurationStatus } from "../api/platform-pairing";
  import type { WebDeviceConfig, WebDeviceState } from "../types";

  type Props = {
    messages: Messages;
    deviceConfig: WebDeviceConfig;
    deviceState: WebDeviceState | null;
    localNodeId?: string;
    siteConfiguration: PlatformSiteConfigurationStatus | null;
    floorplanStorageBaseUrl: string;
    floorplanStorageFetcher?: typeof fetch;
    onUpdateDeviceConfig: (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;
    onSelectDeviceZone: (zoneId: string) => void;
    onSaveDeviceConfig: () => void | Promise<void>;
    onSaveFloorplan: (document: FloorplanStorageDocument, image: Blob) => void | Promise<void>;
    onBeforeSharedFloorplanSave: () => void | Promise<void>;
    onPublishSharedFloorplan: (document: FloorplanStorageDocument) => void | Promise<void>;
    onFloorplanDeleted: () => void | Promise<void>;
    onFloorplanOwnerRequired: () => void;
    onFloorplanDeleteBlocked: () => void;
    onUnsavedChange?: (dirty: boolean) => void;
    canUseDetectionType: boolean;
    onDetectionOwnerRequired: () => void;
  };

  let {
    messages,
    deviceConfig,
    deviceState,
    localNodeId,
    siteConfiguration,
    floorplanStorageBaseUrl,
    floorplanStorageFetcher,
    onUpdateDeviceConfig,
    onSelectDeviceZone,
    onSaveDeviceConfig,
    onSaveFloorplan,
    onBeforeSharedFloorplanSave,
    onPublishSharedFloorplan,
    onFloorplanDeleted,
    onFloorplanOwnerRequired,
    onFloorplanDeleteBlocked,
    onUnsavedChange,
    canUseDetectionType,
    onDetectionOwnerRequired
  }: Props = $props();
</script>

<section class="tab-page">
    <FloorplanPanel
    {messages}
    {deviceConfig}
    {deviceState}
    {localNodeId}
    {siteConfiguration}
    {floorplanStorageBaseUrl}
    {floorplanStorageFetcher}
    {onUpdateDeviceConfig}
    {onSelectDeviceZone}
    {onSaveDeviceConfig}
    {onSaveFloorplan}
    {onBeforeSharedFloorplanSave}
    {onPublishSharedFloorplan}
    {onFloorplanDeleted}
    {onFloorplanOwnerRequired}
    {onFloorplanDeleteBlocked}
    {onUnsavedChange}
    {canUseDetectionType}
    {onDetectionOwnerRequired}
  />
</section>
