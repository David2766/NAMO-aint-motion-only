<script lang="ts">
  type Props = {
    zoneId: string;
    onCancel: () => void;
    onConfirm: (zoneId: string) => void;
  };

  let { zoneId, onCancel, onConfirm }: Props = $props();
</script>

<div data-shrink-confirm-dialog>
  {#if zoneId}
    <div class="protected-zone-dialog-backdrop" role="dialog" aria-modal="true" aria-label="오탐 보정 구역 축소 확인">
      <div class="protected-zone-dialog">
        <strong>보정 구역 최소 크기 보호</strong>
        <p>
          최소 기능 보장을 위해 자동으로 설정된 크기보다 작게 수정하는 것은 추천드리지 않습니다.
          진행하시겠습니까?
        </p>
        <p>
          예를 선택하면 이 보정 구역의 최소 보장 크기 보호가 영구 해제되어 이후에는 경고 없이 축소할 수
          있습니다.
        </p>
        <div class="protected-zone-dialog-actions">
          <button type="button" onclick={onCancel}>아니오</button>
          <button type="button" onclick={() => onConfirm(zoneId)}>예</button>
        </div>
      </div>
    </div>
  {/if}
</div>
