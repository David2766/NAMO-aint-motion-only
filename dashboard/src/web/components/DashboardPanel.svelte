<script lang="ts">
  import { onDestroy } from "svelte";
  import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
  import {
    loadFloorplanStorageDocument,
    loadFloorplanStorageImage,
    loadFloorplanStorageStatus
  } from "../floorplan/floorplan-storage-client";
  import FloorplanRadarPlacementOverlay from "./FloorplanRadarPlacementOverlay.svelte";
  import RadarScene from "./RadarScene.svelte";
  import type { WebControlStatus, WebDeviceConfig, WebDeviceState, WebSystemStatus } from "../types";

  type TabTarget = "zones" | "floorplan" | "stats" | "backup";
  type IntegrationMode = "unknown" | "edge" | "ha";

  type Props = {
    config: WebDeviceConfig | null;
    state: WebDeviceState | null;
    systemStatus: WebSystemStatus | null;
    systemStatusLoading: boolean;
    systemStatusError: string;
    controlStatus: WebControlStatus | null;
    controlStatusLoading: boolean;
    controlStatusError: string;
    controlActionBusy: boolean;
    integrationMode: IntegrationMode;
    integrationModeActionBusy: boolean;
    updatedText: string;
    floorplanStorageBaseUrl: string;
    floorplanStorageFetcher?: typeof fetch;
    onNavigate: (tab: TabTarget) => void;
    onSetStatusLed: (enabled: boolean) => void | Promise<void>;
    onSetLedBlinkDuration: (seconds: number) => void | Promise<void>;
    onSetEnvironmentCorrection: (enabled: boolean) => void | Promise<void>;
    onSetTemperatureOffset: (value: number) => void | Promise<void>;
    onSetHumidityOffset: (value: number) => void | Promise<void>;
    onChangeIntegrationMode: () => void | Promise<void>;
  };

  let {
    config,
    state: deviceState,
    systemStatus,
    systemStatusLoading,
    systemStatusError,
    controlStatus,
    controlStatusLoading,
    controlStatusError,
    controlActionBusy,
    integrationMode,
    integrationModeActionBusy,
    updatedText,
    floorplanStorageBaseUrl,
    floorplanStorageFetcher,
    onNavigate,
    onSetStatusLed,
    onSetLedBlinkDuration,
    onSetEnvironmentCorrection,
    onSetTemperatureOffset,
    onSetHumidityOffset,
    onChangeIntegrationMode
  }: Props = $props();

  let tuningOpen = $state(false);
  let ledDialogOpen = $state(false);
  let environmentDialogOpen = $state(false);
  let floorplanLoading = $state(false);
  let floorplanError = $state("");
  let floorplanDocument = $state<FloorplanStorageDocument | null>(null);
  let floorplanImageUrl = $state("");

  const targetCount = $derived(deviceState?.targetCount ?? deviceState?.targets.filter((target) => target.active).length ?? 0);
  const movingTargetCount = $derived(deviceState?.movingTargetCount ?? 0);
  const stillTargetCount = $derived(deviceState?.stillTargetCount ?? 0);
  const hasFloorplan = $derived(Boolean(config?.floorplan?.enabled && config.floorplan.hasImage));
  const zoneCount = $derived(config?.zones.length ?? 0);
  const filterZoneCount = $derived(config?.zones.filter((zone) => zone.type === "filter").length ?? 0);
  const reducedZoneCount = $derived(config?.zones.filter((zone) => zone.type === "reduced").length ?? 0);
  const disabledZoneCount = $derived(config?.zones.filter((zone) => zone.type === "disabled").length ?? 0);
  const integrationModeButtonText = $derived(
    integrationMode === "edge" ? "HA 모드로" : integrationMode === "ha" ? "Edge 모드로" : "사용 환경 선택"
  );

  function yesNo(value: boolean | undefined): string {
    return value ? "감지됨" : "감지 없음";
  }

  function formatNumber(value: number | null | undefined, suffix = ""): string {
    return typeof value === "number" && Number.isFinite(value) ? `${value}${suffix}` : "-";
  }

  function formatTemperature(value: number | null | undefined): string {
    return formatNumber(value, "°C");
  }

  function formatPercent(value: number | null | undefined): string {
    return formatNumber(value, "%");
  }

  function formatLux(value: number | null | undefined): string {
    return formatNumber(value, " lux");
  }

  function formatBytes(bytes: number | undefined): string {
    if (!Number.isFinite(bytes)) return "-";
    const value = Number(bytes);
    if (value >= 1024 * 1024) return `${(value / 1024 / 1024).toFixed(1)}MB`;
    if (value >= 1024) return `${Math.round(value / 1024)}KB`;
    return `${value}B`;
  }

  function formatStoragePercent(used: number | null | undefined, total: number | null | undefined): string {
    if (!Number.isFinite(used ?? NaN) || !Number.isFinite(total ?? NaN) || Number(total) <= 0) return "-";
    return `${Math.round((Number(used) / Number(total)) * 100)}%`;
  }

  function addBytes(...values: Array<number | null | undefined>): number | undefined {
    if (values.some((value) => !Number.isFinite(value ?? NaN))) return undefined;
    return values.reduce((sum, value) => sum + Number(value), 0);
  }

  function formatUptime(seconds: number | undefined): string {
    if (!Number.isFinite(seconds)) return "-";
    const total = Math.max(0, Math.floor(Number(seconds)));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    if (hours > 0) return `${hours}시간 ${minutes}분`;
    return `${minutes}분`;
  }

  function wifiSignalInfo(connected: boolean | undefined, rssi: number | null | undefined): { bars: number; label: string } {
    if (!connected || !Number.isFinite(rssi ?? NaN)) return { bars: 0, label: systemStatusLoading ? "확인 중" : "연결 안 됨" };
    const value = Number(rssi);
    if (value >= -50) return { bars: 4, label: "매우 좋음" };
    if (value >= -60) return { bars: 3, label: "좋음" };
    if (value >= -70) return { bars: 2, label: "보통" };
    if (value >= -80) return { bars: 1, label: "약함" };
    return { bars: 1, label: "매우 약함" };
  }

  function formatDuration(seconds: number | null | undefined): string {
    if (typeof seconds !== "number" || !Number.isFinite(seconds)) return "-";
    if (seconds < 1) return "계속";
    if (seconds >= 60) return `${Math.round(seconds / 60)}분`;
    return `${Math.round(seconds)}초`;
  }

  function formatOffset(value: number | null | undefined, suffix: string): string {
    if (typeof value !== "number" || !Number.isFinite(value)) return "-";
    return `${value > 0 ? "+" : ""}${value.toFixed(1)}${suffix}`;
  }

  function clampTemperatureOffset(value: number): number {
    return Math.max(-2, Math.min(2, Math.round(value * 10) / 10));
  }

  function clampHumidityOffset(value: number): number {
    return Math.max(-5, Math.min(5, Math.round(value * 2) / 2));
  }

  $effect(() => {
    if (!hasFloorplan || floorplanDocument || floorplanLoading) return;
    void loadDashboardFloorplan();
  });

  $effect(() => {
    if (hasFloorplan) return;
    floorplanDocument = null;
    floorplanLoading = false;
    floorplanError = "";
    if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
    floorplanImageUrl = "";
  });

  onDestroy(() => {
    if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
  });

  async function loadDashboardFloorplan(): Promise<void> {
    floorplanLoading = true;
    floorplanError = "";
    try {
      const status = await loadFloorplanStorageStatus({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher });
      if (!status.hasConfig || !status.hasImage) {
        floorplanDocument = null;
        floorplanError = "저장된 평면도 데이터가 없습니다.";
        return;
      }
      const [document, image] = await Promise.all([
        loadFloorplanStorageDocument({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher }),
        loadFloorplanStorageImage({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher })
      ]);
      if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
      floorplanDocument = document;
      floorplanImageUrl = URL.createObjectURL(image);
    } catch (error) {
      floorplanError = error instanceof Error ? error.message : String(error);
      floorplanDocument = null;
      if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
      floorplanImageUrl = "";
    } finally {
      floorplanLoading = false;
    }
  }

  function floorplanPreviewStyle(): string {
    const width = floorplanDocument?.image?.width ?? 1;
    const height = floorplanDocument?.image?.height ?? 1;
    return `aspect-ratio:${width}/${height};`;
  }

  function roomPoints(points: Array<[number, number]>): string {
    return points.map(([x, y]) => `${x},${y}`).join(" ");
  }

  function roomCenter(points: Array<[number, number]>): [number, number] {
    if (!points.length) return [0, 0];
    const sum = points.reduce(
      (acc, [x, y]) => {
        acc[0] += x;
        acc[1] += y;
        return acc;
      },
      [0, 0]
    );
    return [sum[0] / points.length, sum[1] / points.length];
  }

  function floorplanScaleEstimate() {
    if (!floorplanDocument) return null;
    const [x, y, width, height] = floorplanDocument.scale.outerBoundsPx;
    return {
      outerBounds: { x, y, width, height },
      widthMm: floorplanDocument.scale.widthMm,
      heightMm: floorplanDocument.scale.heightMm,
      mmPerPxX: floorplanDocument.scale.mmPerPxX,
      mmPerPxY: floorplanDocument.scale.mmPerPxY
    };
  }

  function floorplanRadarPlacement() {
    if (!floorplanDocument) return null;
    return {
      originX: floorplanDocument.radar.originPx[0],
      originY: floorplanDocument.radar.originPx[1],
      rotation: floorplanDocument.radar.rotationDeg
    };
  }

  function wallSegmentGeometryKey(x1: number, y1: number, x2: number, y2: number): string {
    const first = [Math.round(x1), Math.round(y1)];
    const second = [Math.round(x2), Math.round(y2)];
    const [start, end] =
      first[0] < second[0] || (first[0] === second[0] && first[1] <= second[1]) ? [first, second] : [second, first];
    return `wall:${start[0]},${start[1]}-${end[0]},${end[1]}`;
  }

  function roomBoundarySegments() {
    return (
      floorplanDocument?.rooms.flatMap((room) =>
        room.pointsPx.map(([x1, y1], index) => {
          const [x2, y2] = room.pointsPx[(index + 1) % room.pointsPx.length];
          const dx = Math.abs(x2 - x1);
          const dy = Math.abs(y2 - y1);
          return {
            id: `dashboard-room-${room.id}-${index}`,
            occlusionKey: wallSegmentGeometryKey(x1, y1, x2, y2),
            x1,
            y1,
            x2,
            y2,
            axis: dy <= 1 ? "horizontal" : dx <= 1 ? "vertical" : "diagonal"
          };
        })
      ) ?? []
    );
  }

  function outerBoundsOcclusionSegments() {
    const scale = floorplanScaleEstimate();
    if (!scale) return [];
    const { x, y, width, height } = scale.outerBounds;
    const x2 = x + width;
    const y2 = y + height;
    return [
      { id: "dashboard-outer-top", occlusionKey: "outer-stored-top", locked: true, x1: x, y1: y, x2, y2: y, axis: "horizontal" },
      { id: "dashboard-outer-right", occlusionKey: "outer-stored-right", locked: true, x1: x2, y1: y, x2, y2, axis: "vertical" },
      { id: "dashboard-outer-bottom", occlusionKey: "outer-stored-bottom", locked: true, x1: x2, y1: y2, x2: x, y2, axis: "horizontal" },
      { id: "dashboard-outer-left", occlusionKey: "outer-stored-left", locked: true, x1: x, y1: y2, x2: x, y2: y, axis: "vertical" }
    ];
  }

  function radarOcclusionSegments() {
    return [...roomBoundarySegments(), ...outerBoundsOcclusionSegments()];
  }

  async function chooseStatusLed(enabled: boolean): Promise<void> {
    await onSetStatusLed(enabled);
    ledDialogOpen = false;
  }

  async function chooseLedDuration(seconds: number): Promise<void> {
    await onSetLedBlinkDuration(seconds);
    ledDialogOpen = false;
  }

  async function chooseEnvironmentCorrection(enabled: boolean): Promise<void> {
    await onSetEnvironmentCorrection(enabled);
  }

  async function nudgeTemperatureOffset(delta: number): Promise<void> {
    await onSetTemperatureOffset(clampTemperatureOffset((controlStatus?.temperatureOffset ?? 0) + delta));
  }

  async function nudgeHumidityOffset(delta: number): Promise<void> {
    await onSetHumidityOffset(clampHumidityOffset((controlStatus?.humidityOffset ?? 0) + delta));
  }

  function closeLedDialogFromBackdrop(event: MouseEvent): void {
    if (event.target === event.currentTarget) ledDialogOpen = false;
  }

  function closeEnvironmentDialogFromBackdrop(event: MouseEvent): void {
    if (event.target === event.currentTarget) environmentDialogOpen = false;
  }

  const wifiInfo = $derived(wifiSignalInfo(systemStatus?.wifi?.connected, systemStatus?.wifi?.rssi));
  const dataStorageUsedBytes = $derived(systemStatus?.flash?.storageUsedBytes ?? systemStatus?.storage?.usedBytes);
  const flashOccupiedBytes = $derived(
    addBytes(systemStatus?.flash?.firmwareUsedBytes, systemStatus?.flash?.otaSlotBytes, dataStorageUsedBytes)
  );
  const flashOccupancyText = $derived(
    `${formatBytes(flashOccupiedBytes)} / ${formatBytes(systemStatus?.flash?.totalBytes)}`
  );
  const flashOccupancyPercentText = $derived(formatStoragePercent(flashOccupiedBytes, systemStatus?.flash?.totalBytes));
