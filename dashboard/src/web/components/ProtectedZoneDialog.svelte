<script lang="ts">
  type Props = {
    open: boolean;
    onClose: () => void;
  };

  let { open, onClose }: Props = $props();
</script>

<div data-protected-zone-dialog>
  {#if open}
    <div class="protected-zone-dialog-backdrop" role="dialog" aria-modal="true" aria-label="오탐 보정 구역 안내">
      <div class="protected-zone-dialog">
        <strong>오탐 보정 구역은 보호되어 있습니다</strong>
        <p>
          오탐 보정 구역은 자동 보정으로 생성된 제외 영역입니다. 일반 구역 편집 중 실수로 변경되는 것을 막기
          위해 레이더맵에서는 직접 선택하거나 편집하지 않습니다.
        </p>
        <p>
          비활성화 또는 삭제가 필요하면 왼쪽의 오탐 보정 목록에서 해당 구역을 선택해 관리하세요.
        </p>
        <button type="button" onclick={onClose}>확인</button>
      </div>
    </div>
  {/if}
</div>
