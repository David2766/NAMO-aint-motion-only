import { describe, expect, it, vi } from "vitest";
import type {
  FloorplanWallSegment,
  RoomCandidate
} from "../../core/floorplan/floorplan-types";
import { createRoomCandidateMapTools } from "./useRoomCandidateMapTools.svelte";

function candidateFixture(overrides: Partial<RoomCandidate> = {}): RoomCandidate {
  const points = overrides.points ?? [[0, 0], [100, 0], [100, 80], [0, 80]] as Array<[number, number]>;
  return {
    id: "room_1",
    name: "Room 1",
    kind: "unknown",
    confidence: 100,
    status: "candidate",
    shape: "polygon",
    rect: {
      x: Math.min(...points.map(([x]) => x)),
      y: Math.min(...points.map(([, y]) => y)),
      width: Math.max(...points.map(([x]) => x)) - Math.min(...points.map(([x]) => x)),
      height: Math.max(...points.map(([, y]) => y)) - Math.min(...points.map(([, y]) => y))
    },
    points,
    ...overrides
  };
}

function createCandidateState(initialCandidates = [candidateFixture()]) {
  let candidates = initialCandidates;
  let selectedCandidateId = initialCandidates[0]?.id ?? "";
  return {
    get candidates() {
      return candidates;
    },
    get selectedCandidateId() {
      return selectedCandidateId;
    },
    replace(nextCandidates: RoomCandidate[], nextSelectedCandidateId = selectedCandidateId) {
      candidates = nextCandidates;
      selectedCandidateId = nextCandidates.some((candidate) => candidate.id === nextSelectedCandidateId)
        ? nextSelectedCandidateId
        : nextCandidates[0]?.id ?? "";
    },
    update(id: string, candidate: RoomCandidate) {
      candidates = candidates.map((item) => item.id === id ? candidate : item);
    },
    select(id: string) {
      selectedCandidateId = candidates.some((candidate) => candidate.id === id) ? id : "";
    },
    add(candidate: RoomCandidate) {
      candidates = [...candidates, candidate];
      selectedCandidateId = candidate.id;
    }
  };
}

const text = {
  defaultRoomName: (index: number) => `Room ${index}`,
  defaultRoomCandidateName: (index: number) => `Candidate ${index}`,
  toolTitleSnap: "Snap",
  toolTitleSplit: "Split",
  toolTitleMerge: "Merge",
  toolTitleManualRoom: "Manual",
  snapSelectWall: "Select wall",
  snapSelectEdge: "Select edge",
  splitStart: "Split start",
  splitEnd: "Split end",
  splitReady: "Split ready",
  splitInvalid: "Split invalid",
  mergeSelectTwo: "Merge select two",
  mergeSelectOneMore: "Merge one more",
  mergeReady: "Merge ready",
  manualNeedPoints: "Manual need points",
  manualReady: "Manual ready",
  manualInvalid: "Manual invalid"
};

function createSession(options: {
  candidates?: RoomCandidate[];
  wallSegments?: FloorplanWallSegment[];
  manualPointTransform?: (points: Array<[number, number]>, point: { x: number; y: number }) => { x: number; y: number };
  manualPointValidator?: (points: Array<[number, number]>, point: { x: number; y: number }) => boolean;
  manualDraftValidator?: (points: Array<[number, number]>) => boolean;
  onManualRoomFinished?: (candidate: RoomCandidate) => boolean | void;
} = {}) {
  const state = createCandidateState(options.candidates);
  const edit = {
    pushHistory: vi.fn(),
    withHistory: vi.fn((action: () => void) => action()),
    cleanup: vi.fn(() => false),
    clearSelectedVertex: vi.fn()
  };
  const session = createRoomCandidateMapTools({
    roomCandidates: state,
    roomCandidateEdit: edit,
    getImageSize: () => ({ width: 300, height: 300 }),
    getWallSegments: () => options.wallSegments ?? [],
    getText: () => text,
    getManualPointTransform: () => options.manualPointTransform ?? null,
    getManualPointValidator: () => options.manualPointValidator ?? null,
    getManualDraftValidator: () => options.manualDraftValidator ?? null,
    onManualRoomFinished: options.onManualRoomFinished
  });
  return { state, edit, session };
}

