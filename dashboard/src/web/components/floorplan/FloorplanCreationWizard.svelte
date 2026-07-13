<script>
  import FloorplanCandidateList from "../FloorplanCandidateList.svelte";
  import FloorplanCandidateOverlay from "../FloorplanCandidateOverlay.svelte";
  import EditorToolbar from "../EditorToolbar.svelte";
  import FloorplanMapToolPanel from "./FloorplanMapToolPanel.svelte";
  import FloorplanOcrDialog from "../FloorplanOcrDialog.svelte";
  import FloorplanRadarPlacementOverlay from "../FloorplanRadarPlacementOverlay.svelte";
  import FloorplanWizardSidebar from "./FloorplanWizardSidebar.svelte";
  import RoomEditStep from "./RoomEditStep.svelte";

  let {
    messages,
    text,
    debugText,
    model
  } = $props();

  const state = $derived(model.state);
  const sidebar = $derived(model.sidebar);
  const room = $derived(model.room);
  const workspace = $derived(model.workspace);
  const ocr = $derived(model.ocr);
  const actions = $derived(model.actions);
</script>

<section
  class={`floorplan-panel ${state.step === "rooms" || state.step === "ocr" ? "rooms-step" : "image-step"} ${state.animating ? "is-transitioning" : ""} transition-${state.direction}`}
  role="presentation"
  onclick={actions.handleFloorplanPanelClick}
  onkeydown={actions.handleFloorplanPanelKeydown}
