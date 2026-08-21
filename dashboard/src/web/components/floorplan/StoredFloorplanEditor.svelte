<script>
  import FloorplanCandidateOverlay from "../FloorplanCandidateOverlay.svelte";
  import FloorplanFurnitureOverlay from "../FloorplanFurnitureOverlay.svelte";
  import FloorplanImageFrame from "./FloorplanImageFrame.svelte";
  import FloorplanMapToolPanel from "./FloorplanMapToolPanel.svelte";
  import FloorplanPhysicalWallOverlay from "./FloorplanPhysicalWallOverlay.svelte";
  import FloorplanRadarAdvancedOptions from "./FloorplanRadarAdvancedOptions.svelte";
  import FloorplanRadarPlacementOverlay from "../FloorplanRadarPlacementOverlay.svelte";
  import ManualFloorplanDoorList from "./ManualFloorplanDoorList.svelte";
  import ManualFloorplanDoorOverlay from "./ManualFloorplanDoorOverlay.svelte";
  import RoomEditStep from "./RoomEditStep.svelte";
  import RoomEditTools from "./RoomEditTools.svelte";
  import StoredFloorplanEditToolTabs from "./StoredFloorplanEditToolTabs.svelte";
  import StoredFloorplanFurnitureTool from "./StoredFloorplanFurnitureTool.svelte";
  import StoredFloorplanLoading from "./StoredFloorplanLoading.svelte";
  import StoredFloorplanSaveStatus from "./StoredFloorplanSaveStatus.svelte";
  import StoredFloorplanScaleTool from "./StoredFloorplanScaleTool.svelte";
  import StoredFloorplanSummary from "./StoredFloorplanSummary.svelte";
  import StoredFloorplanWorkspaceToolbar from "./StoredFloorplanWorkspaceToolbar.svelte";
  import StoredFloorplanZoneTool from "./StoredFloorplanZoneTool.svelte";

  let {
    messages,
    text,
    model
  } = $props();

  const state = $derived(model.state);
  const tools = $derived(model.tools);
  const actions = $derived(model.actions);
</script>

<section
  class={`floorplan-stored-panel ${state.mode === "edit" ? "edit-step" : ""}`}
  aria-label={text.storedTitle}
