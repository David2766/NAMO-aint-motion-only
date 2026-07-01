<script>
  let {
    candidates = [],
    selectedCandidateId = "",
    manualRoomDraft = { active: false, points: [] },
    roomSplitDraft = { active: false, candidateId: "", points: [] },
    roomMergeDraft = { active: false, candidateIds: [] },
    snapEdit = { active: false, candidateId: "", edgeKey: "" },
    onStartSplitDraft,
    onStartMergeDraft,
    onStartSnapEdit
  } = $props();

  function activeCandidateCount() {
    return candidates.filter((candidate) => candidate.status !== "rejected").length;
  }

  function hasActiveTool() {
    return snapEdit.active || roomSplitDraft.active || roomMergeDraft.active || manualRoomDraft.active;
  }
</script>

<div class="floorplan-edit-tool-card" data-active={hasActiveTool() ? "true" : "false"}>
  <strong>편집 도구</strong>

  <div class="floorplan-edit-tool-grid">
    <button type="button" onclick={() => onStartSplitDraft?.()} disabled={hasActiveTool() || !selectedCandidateId}>방 나누기</button>
    <button type="button" onclick={onStartMergeDraft} disabled={hasActiveTool() || activeCandidateCount() < 2}>방 합치기</button>
    <button type="button" onclick={() => onStartSnapEdit?.(selectedCandidateId)} disabled={hasActiveTool() || !selectedCandidateId}>벽에 맞추기</button>
  </div>
</div>