>
  <FloorplanWizardSidebar
    {messages}
    {text}
    wizard={{
      step: state.step,
      title: sidebar.title,
      description: sidebar.description
    }}
    image={{
      url: state.imageUrl,
      name: state.imageName,
      meta: state.imageMeta,
      busy: state.busy
    }}
    status={{
      tone: state.statusTone,
      text: state.statusText,
      analysisBusy: state.analysisBusy
    }}
    totalSize={sidebar.totalSize}
    roomTools={{
      ...sidebar.roomTools,
      onStartSplitDraft: actions.startSplitDraft,
      onStartMergeDraft: actions.startMergeDraft,
      onStartSnapEdit: actions.startSnapEdit
    }}
    radarStep={{
      ...sidebar.radarStep,
      onScaleInput: actions.radarScaleInput,
      onScaleNudge: actions.radarScaleNudge,
      onToggleOcclusionEdit: actions.toggleRadarOcclusionEdit
    }}
    finalStep={sidebar.finalStep}
    actions={{
      fileChange: actions.fileChange,
      clearImage: actions.clearImage,
      totalWidthInput: actions.totalWidthInput,
      totalHeightInput: actions.totalHeightInput,
      togglePreciseRoomSizeEditing: actions.togglePreciseRoomSizeEditing,
      startCandidateAnalysis: actions.startCandidateAnalysis,
      goToImage: actions.goToImage,
      goToOcr: actions.goToOcr,
      goToRooms: actions.goToRooms,
      goToRadar: actions.goToRadar,
      goBackToOcr: actions.goBackToOcr,
      goToFinal: actions.goToFinal,
      goBackToRadar: actions.goBackToRadar,
      save: actions.save
    }}
  />

  {#if state.step === "rooms" || state.step === "ocr"}
    <aside class="floorplan-middle-column">
      {#if state.step === "rooms"}
        <div class="floorplan-manual-card floorplan-fixed-text">
          <strong>{text.toolTitleManualRoom}</strong>
          <button
            type="button"
            onclick={actions.startManualRoomDraft}
            disabled={room.snapEdit.active || room.roomSplitDraft.active || room.roomMergeDraft.active || room.manualRoomDraft.active}
          >
            {text.toolTitleManualRoom}
          </button>
        </div>
        <RoomEditStep
          {messages}
          candidates={room.candidates}
          selectedCandidateId={room.selectedCandidateId}
          selectedCandidateVertexIndex={room.selectedCandidateVertexIndex}
          roomSplitDraft={room.roomSplitDraft}
          roomMergeDraft={room.roomMergeDraft}
          snapEdit={room.snapEdit}
          onSelect={actions.selectRoom}
          onRename={actions.renameRoom}
          onConfirm={actions.confirmRoom}
          onReject={actions.rejectRoom}
          onRemove={actions.removeRoom}
          onStartSnapEdit={actions.startSnapEdit}
          onStartSplitDraft={actions.startSplitDraft}
          onStartMergeDraft={actions.startMergeDraft}
        />
      {:else}
        <FloorplanCandidateList
          {messages}
          mode="ocr"
          candidates={room.candidates}
          selectedCandidateId={room.selectedCandidateId}
          measurements={room.measurements}
          estimatedSizes={room.estimatedSizes}
          showEstimatedSizes={room.showEstimatedSizes}
          onSelect={actions.selectRoom}
          onRename={actions.renameRoom}
          onSizeChange={actions.updateRoomMeasurement}
          onConfirm={actions.confirmRoom}
          onReject={actions.rejectRoom}
          onRemove={actions.removeRoom}
        />
      {/if}
    </aside>
  {/if}

  <section class="floorplan-workspace-column" aria-label={text.workspaceAria}>
    <div class="floorplan-stage">
      {#if state.imageUrl}
        <div
          class={`floorplan-image-layer ${state.floorplanView.panning ? "panning" : ""}`}
          style={workspace.imageLayerStyle}
          role="presentation"
          onwheel={actions.handleFloorplanWheel}
          onpointerdown={actions.handleFloorplanPointerDown}
          onpointermove={actions.handleFloorplanPointerMove}
          onpointerup={actions.stopFloorplanPan}
          onpointercancel={actions.stopFloorplanPan}
          onauxclick={actions.preventFloorplanAuxClick}
        >
          {#if state.step === "rooms" || state.step === "final"}
            <EditorToolbar ariaLabel={text.editToolsAria}>
              {#if state.step === "rooms"}
                <button type="button" title={text.undoTitle} onclick={actions.undoCandidateEdit} disabled={!room.canUndo}>&#8630;</button>
                <button type="button" title={text.redoTitle} onclick={actions.redoCandidateEdit} disabled={!room.canRedo}>&#8631;</button>
              {:else}
                <button
                  type="button"
                  title={text.scaledRadarMapTitle}
                  data-active={state.showFinalRadarOverlay ? "true" : "false"}
                  onclick={actions.toggleFinalRadarOverlay}
                >
                  {text.radarMap}
                </button>
              {/if}
            </EditorToolbar>
          {/if}
          {#if workspace.mapToolActive}
            <FloorplanMapToolPanel
              {text}
              title={workspace.mapToolTitle}
              message={workspace.mapToolMessage}
              canFinish={workspace.mapToolCanFinish}
              onFinish={actions.finishMapTool}
              onCancel={actions.cancelMapTool}
            />
          {/if}
          <svg
            class="floorplan-base-svg"
            viewBox={`0 0 ${state.debugInfo?.width ?? 1} ${state.debugInfo?.height ?? 1}`}
            preserveAspectRatio="none"
            style={workspace.transformStyle}
            aria-label={text.uploadedCandidateAria}
            role="img"
          >
            <image
              href={state.imageUrl}
              x="0"
              y="0"
              width={state.debugInfo?.width ?? 1}
              height={state.debugInfo?.height ?? 1}
              preserveAspectRatio="none"
            />
          </svg>
          <FloorplanCandidateOverlay
            transformStyle={workspace.transformStyle}
            candidates={room.candidates}
            selectedCandidateId={room.selectedCandidateId}
            selectionLocked={room.snapEdit.active}
            imageWidth={state.debugInfo?.width ?? 1}
            imageHeight={state.debugInfo?.height ?? 1}
            ariaLabels={{
              candidateLayer: text.roomCandidateLayerAria,
              addSplitPoint: text.addRoomSplitPointAria,
              addManualPoint: text.addManualRoomPointAria
            }}
            manualDraftActive={room.manualRoomDraft.active}
            manualDraftPoints={room.manualRoomDraft.points}
            manualDraftHoverPoint={room.manualRoomDraftHover}
            splitDraftActive={room.roomSplitDraft.active}
            splitDraftPoints={room.roomSplitDraft.points}
            splitDraftHoverPoint={room.roomSplitDraftHover}
            mergeDraftActive={room.roomMergeDraft.active}
            mergeDraftCandidateIds={room.roomMergeDraft.candidateIds}
            allowCandidateEditing={state.step === "rooms"}
            ocrItems={ocr.overlayItems}
            showOcrItems={state.showOcrOverlay && state.step === "ocr"}
            showRoomSizeBounds={room.roomSizeBoundsVisible}
            wallMaskCells={room.wallMaskCells}
            showWallMaskCells={state.showWallMaskOverlay && state.step === "rooms"}
            snapEdges={room.snapEdges}
            selectedSnapEdgeKey={room.snapEdit.edgeKey}
            snapWallSegments={room.snapWallSegments}
            onSelect={actions.selectRoom}
            onManualDraftPoint={actions.addManualRoomPoint}
            onManualDraftPointDelete={actions.deleteManualRoomPoint}
            onManualDraftHover={actions.previewManualRoomPoint}
            onManualDraftLeave={actions.clearManualRoomPointPreview}
            onSplitDraftPoint={actions.addSplitPoint}
            onSplitDraftHover={actions.previewSplitPoint}
            onSplitDraftLeave={actions.clearSplitPreview}
            onSplitDraftPointMove={actions.moveSplitPoint}
            onSelectSnapEdge={actions.selectSnapEdge}
            onSnapToWallSegment={actions.snapSelectedEdgeToWall}
            onCandidateVertexAdd={actions.addCandidateVertex}
            onCandidateVertexSelect={actions.selectCandidateVertex}
            onCandidateVertexDelete={actions.deleteCandidateVertex}
            onCandidateVertexMoveStart={actions.beginCandidateVertexMove}
            onCandidateVertexMove={actions.moveCandidateVertex}
            onCandidateVertexMoveEnd={actions.endCandidateVertexMove}
          />
          {#if state.step === "radar" || (state.step === "final" && state.showFinalRadarOverlay)}
            <FloorplanRadarPlacementOverlay
              transformStyle={workspace.transformStyle}
              imageWidth={state.debugInfo?.width ?? 1}
              imageHeight={state.debugInfo?.height ?? 1}
              scaleEstimate={workspace.scaleEstimate}
              placement={workspace.radarPlacement}
              scalePercent={workspace.radarScalePercent}
              zones={workspace.zones}
              calibrationZones={workspace.calibrationZones}
              targets={workspace.targets}
              wallSegments={workspace.wallSegments}
              occlusionSegments={workspace.occlusionSegments}
              ignoredOcclusionSegmentIds={workspace.ignoredOcclusionSegmentIds}
              occlusionEditActive={sidebar.radarStep.occlusionEditActive}
              readOnly={state.step === "final"}
              onChange={actions.updateRadarPlacement}
              onCommit={actions.commitRadarPlacement}
              onToggleOcclusionSegment={actions.toggleRadarOcclusionEdge}
            />
          {/if}
          <div class="floorplan-zoom-controls" aria-label="Floorplan zoom controls">
            <button type="button" title={text.zoomInTitle} onclick={() => actions.zoomFloorplan(1)}>+</button>
            <button type="button" title={text.zoomOutTitle} onclick={() => actions.zoomFloorplan(-1)} disabled={workspace.floorplanScale <= 1}>-</button>
            <button type="button" title={text.resetZoomTitle} onclick={actions.resetFloorplanZoom}>{Math.round(workspace.floorplanScale * 100)}%</button>
          </div>
        </div>
      {:else}
        <div class="floorplan-preview">
          <div class="floorplan-room living">{text.demoLivingRoom}</div>
          <div class="floorplan-room room-a">{text.demoRoom}</div>
          <div class="floorplan-room room-b">{text.demoRoom}</div>
          <div class="floorplan-radar-dot"></div>
        </div>
      {/if}
    </div>

    <div class="floorplan-debug-row">
      {#if state.step === "ocr"}
        <details class="floorplan-debug floorplan-ocr-debug floorplan-fixed-text">
          <summary>{debugText.ocrDebug}</summary>
          <label class="floorplan-switch-row">
            <input
              type="checkbox"
              checked={state.showOcrOverlay}
              disabled={!ocr.result}
              onchange={(event) => actions.setShowOcrOverlay(event.currentTarget.checked)}
            />
            <span>{debugText.showOcrRegions}</span>
          </label>
          {#if ocr.logs.length}
            <ul>
              {#each ocr.logs as step}
                <li>{step}</li>
              {/each}
            </ul>
          {:else}
            <span>{debugText.ocrNotRun}</span>
          {/if}
          {#if ocr.errorText}
            <div class="floorplan-ocr-error">{ocr.errorText}</div>
          {/if}
          {#if ocr.result}
            <dl>
              <div><dt>{debugText.recognizedWords}</dt><dd>{debugText.count(ocr.result.words.length)}</dd></div>
              <div><dt>{debugText.recognizedLines}</dt><dd>{debugText.count(ocr.result.lines.length)}</dd></div>
              <div><dt>{debugText.recognizedBlocks}</dt><dd>{debugText.count(ocr.result.rawBlockCount)}</dd></div>
              <div><dt>{debugText.rawPieces}</dt><dd>{debugText.count(ocr.result.rawTextBoxes)}</dd></div>
              <div><dt>{debugText.mergedPieces}</dt><dd>{debugText.count(ocr.result.mergedTextBoxes)}</dd></div>
              <div><dt>{debugText.wallBlockedMerges}</dt><dd>{debugText.times(ocr.result.wallBlockedMerges)}</dd></div>
              <div><dt>{debugText.textCandidates}</dt><dd>{debugText.count(ocr.result.textCandidateBoxes)}</dd></div>
              <div><dt>{debugText.ocrCrops}</dt><dd>{debugText.count(ocr.result.ocrRegions)}</dd></div>
              <div><dt>{debugText.removedGuideLines}</dt><dd>{debugText.count(ocr.result.removedGuideLines)}</dd></div>
              <div><dt>{debugText.rotatedOcr}</dt><dd>{debugText.count(ocr.result.rotatedRegions)}</dd></div>
              <div><dt>{debugText.emptyCrops}</dt><dd>{debugText.count(ocr.result.emptyRegions)}</dd></div>
              <div><dt>{debugText.roomNameCandidates}</dt><dd>{debugText.count(ocr.kindCounts["room-label"])}</dd></div>
              <div><dt>{debugText.noiseCandidates}</dt><dd>{debugText.count(ocr.kindCounts.noise)}</dd></div>
              <div><dt>{debugText.fullText}</dt><dd>{ocr.result.text ? debugText.chars(ocr.result.text.length) : debugText.none}</dd></div>
            </dl>
            {#if ocr.result.text}
              <strong class="floorplan-debug-section-title">{debugText.fullText}</strong>
              <p class="floorplan-ocr-text-preview">{ocr.result.text}</p>
            {/if}
            {#if ocr.previewWords.length}
              <strong class="floorplan-debug-section-title">{debugText.preview}</strong>
              <ul>
                {#each ocr.previewWords as word}
                  <li>{word.text} - {debugText.confidence(word.confidence)}</li>
                {/each}
              </ul>
            {/if}
          {/if}
        </details>
      {:else if state.step === "rooms" && state.analysisSteps.length}
        <details class="floorplan-debug floorplan-analysis-log floorplan-fixed-text" open={state.analysisBusy}>
          <summary>{debugText.analysisLog}</summary>
          <ul>
            {#each state.analysisSteps as step}
              <li>{step}</li>
            {/each}
          </ul>
        </details>
      {/if}

      {#if state.step === "rooms" && state.analysisDebug}
        <details class="floorplan-debug floorplan-fixed-text">
          <summary>{debugText.roomCandidateDebug}</summary>
          <label class="floorplan-switch-row">
            <input
              type="checkbox"
              checked={state.showWallMaskOverlay}
              disabled={!state.analysisDebug.wallMaskCells?.length}
              onchange={(event) => actions.setShowWallMaskOverlay(event.currentTarget.checked)}
            />
            <span>{debugText.showWallMaskOverlay}</span>
          </label>
          <dl>
            <div><dt>{debugText.analysisMethod}</dt><dd>{state.analysisEngine || state.analysisDebug.engine || "jsfeat-lite-worker"}</dd></div>
            <div><dt>{debugText.candidatesBeforeCleanup}</dt><dd>{state.analysisDebug.acceptedBeforeSanitize}</dd></div>
            <div><dt>{debugText.finalCandidates}</dt><dd>{state.analysisDebug.acceptedAfterSanitize}</dd></div>
            <div><dt>{debugText.candidateFilter}</dt><dd>{state.analysisDebug.filtersEnabled === false ? debugText.filterOff : debugText.filterOn}</dd></div>
          </dl>
          {#if state.analysisDebug.edge}
            <strong class="floorplan-debug-section-title">{debugText.edgeAnalysisDetail}</strong>
            <dl>
              <div><dt>{debugText.cellSize}</dt><dd>{state.analysisDebug.edge.cellSize}px</dd></div>
              <div><dt>{debugText.analysisGrid}</dt><dd>{state.analysisDebug.edge.gridWidth} x {state.analysisDebug.edge.gridHeight}</dd></div>
              <div><dt>{debugText.totalCells}</dt><dd>{state.analysisDebug.edge.totalCells}</dd></div>
              <div><dt>{debugText.edgePixels}</dt><dd>{state.analysisDebug.edge.edgePixels}</dd></div>
              <div><dt>{debugText.wallCandidateCells}</dt><dd>{state.analysisDebug.edge.wallCells}</dd></div>
              <div><dt>{debugText.expandedWallCells}</dt><dd>{state.analysisDebug.edge.expandedWallCells}</dd></div>
              <div><dt>{debugText.freeSpaceRegions}</dt><dd>{state.analysisDebug.edge.freeComponents}</dd></div>
              <div><dt>{debugText.cannyThreshold}</dt><dd>{state.analysisDebug.edge.lowThreshold} / {state.analysisDebug.edge.highThreshold}</dd></div>
            </dl>
          {/if}
          {#if state.analysisDebug.color}
            <strong class="floorplan-debug-section-title">{debugText.colorFillAnalysisDetail}</strong>
            <dl>
              <div><dt>{debugText.cellSize}</dt><dd>{state.analysisDebug.color.cellSize}px</dd></div>
              <div><dt>{debugText.analysisGrid}</dt><dd>{state.analysisDebug.color.gridWidth} x {state.analysisDebug.color.gridHeight}</dd></div>
              <div><dt>{debugText.totalCells}</dt><dd>{state.analysisDebug.color.totalCells}</dd></div>
              <div><dt>{debugText.colorPixels}</dt><dd>{state.analysisDebug.color.coloredPixels}</dd></div>
              <div><dt>{debugText.colorCandidateCells}</dt><dd>{state.analysisDebug.color.coloredCells}</dd></div>
              <div><dt>{debugText.connectedRegions}</dt><dd>{state.analysisDebug.color.components}</dd></div>
            </dl>
          {/if}
          {#if state.analysisDebug.wall}
            <strong class="floorplan-debug-section-title">{debugText.wallLineAnalysisDetail}</strong>
            <dl>
              <div><dt>{debugText.cellSize}</dt><dd>{state.analysisDebug.wall.cellSize}px</dd></div>
              <div><dt>{debugText.analysisGrid}</dt><dd>{state.analysisDebug.wall.gridWidth} x {state.analysisDebug.wall.gridHeight}</dd></div>
              <div><dt>{debugText.totalCells}</dt><dd>{state.analysisDebug.wall.totalCells}</dd></div>
              <div><dt>{debugText.darkPixels}</dt><dd>{state.analysisDebug.wall.darkPixels}</dd></div>
              <div><dt>{debugText.wallCandidateCells}</dt><dd>{state.analysisDebug.wall.wallCells}</dd></div>
              <div><dt>{debugText.expandedWallCells}</dt><dd>{state.analysisDebug.wall.expandedWallCells}</dd></div>
              <div><dt>{debugText.freeSpaceRegions}</dt><dd>{state.analysisDebug.wall.freeComponents}</dd></div>
            </dl>
          {/if}
          {#if state.analysisDebug.wallMaskCells?.length}
            <strong class="floorplan-debug-section-title">{debugText.wallSelectionDebug}</strong>
            <dl>
              <div><dt>{debugText.wallCellCoordinates}</dt><dd>{debugText.count(state.analysisDebug.wallMaskCells.length)}</dd></div>
              <div><dt>{debugText.rejectedCandidates}</dt><dd>{debugText.count(state.analysisDebug.wallRejectedCells?.length ?? 0)}</dd></div>
              {#each Object.entries(actions.wallMaskReasonCounts()) as [reason, count]}
                <div><dt>{reason}</dt><dd>{debugText.count(count)}</dd></div>
              {/each}
            </dl>
            <button type="button" class="floorplan-debug-download" onclick={actions.downloadWallSelectionDebug}>
              {debugText.saveWallSelectionDebugTxt}
            </button>
            <p class="floorplan-debug-note">{debugText.wallSelectionDebugNote}</p>
          {/if}
          {#if state.analysisDebug.wallSnap}
            <strong class="floorplan-debug-section-title">{debugText.wallSnapDebug}</strong>
            <dl>
              <div><dt>{debugText.checkedEdges}</dt><dd>{state.analysisDebug.wallSnap.edgeChecks}</dd></div>
              <div><dt>{debugText.notAxisAligned}</dt><dd>{state.analysisDebug.wallSnap.noAxisEdges}</dd></div>
              <div><dt>{debugText.candidateQueries}</dt><dd>{state.analysisDebug.wallSnap.candidateQueries}</dd></div>
              <div><dt>{debugText.noRedCandidateLine}</dt><dd>{state.analysisDebug.wallSnap.candidateNone}</dd></div>
              <div><dt>{debugText.alreadyAligned}</dt><dd>{state.analysisDebug.wallSnap.alreadyAligned}</dd></div>
              <div><dt>{debugText.moveAttempts}</dt><dd>{state.analysisDebug.wallSnap.moveAttempts}</dd></div>
              <div><dt>{debugText.wallSnapApplied}</dt><dd>{state.analysisDebug.wallSnap.applied}</dd></div>
              <div><dt>{debugText.selfIntersectionRejected}</dt><dd>{state.analysisDebug.wallSnap.rejectedSelfIntersection}</dd></div>
              <div><dt>{debugText.overExpansionRejected}</dt><dd>{state.analysisDebug.wallSnap.rejectedOverExpanded}</dd></div>
              <div><dt>{debugText.cornerAttempts}</dt><dd>{state.analysisDebug.wallSnap.cornerAttempts}</dd></div>
              <div><dt>{debugText.cornerApplied}</dt><dd>{state.analysisDebug.wallSnap.cornerApplied}</dd></div>
              <div><dt>{debugText.noIntersection}</dt><dd>{state.analysisDebug.wallSnap.cornerNoIntersection}</dd></div>
              <div><dt>{debugText.nearExistingPoint}</dt><dd>{state.analysisDebug.wallSnap.cornerExistingPoint}</dd></div>
              <div><dt>{debugText.spikeCornerRejected}</dt><dd>{state.analysisDebug.wallSnap.cornerSpike}</dd></div>
              <div><dt>{debugText.cornerSelfIntersection}</dt><dd>{state.analysisDebug.wallSnap.cornerSelfIntersection}</dd></div>
            </dl>
            {#if state.analysisDebug.wallSnap.logs?.length}
              <strong class="floorplan-debug-section-title">{debugText.wallSnapLog}</strong>
              <ul>
                {#each state.analysisDebug.wallSnap.logs as log}
                  <li>{log}</li>
                {/each}
              </ul>
            {/if}
          {/if}
          {#if !state.analysisDebug.edge && !state.analysisDebug.color}
            <dl>
              {#if state.analysisDebug.cellSize}
                <div><dt>{debugText.cellSize}</dt><dd>{state.analysisDebug.cellSize}px</dd></div>
              {/if}
              {#if state.analysisDebug.gridWidth}
                <div><dt>{debugText.analysisGrid}</dt><dd>{state.analysisDebug.gridWidth} x {state.analysisDebug.gridHeight}</dd></div>
                <div><dt>{debugText.totalCells}</dt><dd>{state.analysisDebug.totalCells}</dd></div>
                {#if state.analysisDebug.seedCells !== undefined}
                  <div><dt>{debugText.initialCandidateCells}</dt><dd>{state.analysisDebug.seedCells}</dd></div>
                {/if}
                {#if state.analysisDebug.expandedCells !== undefined}
                  <div><dt>{debugText.expandedCells}</dt><dd>{state.analysisDebug.expandedCells}</dd></div>
                {/if}
                {#if state.analysisDebug.components !== undefined}
                  <div><dt>{debugText.connectedRegions}</dt><dd>{state.analysisDebug.components}</dd></div>
                {/if}
              {/if}
              {#if state.analysisDebug.edgePixels !== undefined}
                <div><dt>{debugText.edgePixels}</dt><dd>{state.analysisDebug.edgePixels}</dd></div>
                <div><dt>{debugText.wallCandidateCells}</dt><dd>{state.analysisDebug.wallCells}</dd></div>
                <div><dt>{debugText.expandedWallCells}</dt><dd>{state.analysisDebug.expandedWallCells}</dd></div>
                <div><dt>{debugText.freeSpaceRegions}</dt><dd>{state.analysisDebug.freeComponents}</dd></div>
                <div><dt>{debugText.cannyThreshold}</dt><dd>{state.analysisDebug.lowThreshold} / {state.analysisDebug.highThreshold}</dd></div>
              {/if}
              {#if state.analysisDebug.coloredPixels !== undefined}
                <div><dt>{debugText.colorPixels}</dt><dd>{state.analysisDebug.coloredPixels}</dd></div>
                <div><dt>{debugText.colorCandidateCells}</dt><dd>{state.analysisDebug.coloredCells}</dd></div>
              {/if}
              {#if state.analysisDebug.darkPixels !== undefined}
                <div><dt>{debugText.darkPixels}</dt><dd>{state.analysisDebug.darkPixels}</dd></div>
                <div><dt>{debugText.wallCandidateCells}</dt><dd>{state.analysisDebug.wallCells}</dd></div>
                <div><dt>{debugText.expandedWallCells}</dt><dd>{state.analysisDebug.expandedWallCells}</dd></div>
                <div><dt>{debugText.freeSpaceRegions}</dt><dd>{state.analysisDebug.freeComponents}</dd></div>
              {/if}
            </dl>
          {/if}
          <ul>
            <li>{debugText.rejectedTooSmallBlob}: {state.analysisDebug.rejectedSmall}</li>
            <li>{debugText.rejectedTooSmallArea}: {state.analysisDebug.rejectedTooSmallArea}</li>
            <li>{debugText.rejectedTooLargeArea}: {state.analysisDebug.rejectedTooLargeArea}</li>
            <li>{debugText.rejectedSparse}: {state.analysisDebug.rejectedSparse}</li>
            <li>{debugText.rejectedThin}: {state.analysisDebug.rejectedThin}</li>
          </ul>
        </details>
      {/if}
    </div>
  </section>
</section>

<FloorplanOcrDialog
  {messages}
  open={ocr.open}
  busy={ocr.busy}
  progress={ocr.progress}
  statusText={ocr.statusText}
  logs={ocr.logs}
  resultSummary={ocr.resultSummary}
  errorText={ocr.errorText}
  onCancel={actions.cancelFloorplanOcr}
  onClose={actions.closeFloorplanOcrDialog}
/>