>
  {#if state.storage.checkBusy}
    <StoredFloorplanLoading {text} />
  {:else}
    <aside class="floorplan-stored-controls">
      <StoredFloorplanSummary
        {text}
        mode={state.mode}
        floorplanName={state.document?.image?.name}
        roomCount={state.document?.rooms?.length ?? 0}
        dirty={state.summary.dirty}
        saveBusy={state.summary.saveBusy}
        canSave={state.summary.canSave}
        onView={actions.view}
        onEdit={actions.edit}
        onReplace={actions.replace}
        onDelete={actions.delete}
        onSave={actions.save}
      />

      {#if state.mode === "edit"}
        {#if state.editTool === "rooms"}
          <RoomEditTools
            {messages}
            candidates={tools.room.candidates}
            selectedCandidateId={tools.room.selectedCandidateId}
            manualRoomDraft={tools.room.manualRoomDraft}
            roomSplitDraft={tools.room.roomSplitDraft}
            roomMergeDraft={tools.room.roomMergeDraft}
            snapEdit={tools.room.snapEdit}
            manualMode={state.structuredManualSource}
            doorPlacementActive={tools.doors.placementActive}
            onStartSplitDraft={actions.startSplitDraft}
            onStartMergeDraft={actions.startMergeDraft}
            onStartSnapEdit={actions.startSnapEdit}
            onStartDoorPlacement={actions.startDoorPlacement}
          />
        {/if}

        <StoredFloorplanEditToolTabs
          {text}
          activeTool={state.editTool}
          onRooms={actions.selectRoomsTool}
          onScale={actions.selectScaleTool}
          onRadar={actions.selectRadarTool}
          onZones={actions.selectZonesTool}
          onCalibration={actions.selectCalibrationTool}
          onFurniture={actions.selectFurnitureTool}
        />
      {/if}

      <StoredFloorplanSaveStatus
        {text}
        tone={state.summary.saveTone}
        status={state.summary.saveStatus}
      />
    </aside>

    {#if state.mode === "edit"}
      <aside class="floorplan-middle-column floorplan-stored-middle-column">
        {#if state.editTool === "rooms"}
          <div class="floorplan-manual-card floorplan-fixed-text">
            <strong>{text.toolTitleManualRoom}</strong>
            <button
              type="button"
              onclick={actions.startManualRoomDraft}
              disabled={tools.room.snapEdit.active || tools.room.roomSplitDraft.active || tools.room.roomMergeDraft.active || tools.room.manualRoomDraft.active}
            >
              {text.toolTitleManualRoom}
            </button>
          </div>
          <RoomEditStep
            {messages}
            candidates={tools.room.candidates}
            selectedCandidateId={tools.room.selectedCandidateId}
            roomSplitDraft={tools.room.roomSplitDraft}
            roomMergeDraft={tools.room.roomMergeDraft}
            snapEdit={tools.room.snapEdit}
            onSelect={actions.selectRoom}
            onRename={actions.renameRoom}
            onConfirm={actions.confirmRoom}
            onReject={actions.rejectRoom}
            onRemove={actions.removeRoom}
            onStartSnapEdit={actions.startSnapEdit}
            onStartSplitDraft={actions.startSplitDraft}
            onStartMergeDraft={actions.startMergeDraft}
          />
          {#if state.structuredManualSource}
            <ManualFloorplanDoorList
              {text}
              doors={tools.doors.doors}
              placementActive={tools.doors.placementActive}
              placementKind={tools.doors.placementKind}
              placementError={tools.doors.placementError}
              selectedDoorId={tools.doors.selectedDoorId}
              onKindChange={actions.setDoorPlacementKind}
              onSelect={actions.selectDoor}
              onRemove={actions.removeDoor}
            />
          {/if}
        {:else if state.editTool === "scale"}
          <StoredFloorplanScaleTool
            {text}
            size={tools.scale.size}
            estimate={tools.scale.estimate}
            valid={tools.scale.valid}
            onWidthInput={actions.scaleWidthInput}
            onHeightInput={actions.scaleHeightInput}
            onCommit={actions.scaleCommit}
          />
        {:else if state.editTool === "zones" || state.editTool === "calibration"}
          <StoredFloorplanZoneTool
            {messages}
            {text}
            source={tools.zone.source}
            zones={tools.zone.zones}
            selection={tools.zone.selection}
            actions={tools.zone.actions}
          />
        {:else if state.editTool === "furniture"}
          <StoredFloorplanFurnitureTool
            {text}
            assets={tools.furniture.assets}
            objects={tools.furniture.objects}
            selectedObjectId={tools.furniture.selectedObjectId}
            hasSelectedObject={tools.furniture.hasSelectedObject}
            targetRoomName={tools.furniture.targetRoomName}
            onAdd={actions.addFurniture}
            onSelect={actions.selectFurniture}
            onDeleteSelected={actions.deleteSelectedFurniture}
          />
        {:else}
          <div class="floorplan-stored-tool-placeholder">
            <strong>{text.storedRadarTitle}</strong>
            <span>{text.storedRadarDescription}</span>
            {#if state.editTool === "radar"}
              <FloorplanRadarAdvancedOptions
                text={messages.floorplan.radarStep}
                occlusionEditActive={tools.radar.occlusionEditActive}
                ignoredOcclusionEdges={tools.radar.ignoredOcclusionEdges}
                onToggleOcclusionEdit={actions.toggleStoredOcclusionEdit}
                onResetOcclusion={actions.resetStoredOcclusionEdges}
              />
            {/if}
          </div>
        {/if}
      </aside>
    {/if}

    <section class="floorplan-stored-workspace" aria-label={text.storedPreviewAria}>
      <div class="floorplan-stage">
        {#if state.document && state.storage.imageUrl}
          <div class="floorplan-stored-preview">
            <FloorplanImageFrame
              imageUrl={state.storage.imageUrl}
              imageWidth={state.document.image.width}
              imageHeight={state.document.image.height}
              viewBounds={tools.workspace.viewBounds}
              frameStyle={tools.workspace.imageFrameStyle}
              ariaLabel={text.storedTitle}
              showImage={!state.structuredManualSource}
              backgroundFill={state.structuredManualSource ? "#f4f6f3" : ""}
            >
              {#if state.mode === "edit"}
                <StoredFloorplanWorkspaceToolbar
                  {text}
                  canUndo={tools.workspace.canUndo}
                  canRedo={tools.workspace.canRedo}
                  furnitureControlsVisible={state.editTool === "furniture"}
                  furnitureSelected={!!tools.furniture.selectedObjectId}
                  radarVisible={tools.workspace.radarVisible}
                  onUndo={actions.undo}
                  onRedo={actions.redo}
                  onRotateCounterClockwise={actions.rotateFurnitureCounterClockwise}
                  onRotateClockwise={actions.rotateFurnitureClockwise}
                  onToggleRadar={actions.toggleRadar}
                />
              {/if}
              {#if tools.mapTool.active}
                <FloorplanMapToolPanel
                  {text}
                  title={tools.mapTool.title}
                  message={tools.mapTool.message}
                  canFinish={tools.mapTool.canFinish}
                  onFinish={actions.finishMapTool}
                  onCancel={actions.cancelMapTool}
                />
              {/if}
              <FloorplanCandidateOverlay
                candidates={tools.room.candidates}
                selectedCandidateId={tools.room.selectedCandidateId}
                selectedCandidateVertexIndex={tools.room.selectedCandidateVertexIndex}
                focusSelectedCandidate={state.editTool === "furniture"}
                imageWidth={state.document.image.width}
                imageHeight={state.document.image.height}
                viewBounds={tools.workspace.viewBounds}
                ariaLabels={{
                  candidateLayer: text.roomCandidateLayerAria,
                  addSplitPoint: text.addRoomSplitPointAria,
                  addManualPoint: text.addManualRoomPointAria
                }}
                manualDraftActive={tools.room.manualRoomDraft.active}
                manualDraftPoints={tools.room.manualRoomDraft.points}
                manualDraftHoverPoint={tools.room.manualRoomDraftHover}
                splitDraftActive={tools.room.roomSplitDraft.active}
                splitDraftPoints={tools.room.roomSplitDraft.points}
                splitDraftHoverPoint={tools.room.roomSplitDraftHover}
                mergeDraftActive={tools.room.roomMergeDraft.active}
                mergeDraftCandidateIds={tools.room.roomMergeDraft.candidateIds}
                allowCandidateEditing={state.mode === "edit" && state.editTool === "rooms"}
                metricMmPerPx={(tools.workspace.scaleEstimate?.mmPerPxX + tools.workspace.scaleEstimate?.mmPerPxY) / 2 || 0}
                showMetricGrid={false}
                snapEdges={tools.workspace.snapEdges}
                selectedSnapEdgeKey={tools.room.snapEdit.edgeKey}
                snapWallSegments={tools.workspace.snapWallSegments}
                onSelect={state.mode === "edit" ? actions.selectRoom : undefined}
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
              {#if state.structuredManualSource}
                <FloorplanPhysicalWallOverlay
                  segments={tools.workspace.physicalWallSegments}
                  imageWidth={state.document.image.width}
                  imageHeight={state.document.image.height}
                  viewBounds={tools.workspace.viewBounds}
                />
              {/if}
              {#if state.structuredManualSource && (tools.doors.doors.length || tools.doors.placementActive)}
                <ManualFloorplanDoorOverlay
                  doors={tools.doors.doors}
                  placementActive={state.mode === "edit" && state.editTool === "rooms" && tools.doors.placementActive}
                  selectedDoorId={tools.doors.selectedDoorId}
                  imageWidth={state.document.image.width}
                  imageHeight={state.document.image.height}
                  viewBounds={tools.workspace.viewBounds}
                  mmPerPx={(state.document.scale.mmPerPxX + state.document.scale.mmPerPxY) / 2}
                  ariaLabel={text.manualPlaceDoorAria}
                  onPreview={state.editTool === "rooms" ? actions.previewDoor : undefined}
                  onPlace={state.editTool === "rooms" ? actions.placeDoor : undefined}
                  onSelect={state.editTool === "rooms" ? actions.selectDoor : undefined}
                  onResize={state.editTool === "rooms" ? actions.resizeDoor : undefined}
                  onResizeCommit={state.editTool === "rooms" ? actions.resizeDoorCommit : undefined}
                  onMove={state.editTool === "rooms" ? actions.moveDoor : undefined}
                  onMoveCommit={state.editTool === "rooms" ? actions.moveDoorCommit : undefined}
                />
              {/if}
              {#if tools.workspace.radarVisible}
                <FloorplanRadarPlacementOverlay
                  imageWidth={state.document.image.width}
                  imageHeight={state.document.image.height}
                  viewBounds={tools.workspace.viewBounds}
                  scaleEstimate={tools.workspace.scaleEstimate}
                  placement={tools.workspace.radarPlacement}
                  scalePercent={tools.workspace.radarScalePercent}
                  zones={tools.workspace.zones}
                  calibrationZones={tools.workspace.calibrationZones}
                  targets={tools.workspace.targets}
                  peerRadars={tools.workspace.peerRadars}
                  peerRadarLabel={messages.dashboard.connectedRadar}
                  fusionLabel={messages.common.fused}
                  wallSegments={tools.workspace.wallSegments}
                  occlusionSegments={tools.workspace.occlusionSegments}
                  ignoredOcclusionSegmentIds={tools.workspace.ignoredOcclusionSegmentIds}
                  occlusionEditActive={tools.radar.occlusionEditActive}
                  editableZoneSource={tools.workspace.editableZoneSource}
                  selectedZoneId={tools.workspace.selectedZoneId}
                  selectedZonePointIndex={tools.workspace.selectedZonePointIndex}
                  zoneEditable={tools.workspace.zoneEditable}
                  readOnly={state.mode !== "edit" || state.editTool !== "radar"}
                  onChange={actions.changeRadarPlacement}
                  onCommit={actions.commitRadarPlacement}
                  onToggleOcclusionSegment={actions.toggleStoredOcclusionEdge}
                  onSelectZone={actions.selectZone}
                  onZoneMove={actions.moveZone}
                  onZonePointMove={actions.moveZonePoint}
                  onZoneEdgeClick={actions.addZonePoint}
                  onZoneEditCommit={actions.commitZoneEdit}
                />
              {/if}
              <FloorplanFurnitureOverlay
                objects={tools.furniture.objects}
                assets={tools.furniture.assets}
                selectedObjectId={tools.furniture.selectedObjectId}
                imageWidth={state.document.image.width}
                imageHeight={state.document.image.height}
                viewBounds={tools.workspace.viewBounds}
                bounds={tools.workspace.furnitureBounds}
                rooms={tools.workspace.furnitureRooms}
                editable={state.mode === "edit" && state.editTool === "furniture"}
                onSelect={actions.selectFurniture}
                onCommit={actions.commitFurniture}
              />
            </FloorplanImageFrame>
          </div>
        {:else}
          <div class="floorplan-stored-preview floorplan-stored-empty">
            <strong>{text.storedPreparingTitle}</strong>
            <span>{text.storedPreparingDescription}</span>
          </div>
        {/if}
      </div>
    </section>
  {/if}
</section>