describe("room candidate map tools", () => {
  it("creates a manual room from the current draft", () => {
    const { state, edit, session } = createSession({ candidates: [] });

    session.startManualRoomDraft();
    session.addManualRoomPoint({ x: 10, y: 10 });
    session.addManualRoomPoint({ x: 90, y: 10 });
    session.addManualRoomPoint({ x: 90, y: 70 });
    session.finishManualRoomDraft();

    expect(edit.pushHistory).toHaveBeenCalledTimes(3);
    expect(edit.withHistory).toHaveBeenCalledTimes(1);
    expect(state.candidates).toHaveLength(1);
    expect(state.candidates[0]).toMatchObject({
      name: "Room 1",
      shape: "polygon",
      points: [[10, 10], [90, 10], [90, 70]]
    });
    expect(session.manualRoomDraft.active).toBe(false);
  });

  it("uses the manual floorplan transform and blocks an invalid outer outline", () => {
    const onFinished = vi.fn();
    const { state, session } = createSession({
      candidates: [],
      manualPointTransform: (_points, point) => ({ x: Math.round(point.x / 10) * 10, y: Math.round(point.y / 10) * 10 }),
      manualDraftValidator: (points) => points.length >= 4,
      onManualRoomFinished: onFinished
    });

    session.startManualRoomDraft();
    session.addManualRoomPoint({ x: 13, y: 17 });
    session.addManualRoomPoint({ x: 93, y: 17 });
    session.addManualRoomPoint({ x: 93, y: 77 });

    expect(session.activeMapToolCanFinish()).toBe(false);
    session.finishActiveMapTool();
    expect(state.candidates).toHaveLength(0);

    session.addManualRoomPoint({ x: 13, y: 77 });
    expect(session.activeMapToolCanFinish()).toBe(true);
    session.finishActiveMapTool();

    expect(state.candidates[0].points).toEqual([[10, 20], [90, 20], [90, 80], [10, 80]]);
    expect(state.candidates[0].status).toBe("confirmed");
    expect(state.selectedCandidateId).toBe("");
    expect(onFinished).toHaveBeenCalledWith(state.candidates[0]);
  });

  it("lets the manual floorplan session consume the outer outline without adding it as a room", () => {
    const onFinished = vi.fn(() => false);
    const { state, session } = createSession({
      candidates: [],
      manualPointTransform: (_points, point) => point,
      onManualRoomFinished: onFinished
    });

    session.startManualRoomDraft();
    session.addManualRoomPoint({ x: 10, y: 10 });
    session.addManualRoomPoint({ x: 100, y: 10 });
    session.addManualRoomPoint({ x: 100, y: 100 });
    session.addManualRoomPoint({ x: 10, y: 100 });
    session.finishManualRoomDraft();

    expect(onFinished).toHaveBeenCalledTimes(1);
    expect(state.candidates).toEqual([]);
    expect(session.manualRoomDraft.active).toBe(false);
  });

  it("rejects a manual room point before it can leave the outer boundary", () => {
    const { edit, session } = createSession({
      candidates: [],
      manualPointTransform: (_points, point) => point,
      manualPointValidator: (_points, point) => point.x <= 200
    });

    session.startManualRoomDraft();
    session.addManualRoomPoint({ x: 100, y: 100 });
    session.previewManualRoomPoint({ x: 250, y: 100 });
    expect(session.manualRoomDraftHover).toBeNull();

    session.addManualRoomPoint({ x: 250, y: 100 });
    expect(session.manualRoomDraft.points).toEqual([[100, 100]]);
    expect(edit.pushHistory).toHaveBeenCalledTimes(1);
  });

  it("splits a selected room with the existing split logic", () => {
    const { state, edit, session } = createSession();

    session.startRoomSplitDraft("room_1");
    session.addRoomSplitPoint({ x: 50, y: -20 });
    session.addRoomSplitPoint({ x: 50, y: 120 });

    expect(session.canFinishRoomSplitDraft()).toBe(true);

    session.finishRoomSplitDraft();

    expect(edit.withHistory).toHaveBeenCalledTimes(1);
    expect(state.candidates).toHaveLength(2);
    expect(state.candidates.map((candidate) => candidate.id)).toEqual([
      expect.stringContaining("room_1_split_"),
      expect.stringContaining("room_1_split_")
    ]);
    expect(state.selectedCandidateId).toBe(state.candidates[0].id);
    expect(session.roomSplitDraft.active).toBe(false);
  });

  it("toggles merge selection and merges two candidates", () => {
    const first = candidateFixture({ id: "a", name: "A", points: [[0, 0], [80, 0], [80, 80], [0, 80]] });
    const second = candidateFixture({ id: "b", name: "B", points: [[80, 0], [160, 0], [160, 80], [80, 80]] });
    const { state, edit, session } = createSession({ candidates: [first, second] });

    session.startRoomMergeDraft();
    session.selectRoomCandidate("b");

    expect(session.roomMergeDraft.candidateIds).toEqual(["a", "b"]);
    expect(session.canFinishRoomMergeDraft()).toBe(true);

    session.finishRoomMergeDraft();

    expect(edit.withHistory).toHaveBeenCalledTimes(1);
    expect(state.candidates).toHaveLength(1);
    expect(state.candidates[0]).toMatchObject({
      name: "A",
      shape: "polygon",
      rect: { x: 0, y: 0, width: 160, height: 80 }
    });
    expect(session.roomMergeDraft.active).toBe(false);
  });

  it("restores the original candidate when snap editing is cancelled", () => {
    const wallSegments: FloorplanWallSegment[] = [{
      id: "wall_top",
      axis: "horizontal",
      x1: 0,
      y1: 20,
      x2: 100,
      y2: 20,
      length: 100
    }];
    const { state, edit, session } = createSession({ wallSegments });

    session.startSnapEdit("room_1");
    session.selectSnapEdge("poly:0");
    session.snapSelectedEdgeToWall("wall_top");

    expect(state.candidates[0].points?.[0]).toEqual([0, 20]);
    expect(edit.withHistory).toHaveBeenCalledTimes(1);

    session.cancelSnapEdit();

    expect(state.candidates[0].points?.[0]).toEqual([0, 0]);
    expect(edit.cleanup).toHaveBeenCalledWith("room_1", false);
    expect(session.snapEdit.active).toBe(false);
  });

  it("reports active tool messages from the active map tool", () => {
    const { session } = createSession();

    session.startRoomSplitDraft("room_1");

    expect(session.isActive()).toBe(true);
    expect(session.activeMapToolTitle()).toBe("Split");
    expect(session.activeMapToolMessage()).toBe("Split start");

    session.cancelActiveMapTool();

    expect(session.isActive()).toBe(false);
  });
});
