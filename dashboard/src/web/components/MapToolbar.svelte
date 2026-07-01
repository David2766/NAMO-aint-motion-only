<script lang="ts">
  import EditorToolbar from "./EditorToolbar.svelte";
  import type { SaveState, WebZone } from "../types";

  type Props = {
    canUndo: boolean;
    canRedo: boolean;
    selectedZone: WebZone | null;
    hasSelectedCalibrationZone: boolean;
    selectedLabel: string;
    saveState: SaveState;
    saveStatusText: string;
    updatedText: string;
    debugMode: boolean;
    onUndo: () => void;
    onRedo: () => void;
    onConvertToRect: () => void;
    onDeleteSelected: () => void;
    onToggleDebug: () => void;
  };

  let {
    canUndo,
    canRedo,
    selectedZone,
    hasSelectedCalibrationZone,
    selectedLabel,
    saveState,
    saveStatusText,
    updatedText,
    debugMode,
    onUndo,
    onRedo,
    onConvertToRect,
    onDeleteSelected,
    onToggleDebug
  }: Props = $props();
</script>

<EditorToolbar ariaLabel="구역 편집 도구" label={selectedLabel}>
  <button type="button" disabled={!canUndo} title="되돌리기" onclick={onUndo}>↶</button>
  <button type="button" disabled={!canRedo} title="다시 실행" onclick={onRedo}>↷</button>
  <button type="button" disabled={!selectedZone || selectedZone.shape === "rect"} title="사각형으로 정리" onclick={onConvertToRect}>
    사각형
  </button>
  <button type="button" disabled={!selectedZone && !hasSelectedCalibrationZone} title="선택 항목 삭제" onclick={onDeleteSelected}>삭제</button>
  <button type="button" data-active={debugMode ? "true" : "false"} aria-pressed={debugMode} onclick={onToggleDebug}>Debug</button>
</EditorToolbar>

<div class="map-toolbar-status" data-map-toolbar>
  <span class="save-status" data-save-state={saveState}>{saveStatusText}</span>
  <span>마지막 업데이트</span>
  <strong data-updated-at>{updatedText}</strong>
</div>