</script>

<section class="dashboard-workspace">
  <div class="floorplan-workflow-card dashboard-intro-card">
    <div>
      <strong>대시보드</strong>
      <span>센서 상태와 실시간 감지 정보를 한 화면에서 확인합니다.</span>
    </div>
    <dl class="floorplan-stored-summary dashboard-intro-summary">
      <div>
        <dt>연결</dt>
          <dd>{deviceState?.connected ? "연결됨" : "대기 중"}</dd>
      </div>
      <div>
        <dt>업데이트</dt>
        <dd>{updatedText}</dd>
      </div>
    </dl>
  </div>

  <section class="dashboard-status-grid" aria-label="핵심 상태">
    <div class="floorplan-workflow-card dashboard-status-card" data-tone={deviceState?.presence ? "ok" : "idle"}>
      <div>
        <span>재실</span>
        <strong>{yesNo(deviceState?.presence)}</strong>
      </div>
    </div>

    <div class="floorplan-workflow-card dashboard-status-card" data-tone={deviceState?.motion ? "ok" : "idle"}>
      <div>
        <span>움직임</span>
        <strong>{yesNo(deviceState?.motion)}</strong>
      </div>
    </div>

    <div class="floorplan-workflow-card dashboard-status-card">
      <div>
        <span>대상 수</span>
        <strong>{targetCount}개</strong>
      </div>
      <small>이동 {movingTargetCount} · 정지 {stillTargetCount}</small>
    </div>

    <div class="floorplan-workflow-card dashboard-status-card">
      <div>
        <span>환경</span>
        <strong>{formatTemperature(deviceState?.temperatureC)}</strong>
      </div>
      <small>{formatPercent(deviceState?.humidityPercent)} · {formatLux(deviceState?.illuminanceLux)}</small>
    </div>
  </section>

  <section class="dashboard-main-grid">
    <section class="dashboard-visual-panel">
      <div class="radar-scene-frame dashboard-visual-frame">
        <div class="dashboard-visual-status">
          <span>{hasFloorplan ? "평면도 준비됨" : "레이더맵 준비됨"}</span>
          <strong>{targetCount}개 대상</strong>
        </div>
        {#if floorplanDocument && floorplanImageUrl}
          <div class="dashboard-floorplan-preview" style={floorplanPreviewStyle()}>
            <svg
              viewBox={`0 0 ${floorplanDocument.image.width} ${floorplanDocument.image.height}`}
              role="img"
              aria-label="저장된 평면도"
            >
              <image
                href={floorplanImageUrl}
                width={floorplanDocument.image.width}
                height={floorplanDocument.image.height}
                preserveAspectRatio="xMidYMid meet"
              />
              {#each floorplanDocument.rooms as room}
                {@const center = roomCenter(room.pointsPx)}
                <polygon class="dashboard-floorplan-room" points={roomPoints(room.pointsPx)} />
                <text class="dashboard-floorplan-room-label" x={center[0]} y={center[1]}>{room.name}</text>
              {/each}
            </svg>
            <FloorplanRadarPlacementOverlay
              imageWidth={floorplanDocument.image.width}
              imageHeight={floorplanDocument.image.height}
              scaleEstimate={floorplanScaleEstimate()}
              placement={floorplanRadarPlacement()}
              scalePercent={(floorplanDocument.radar.scale ?? 1) * 100}
              zones={config?.zones ?? []}
              calibrationZones={config?.calibrationZones ?? []}
              targets={deviceState?.targets ?? []}
              wallSegments={roomBoundarySegments()}
              occlusionSegments={radarOcclusionSegments()}
              ignoredOcclusionSegmentIds={floorplanDocument.occlusion?.ignoredEdges ?? []}
              readOnly
              showPlacementLabel={false}
            />
          </div>
        {:else if hasFloorplan || floorplanLoading || floorplanError}
          <div class="dashboard-visual-placeholder">
            <strong>
              {floorplanLoading
                ? "평면도 불러오는 중"
                : hasFloorplan
                  ? "평면도 보기 영역"
                  : "레이더맵 보기 영역"}
            </strong>
            <span>
              {floorplanError
                ? `평면도를 불러오지 못했습니다. ${floorplanError}`
                : hasFloorplan
                  ? "저장된 평면도 이미지를 불러오고 있습니다."
                  : "저장된 평면도가 없으면 레이더맵이 이 영역에 표시됩니다."}
            </span>
          </div>
        {:else}
          <div class="dashboard-radar-preview">
            <RadarScene state={deviceState} {config} />
          </div>
        {/if}
      </div>
    </section>

    <aside class="dashboard-side-stack">
      <div class="floorplan-workflow-card dashboard-metric-card">
        <strong>빠른 이동</strong>
        <div class="floorplan-stored-tools dashboard-nav-tools">
          <button type="button" onclick={() => onNavigate("zones")}>구역 설정</button>
          <button type="button" onclick={() => onNavigate("floorplan")}>평면도</button>
          <button type="button" onclick={() => onNavigate("stats")}>감지 통계</button>
          <button type="button" onclick={() => onNavigate("backup")}>관리 / 백업</button>
        </div>
      </div>

      <div class="floorplan-workflow-card dashboard-metric-card">
        <strong>구역 상태</strong>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>구역</dt>
            <dd>{zoneCount}개</dd>
          </div>
          <div>
            <dt>필터</dt>
            <dd>{filterZoneCount}개</dd>
          </div>
          <div>
            <dt>둔감</dt>
            <dd>{reducedZoneCount}개</dd>
          </div>
          <div>
            <dt>제외</dt>
            <dd>{disabledZoneCount}개</dd>
          </div>
        </dl>
      </div>

      <div class="floorplan-workflow-card dashboard-metric-card">
        <strong>기기 상태</strong>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>펌웨어</dt>
            <dd>{systemStatus?.firmware?.version ?? (systemStatusLoading ? "확인 중" : "-")}</dd>
          </div>
          <div>
            <dt>대시보드</dt>
            <dd>{systemStatus?.dashboard?.version ?? "-"}</dd>
          </div>
          <div>
            <dt>Wi-Fi</dt>
            <dd>
              <span class="wifi-strength" aria-label={`Wi-Fi ${wifiInfo.label}`}>
                {#each [0, 1, 2, 3] as index}
                  <span class:active={index < wifiInfo.bars}></span>
                {/each}
              </span>
              {wifiInfo.label}{systemStatus?.wifi?.connected ? ` · ${systemStatus.wifi?.rssi ?? "-"}dBm` : ""}
            </dd>
          </div>
          <div>
            <dt>저장공간</dt>
            <dd>{flashOccupancyText} · {flashOccupancyPercentText} 사용 중</dd>
          </div>
          <div>
            <dt>가동 시간</dt>
            <dd>{formatUptime(systemStatus?.firmware?.uptimeSeconds)}</dd>
          </div>
        </dl>
        {#if systemStatusError}
          <p class="dashboard-note">시스템 정보를 읽지 못했습니다. {systemStatusError}</p>
        {/if}
      </div>

      <div class="floorplan-workflow-card dashboard-metric-card dashboard-tuning-control-card">
        <strong>고급 설정</strong>
        <button
          class="dashboard-tuning-toggle"
          type="button"
          aria-expanded={tuningOpen}
          onclick={() => (tuningOpen = !tuningOpen)}
        >
          {tuningOpen ? "접기" : "펼치기"}
        </button>
      </div>

      {#if tuningOpen}
        <div class="floorplan-workflow-card dashboard-tuning-card">
          <strong>고급 튜닝</strong>
          <span>자주 쓰지 않는 기기 제어를 모아둡니다.</span>
          <dl class="floorplan-stored-summary">
            <div>
              <dt>LED 동작</dt>
              <dd>
                {controlStatusLoading
                  ? "확인 중"
                  : controlStatus?.statusLedEnabled
                    ? "켜짐"
                    : controlStatus?.statusLedKnown
                      ? "꺼짐"
                      : "-"}
              </dd>
            </div>
            <div>
              <dt>자동 꺼짐</dt>
              <dd>{formatDuration(controlStatus?.ledBlinkDuration)}</dd>
            </div>
            <div>
              <dt>온습도 보정</dt>
              <dd>
                {controlStatusLoading
                  ? "확인 중"
                  : controlStatus?.environmentCorrectionEnabled
                    ? "켜짐"
                    : controlStatus?.environmentCorrectionKnown
                      ? "꺼짐"
                      : "-"}
              </dd>
            </div>
          </dl>
          {#if controlStatusError}
            <p class="dashboard-note">제어 상태를 읽지 못했습니다. {controlStatusError}</p>
          {/if}
          <div class="dashboard-tuning-actions">
            <button type="button" disabled={controlActionBusy} onclick={() => (environmentDialogOpen = true)}>온습도 보정</button>
            <button type="button" disabled={controlActionBusy} onclick={() => (ledDialogOpen = true)}>Status LED 제어</button>
            <button type="button" disabled={integrationModeActionBusy} onclick={onChangeIntegrationMode}>
              {integrationModeActionBusy ? "변경 중..." : integrationModeButtonText}
            </button>
          </div>
        </div>
      {/if}
    </aside>
  </section>
</section>

{#if ledDialogOpen}
  <div class="dashboard-dialog-backdrop" role="presentation" onclick={closeLedDialogFromBackdrop}>
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="dashboard-led-dialog-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="dashboard-led-dialog-title">Status LED 제어</strong>
        <span>LED 동작과 재실 감지 시 자동 꺼짐 시간을 선택합니다.</span>
      </div>
      <dl class="floorplan-stored-summary">
        <div>
          <dt>LED 동작</dt>
          <dd>{controlStatus?.statusLedEnabled ? "켜짐" : controlStatus?.statusLedKnown ? "꺼짐" : "-"}</dd>
        </div>
        <div>
          <dt>자동 꺼짐</dt>
          <dd>{formatDuration(controlStatus?.ledBlinkDuration)}</dd>
        </div>
      </dl>
      <div class="dashboard-dialog-section">
        <strong>LED 동작</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => chooseStatusLed(true)}>켜기</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseStatusLed(false)}>끄기</button>
        </div>
      </div>
      <div class="dashboard-dialog-section">
        <strong>자동 꺼짐 시간</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(0)}>계속</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(30)}>30초</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(60)}>60초</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(300)}>5분</button>
        </div>
      </div>
      <div class="floorplan-delete-dialog-actions">
        <button
          type="button"
          class="danger-button dashboard-dialog-close-button"
          disabled={controlActionBusy}
          onclick={() => (ledDialogOpen = false)}
        >
          닫기
        </button>
      </div>
    </div>
  </div>
{/if}

{#if environmentDialogOpen}
  <div class="dashboard-dialog-backdrop" role="presentation" onclick={closeEnvironmentDialogFromBackdrop}>
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="dashboard-environment-dialog-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="dashboard-environment-dialog-title">온습도 보정</strong>
        <span>기기 내부 보정 사용 여부와 사용자 오프셋을 조정합니다.</span>
      </div>
      <dl class="floorplan-stored-summary">
        <div>
          <dt>보정 동작</dt>
          <dd>{controlStatus?.environmentCorrectionEnabled ? "켜짐" : controlStatus?.environmentCorrectionKnown ? "꺼짐" : "-"}</dd>
        </div>
        <div>
          <dt>온도 오프셋</dt>
          <dd>{formatOffset(controlStatus?.temperatureOffset, "°C")}</dd>
        </div>
        <div>
          <dt>습도 오프셋</dt>
          <dd>{formatOffset(controlStatus?.humidityOffset, "%")}</dd>
        </div>
      </dl>
      <div class="dashboard-dialog-section">
        <strong>보정 동작</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => chooseEnvironmentCorrection(true)}>켜기</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseEnvironmentCorrection(false)}>끄기</button>
        </div>
      </div>
      <div class="dashboard-dialog-section">
        <strong>온도 오프셋</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeTemperatureOffset(-0.1)}>-0.1°C</button>
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeTemperatureOffset(0.1)}>+0.1°C</button>
        </div>
      </div>
      <div class="dashboard-dialog-section">
        <strong>습도 오프셋</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeHumidityOffset(-0.5)}>-0.5%</button>
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeHumidityOffset(0.5)}>+0.5%</button>
        </div>
      </div>
      <div class="floorplan-delete-dialog-actions">
        <button
          type="button"
          class="danger-button dashboard-dialog-close-button"
          disabled={controlActionBusy}
          onclick={() => (environmentDialogOpen = false)}
        >
          닫기
        </button>
      </div>
    </div>
  </div>
{/if}
