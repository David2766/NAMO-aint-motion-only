<script>
  let {
    candidates = [],
    selectedCandidateId = "",
    onSelect,
    onRename,
    onConfirm,
    onReject,
    onRemove,
    mode = "edit",
    measurements = {},
    estimatedSizes = {},
    showEstimatedSizes = false,
    onSizeChange,
    snapEditActive = false,
    snapEditCandidateId = "",
    splitDraftActive = false,
    splitDraftCandidateId = "",
    mergeDraftActive = false,
    mergeDraftCandidateIds = [],
    selectionLocked = false,
    onStartSnapEdit,
    onFinishSnapEdit,
    onCancelSnapEdit,
    onStartSplitDraft,
    onFinishSplitDraft,
    onCancelSplitDraft,
    canFinishSplitDraft,
    onStartMergeDraft,
    onFinishMergeDraft,
    onCancelMergeDraft,
    canFinishMergeDraft,
    showCandidateTools = false
  } = $props();

  const statusLabel = {
    candidate: "후보",
    confirmed: "확정",
    rejected: "제외"
  };
  function selectCandidate(id) {
    if (selectionLocked && id !== selectedCandidateId) return;
    onSelect?.(id);
  }

  function candidateMeasurement(candidate) {
    return measurements[candidate.id] ?? { width: "", height: "" };
  }

  function candidateEstimatedSize(candidate) {
    return estimatedSizes[candidate.id] ?? null;
  }

  function sizeNotes(size) {
    if (!size) return "";
    const notes = [];
    if (size.widthFromOuter) notes.push("가로 전체 기준");
    if (size.heightFromOuter) notes.push("세로 전체 기준");
    if (size.manuallyEdited) notes.push("직접 수정됨");
    return notes.join(" · ");
  }

  function measurementValue(candidate, field) {
    const measurement = candidateMeasurement(candidate);
    const estimated = candidateEstimatedSize(candidate);
    return measurement[field] || (estimated ? String(field === "width" ? estimated.widthMm : estimated.heightMm) : "");
  }

  function mergeSelected(candidate) {
    return mergeDraftCandidateIds.includes(candidate.id);
  }
</script>

<div class="floorplan-candidate-card">
  <strong>방 목록</strong>
  {#if candidates.length}
    <ul>
      {#each candidates as candidate}
        <li class={`${candidate.status} ${selectedCandidateId === candidate.id ? "selected" : ""} ${mergeSelected(candidate) ? "merge-selected" : ""}`}>
          <button
            type="button"
            class="floorplan-candidate-select"
            disabled={selectionLocked && candidate.id !== selectedCandidateId}
            onclick={() => selectCandidate(candidate.id)}
          >
            <span>{candidate.name || "이름 없음"}</span>
            <em>{statusLabel[candidate.status]} · 신뢰도 {candidate.confidence}%</em>
            {#if mode === "ocr" && showEstimatedSizes && candidateEstimatedSize(candidate)}
              {@const size = candidateEstimatedSize(candidate)}
              <em>{size.widthPx} x {size.heightPx}px · 약 {size.widthMm} x {size.heightMm}mm</em>
              {#if sizeNotes(size)}
                <em>{sizeNotes(size)}</em>
              {/if}
            {/if}
          </button>

          {#if mode === "ocr" && selectedCandidateId === candidate.id}
            <div class="floorplan-candidate-editor floorplan-room-info-editor">
              <input
                value={candidate.name}
                maxlength="16"
                placeholder="방 이름"
                oninput={(event) => onRename?.(candidate.id, event.currentTarget.value)}
              />
              {#if showEstimatedSizes && candidateEstimatedSize(candidate)}
                {@const size = candidateEstimatedSize(candidate)}
                <div class="floorplan-estimated-size">
                  <span>추정 크기</span>
                  <strong>{size.widthMm} x {size.heightMm}mm</strong>
                  <em>{size.widthPx} x {size.heightPx}px 기준</em>
                  {#if sizeNotes(size)}
                    <em>{sizeNotes(size)}</em>
                  {/if}
                </div>
                <div class={`floorplan-size-fields ${size.manuallyEdited ? "manual" : ""}`}>
                  <label>
                    <span>가로</span>
                    <input
                      inputmode="decimal"
                      value={measurementValue(candidate, "width")}
                      placeholder="mm"
                      oninput={(event) => onSizeChange?.(candidate.id, "width", event.currentTarget.value)}
                    />
                  </label>
                  <label>
                    <span>세로</span>
                    <input
                      inputmode="decimal"
                      value={measurementValue(candidate, "height")}
                      placeholder="mm"
                      oninput={(event) => onSizeChange?.(candidate.id, "height", event.currentTarget.value)}
                    />
                  </label>
                </div>
              {/if}
            </div>
          {:else if mode !== "ocr" && selectedCandidateId === candidate.id}
            <div class="floorplan-candidate-editor">
              <input
                value={candidate.name}
                maxlength="16"
                placeholder="방 이름"
                oninput={(event) => onRename?.(candidate.id, event.currentTarget.value)}
              />
              <div>
                {#if snapEditActive && snapEditCandidateId === candidate.id}
                  <span class="floorplan-inline-tool-note">평면도에서 벽에 맞추는 중입니다.</span>
                {:else if splitDraftActive && splitDraftCandidateId === candidate.id}
                  <span class="floorplan-inline-tool-note">평면도에서 방을 나누는 중입니다.</span>
                {:else if mergeDraftActive}
                  <span class="floorplan-inline-tool-note">
                    {mergeSelected(candidate) ? "합칠 방으로 선택되었습니다." : "합칠 방을 하나 더 선택하세요."}
                  </span>
                {:else if showCandidateTools}
                  <button type="button" onclick={() => onStartSnapEdit?.(candidate.id)}>벽에 맞추기</button>
                  <button type="button" onclick={() => onStartSplitDraft?.(candidate.id)}>방 나누기</button>
                  <button type="button" onclick={() => onStartMergeDraft?.()}>방 합치기</button>
                {/if}
                <button type="button" onclick={() => onConfirm?.(candidate.id)}>확정</button>
                <button type="button" onclick={() => onReject?.(candidate.id)}>제외</button>
                <button type="button" class="danger-button" onclick={() => onRemove?.(candidate.id)}>삭제</button>
              </div>
            </div>
          {/if}
        </li>
      {/each}
    </ul>
  {:else}
    <span>아직 생성된 방 후보가 없습니다.</span>
  {/if}
</div>

