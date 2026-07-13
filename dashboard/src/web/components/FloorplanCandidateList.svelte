<script>
  let {
    messages,
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
    onStartSplitDraft,
    onStartMergeDraft,
    showCandidateTools = false
  } = $props();

  const text = $derived(messages.floorplan.candidateList);

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
    if (size.widthFromOuter) notes.push(text.notes.widthFromOuter);
    if (size.heightFromOuter) notes.push(text.notes.heightFromOuter);
    if (size.manuallyEdited) notes.push(text.notes.manuallyEdited);
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
  <strong>{text.title}</strong>
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
            <span>{candidate.name || text.noName}</span>
            <em>{text.status[candidate.status]} · {text.confidence(candidate.confidence)}</em>
            {#if mode === "ocr" && showEstimatedSizes && candidateEstimatedSize(candidate)}
              {@const size = candidateEstimatedSize(candidate)}
              <em>{text.approximateSize(size.widthPx, size.heightPx, size.widthMm, size.heightMm)}</em>
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
                placeholder={text.roomNamePlaceholder}
                oninput={(event) => onRename?.(candidate.id, event.currentTarget.value)}
              />
              {#if showEstimatedSizes && candidateEstimatedSize(candidate)}
                {@const size = candidateEstimatedSize(candidate)}
                <div class="floorplan-estimated-size">
                  <span>{text.estimatedSize}</span>
                  <strong>{size.widthMm} x {size.heightMm}mm</strong>
                  <em>{text.estimatedPixelBasis(size.widthPx, size.heightPx)}</em>
                  {#if sizeNotes(size)}
                    <em>{sizeNotes(size)}</em>
                  {/if}
                </div>
                <div class={`floorplan-size-fields ${size.manuallyEdited ? "manual" : ""}`}>
                  <label>
                    <span>{text.width}</span>
                    <input
                      inputmode="decimal"
                      value={measurementValue(candidate, "width")}
                      placeholder="mm"
                      oninput={(event) => onSizeChange?.(candidate.id, "width", event.currentTarget.value)}
                    />
                  </label>
                  <label>
                    <span>{text.height}</span>
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
                placeholder={text.roomNamePlaceholder}
                oninput={(event) => onRename?.(candidate.id, event.currentTarget.value)}
              />
              <div>
                {#if snapEditActive && snapEditCandidateId === candidate.id}
                  <span class="floorplan-inline-tool-note">{text.inline.snapActive}</span>
                {:else if splitDraftActive && splitDraftCandidateId === candidate.id}
                  <span class="floorplan-inline-tool-note">{text.inline.splitActive}</span>
                {:else if mergeDraftActive}
                  <span class="floorplan-inline-tool-note">
                    {mergeSelected(candidate) ? text.inline.mergeSelected : text.inline.mergePickMore}
                  </span>
                {:else if showCandidateTools}
                  <button type="button" onclick={() => onStartSnapEdit?.(candidate.id)}>{text.actions.snap}</button>
                  <button type="button" onclick={() => onStartSplitDraft?.(candidate.id)}>{text.actions.split}</button>
                  <button type="button" onclick={() => onStartMergeDraft?.()}>{text.actions.merge}</button>
                {/if}
                <button type="button" onclick={() => onConfirm?.(candidate.id)}>{text.actions.confirm}</button>
                <button type="button" onclick={() => onReject?.(candidate.id)}>{text.actions.reject}</button>
                <button type="button" class="danger-button" onclick={() => onRemove?.(candidate.id)}>{text.actions.remove}</button>
              </div>
            </div>
          {/if}
        </li>
      {/each}
    </ul>
  {:else}
    <span>{text.noCandidates}</span>
  {/if}
</div>
