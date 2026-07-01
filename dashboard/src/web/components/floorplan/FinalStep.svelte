<script>
  import {
    FLOORPLAN_CONFIG_PATH,
    FLOORPLAN_IMAGE_PATH,
    floorplanStorageJson
  } from "../../../core/floorplan/floorplan-storage";

  let {
    storageDraft = null,
    scaleSummary = null,
    placement = { originX: 0, originY: 0, rotation: 0, scale: 1 },
    imageName = "",
    roomCount = 0,
    ignoredOcclusionEdges = [],
    canSave = false,
    saveBusy = false,
    saveStatus = "",
    saveTone = "idle",
    onBack,
    onSave
  } = $props();

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  }

  function jsonSizeText() {
    return storageDraft ? formatBytes(floorplanStorageJson(storageDraft).length) : "준비 불가";
  }
</script>

<div class="floorplan-radar-placement-card">
  <strong>최종 확인</strong>
  <span>아직 ESP32에 저장하지 않았습니다. 지금까지 만든 설정을 한 번 확인합니다.</span>
  <dl>
    <div><dt>이미지</dt><dd>{imageName || FLOORPLAN_IMAGE_PATH}</dd></div>
    <div><dt>설정</dt><dd>{FLOORPLAN_CONFIG_PATH}</dd></div>
    <div><dt>방 후보</dt><dd>{roomCount}개</dd></div>
    <div><dt>전체 크기</dt><dd>{scaleSummary ? `${scaleSummary.widthMm} x ${scaleSummary.heightMm}mm` : "입력 필요"}</dd></div>
    <div><dt>레이더 위치</dt><dd>{Math.round(placement.originX)}, {Math.round(placement.originY)}px</dd></div>
    <div><dt>회전</dt><dd>{Math.round(placement.rotation)}°</dd></div>
    <div><dt>차폐 예외</dt><dd>{ignoredOcclusionEdges.length}개</dd></div>
    <div><dt>JSON 크기</dt><dd>{jsonSizeText()}</dd></div>
  </dl>
  <ul class="floorplan-final-summary-list">
    <li>업로드 이미지를 ESP32 저장용 WebP 후보로 준비했습니다.</li>
    <li>방 후보 정리와 방 이름 OCR 결과를 설정 후보에 반영했습니다.</li>
    <li>전체 평면도 크기 기준으로 레이더 스케일을 계산했습니다.</li>
    <li>레이더 위치, 회전, 차폐 예외 설정을 저장 후보에 포함합니다.</li>
  </ul>
  {#if !storageDraft}
    <em>저장 후보 JSON을 아직 만들 수 없습니다. 이미지와 전체 크기 입력 상태를 확인하세요.</em>
  {/if}
</div>

<div class="floorplan-step-actions">
  <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={onBack}>
    <span>이전 단계</span>
    <strong>레이더 배치</strong>
  </button>
  <button
    type="button"
    class="floorplan-next-button floorplan-fixed-text"
    onclick={onSave}
    disabled={saveBusy || !canSave}
  >
    <span>{saveBusy ? "저장 중" : "저장"}</span>
    <strong>저장하기</strong>
  </button>
</div>

{#if saveStatus}
  <div class="floorplan-save-status" data-tone={saveTone}>{saveStatus}</div>
{/if}
