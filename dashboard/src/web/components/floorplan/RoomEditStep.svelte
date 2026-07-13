<script>
  import FloorplanCandidateList from "../FloorplanCandidateList.svelte";

  let {
    messages,
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
    onStartSplitDraft,
    onStartMergeDraft
  } = $props();
</script>

<FloorplanCandidateList
  {messages}
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
  onStartSplitDraft={onStartSplitDraft}
  onStartMergeDraft={onStartMergeDraft}
/>
