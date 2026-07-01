import type { RoomCandidate, RoomCandidateImageMeta, RoomCandidateStatus } from "../../core/floorplan/floorplan-types";
import { createMockRoomCandidates } from "../../core/floorplan/room-candidate-mock";

export function createRoomCandidateState() {
  let candidates = $state<RoomCandidate[]>([]);
  let selectedCandidateId = $state("");

  function analyzeMock(image: RoomCandidateImageMeta): void {
    candidates = createMockRoomCandidates(image);
    selectedCandidateId = candidates[0]?.id ?? "";
  }

  function setCandidates(nextCandidates: RoomCandidate[]): void {
    candidates = nextCandidates;
    selectedCandidateId = candidates[0]?.id ?? "";
  }

  function replace(nextCandidates: RoomCandidate[], nextSelectedCandidateId = selectedCandidateId): void {
    candidates = nextCandidates;
    selectedCandidateId = candidates.some((candidate) => candidate.id === nextSelectedCandidateId)
      ? nextSelectedCandidateId
      : candidates[0]?.id ?? "";
  }

  function clear(): void {
    candidates = [];
    selectedCandidateId = "";
  }

  function select(id: string): void {
    selectedCandidateId = candidates.some((candidate) => candidate.id === id) ? id : "";
  }

  function rename(id: string, name: string): void {
    candidates = candidates.map((candidate) =>
      candidate.id === id ? { ...candidate, name: name.slice(0, 16) } : candidate
    );
  }

  function setStatus(id: string, status: RoomCandidateStatus): void {
    candidates = candidates.map((candidate) =>
      candidate.id === id ? { ...candidate, status } : candidate
    );
    if (status === "rejected" && selectedCandidateId === id) selectedCandidateId = "";
  }

  function update(id: string, nextCandidate: RoomCandidate): void {
    candidates = candidates.map((candidate) =>
      candidate.id === id ? nextCandidate : candidate
    );
  }

  function add(candidate: RoomCandidate): void {
    candidates = [...candidates, candidate];
    selectedCandidateId = candidate.id;
  }

  function remove(id: string): void {
    candidates = candidates.filter((candidate) => candidate.id !== id);
    if (selectedCandidateId === id) selectedCandidateId = "";
  }

  return {
    get candidates() {
      return candidates;
    },
    get selectedCandidateId() {
      return selectedCandidateId;
    },
    analyzeMock,
    setCandidates,
    replace,
    clear,
    select,
    rename,
    setStatus,
    update,
    add,
    remove
  };
}
