<script>
  import FinalStep from "./FinalStep.svelte";
  import ManualFloorplanControls from "./ManualFloorplanControls.svelte";
  import RadarStep from "./RadarStep.svelte";
  import RoomEditTools from "./RoomEditTools.svelte";

  let {
    messages,
    text,
    wizard,
    image,
    manual,
    replacement = false,
    status,
    totalSize,
    roomTools,
    radarStep,
    finalStep,
    actions
  } = $props();
</script>

<aside class="floorplan-controls">
  <div class="floorplan-workflow-card">
    <div>
      <strong>{wizard.title}</strong>
      <span>{wizard.description}</span>
    </div>

    {#if replacement}
      <button type="button" onclick={actions.cancelReplacement}>{text.storedReplaceCancel}</button>
    {/if}

    {#if image.url}
      <div class="floorplan-file-info">
        <strong>{image.name}</strong>
        <span>{image.meta}</span>
      </div>
      <button type="button" class="floorplan-clear-button" onclick={actions.clearImage}>{text.clearImage}</button>
    {/if}

    {#if !image.url}
      <div class="floorplan-source-actions">
        <label class="floorplan-upload-button">
          <input type="file" accept="image/png,image/jpeg,image/webp" onchange={actions.fileChange} disabled={image.busy} />
          {image.busy ? text.converting : text.selectImage}
        </label>
        <button type="button" onclick={actions.startManualFloorplan} disabled={image.busy}>{text.drawFloorplan}</button>
      </div>
    {/if}

    {#if wizard.step === "image"}
      <div class="floorplan-source-status" data-tone={status.tone}>
        <strong>{status.tone === "ok" ? text.statusPassed : status.tone === "error" ? text.statusCheckRequired : text.statusWaiting}</strong>
        <span>{status.text}</span>
      </div>
    {/if}

    {#if manual.active && wizard.step === "rooms"}
      <ManualFloorplanControls
        {text}
        phase={manual.phase}
        activeTool={manual.activeTool}
        outlineEditing={manual.outlineEditing}
        roomCount={manual.roomCount}
        roomsValid={manual.roomsValid}
        onStartOutline={actions.startManualOutline}
        onStartRoom={actions.startManualRoom}
        onEditOutline={actions.editManualOutline}
      />
    {/if}

    {#if wizard.step === "ocr"}
      <div class="floorplan-total-size-card">
        <strong>{text.totalFloorplanSize}</strong>
        <span>{text.totalFloorplanSizeDescription}</span>
        <div class="floorplan-total-size-fields">
          <label>
            <span>{text.totalWidth}</span>
            <input
              inputmode="decimal"
              value={totalSize.value.width}
              placeholder="mm"
              oninput={(event) => actions.totalWidthInput(event.currentTarget.value)}
            />
          </label>
          <label>
            <span>{text.totalHeight}</span>
            <input
              inputmode="decimal"
              value={totalSize.value.height}
              placeholder="mm"
              oninput={(event) => actions.totalHeightInput(event.currentTarget.value)}
            />
          </label>
        </div>
        <button type="button" onclick={actions.togglePreciseRoomSizeEditing} disabled={!totalSize.canCalculate()}>
          {totalSize.preciseEditing ? text.closePreciseTuning : text.preciseTuning}
        </button>
        <em data-tone={totalSize.error ? "error" : totalSize.calculated ? "ok" : "idle"}>{totalSize.message()}</em>
      </div>
    {/if}

    {#if !image.url}<span class="floorplan-source-help">{text.drawFloorplanHint}</span>{/if}
  </div>

  {#if wizard.step === "image"}
    <div class="floorplan-step-actions">
      <button type="button" class="floorplan-step-button floorplan-fixed-text" disabled>
        <span>{text.previousStep}</span>
        <strong>{text.firstStep}</strong>
      </button>
      <button
        type="button"
        class="floorplan-next-button floorplan-fixed-text"
        onclick={actions.startCandidateAnalysis}
        disabled={!image.url || status.tone !== "ok" || status.analysisBusy}
      >
        <span>{status.analysisBusy ? text.analyzing : text.nextStep}</span>
        <strong>{text.roomAutoDetect}</strong>
      </button>
    </div>
  {:else if wizard.step === "rooms"}
    <RoomEditTools
      {messages}
      candidates={roomTools.candidates}
      selectedCandidateId={roomTools.selectedCandidateId}
      manualRoomDraft={roomTools.manualRoomDraft}
      roomSplitDraft={roomTools.roomSplitDraft}
      roomMergeDraft={roomTools.roomMergeDraft}
      snapEdit={roomTools.snapEdit}
      manualMode={manual.active}
      doorPlacementActive={manual.doorPlacementActive}
      onStartSplitDraft={roomTools.onStartSplitDraft}
      onStartMergeDraft={roomTools.onStartMergeDraft}
      onStartSnapEdit={roomTools.onStartSnapEdit}
      onStartDoorPlacement={actions.startDoorPlacement}
    />
    <div class="floorplan-step-actions">
      <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={actions.goToImage}>
        <span>{text.previousStep}</span>
        <strong>{text.imageReady}</strong>
      </button>
      <button
        type="button"
        class="floorplan-next-button floorplan-fixed-text"
        onclick={manual.active ? actions.goToRadar : actions.goToOcr}
        title={manual.active
          ? (!manual.hasOutline
            ? text.manualOutlineRequired
            : manual.roomCount < 1
              ? text.manualRoomRequired
              : !manual.roomsValid
                ? text.manualRoomInvalid
              : text.goToRadarTitle)
          : text.goToOcrTitle}
        disabled={manual.active && (!manual.hasOutline || manual.roomCount < 1 || !manual.roomsValid)}
      >
        <span>{text.nextStep}</span>
        <strong>{manual.active ? text.radarPlacement : text.roomNameRecognition}</strong>
      </button>
    </div>
  {:else if wizard.step === "ocr"}
    <div class="floorplan-step-actions">
      <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={actions.goToRooms}>
        <span>{text.previousStep}</span>
        <strong>{text.roomCandidateCleanup}</strong>
      </button>
      <button
        type="button"
        class="floorplan-next-button floorplan-fixed-text"
        onclick={actions.goToRadar}
        title={radarStep.canGo() ? text.goToRadarTitle : text.goToRadarDisabledTitle}
        disabled={!radarStep.canGo()}
      >
        <span>{text.nextStep}</span>
        <strong>{text.radarPlacement}</strong>
      </button>
    </div>
  {:else if wizard.step === "radar"}
    <RadarStep
      {messages}
      scaleSummary={radarStep.scaleSummary()}
      radarScalePercent={radarStep.scalePercent}
      minScalePercent={radarStep.minScalePercent}
      maxScalePercent={radarStep.maxScalePercent}
      scaleStepPercent={radarStep.scaleStepPercent}
      occlusionEditActive={radarStep.occlusionEditActive}
      ignoredOcclusionEdges={radarStep.ignoredOcclusionEdges}
      onScaleInput={radarStep.onScaleInput}
      onScaleNudge={radarStep.onScaleNudge}
      onToggleOcclusionEdit={radarStep.onToggleOcclusionEdit}
      onResetOcclusion={radarStep.onResetOcclusion}
      onBack={manual.active ? actions.goToRooms : actions.goBackToOcr}
      onNext={actions.goToFinal}
    />
  {:else if wizard.step === "final"}
    {@const storageDocument = finalStep.storageDocument()}
    <FinalStep
      {messages}
      {storageDocument}
      scaleSummary={finalStep.scaleSummary()}
      placement={finalStep.placement()}
      imageName={image.name}
      roomCount={finalStep.roomCount()}
      ignoredOcclusionEdges={finalStep.ignoredOcclusionEdges}
      canSave={Boolean(storageDocument && finalStep.hasImageBlob)}
      saveBusy={finalStep.saveBusy}
      saveStatus={finalStep.saveStatus}
      saveTone={finalStep.saveTone}
      onBack={actions.goBackToRadar}
      onSave={actions.save}
    />
  {/if}
</aside>
