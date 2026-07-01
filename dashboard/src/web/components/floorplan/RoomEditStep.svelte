<script>
  import FloorplanCandidateList from "../FloorplanCandidateList.svelte";

  let {
    candidates = [],
    selectedCandidateId = "",
    roomSplitDraft = { active: false, candidateId: "", points: [] },
    roomMergeDraft = { active: false, candidateIds: [] },
    snapEdit = { active: false, candidateId: "", edgeKey: "" },
    onSelect,
    onRename,
    onConfirm,
    onReject,
    onRemove,
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
    canFinishMergeDraft
  } = $props();
</script>

<FloorplanCandidateList
  mode="edit"
  candidates={candidates}
  selectedCandidateId={selectedCandidateId}
  onSelect={onSelect}
  onRename={onRename}
  onConfirm={onConfirm}
  onReject={onReject}
  onRemove={onRemove}
  snapEditActive={snapEdit.active}
  snapEditCandidateId={snapEdit.candidateId}
  splitDraftActive={roomSplitDraft.active}
  splitDraftCandidateId={roomSplitDraft.candidateId}
  mergeDraftActive={roomMergeDraft.active}
  mergeDraftCandidateIds={roomMergeDraft.candidateIds}
  selectionLocked={snapEdit.active || roomSplitDraft.active}
  onStartSnapEdit={onStartSnapEdit}
  onFinishSnapEdit={onFinishSnapEdit}
  onCancelSnapEdit={onCancelSnapEdit}
  onStartSplitDraft={onStartSplitDraft}
  onFinishSplitDraft={onFinishSplitDraft}
  onCancelSplitDraft={onCancelSplitDraft}
  canFinishSplitDraft={canFinishSplitDraft}
  onStartMergeDraft={onStartMergeDraft}
  onFinishMergeDraft={onFinishMergeDraft}
  onCancelMergeDraft={onCancelMergeDraft}
  canFinishMergeDraft={canFinishMergeDraft}
/>
