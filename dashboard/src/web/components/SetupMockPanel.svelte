<script lang="ts">
  type MockNetwork = {
    ssid: string;
    rssi: number;
    locked: boolean;
  };

  let { onComplete = () => {} } = $props<{ onComplete?: () => void }>();

  const networks: MockNetwork[] = [
    { ssid: "TEST_SSID", rssi: -31, locked: true },
    { ssid: "LivingRoom_5G", rssi: -44, locked: true },
    { ssid: "Kitchen-IoT", rssi: -58, locked: true },
    { ssid: "Galaxy Hotspot", rssi: -63, locked: true },
    { ssid: "HomeMesh_Extender", rssi: -68, locked: true },
    { ssid: "Neighbor_WiFi_2.4G", rssi: -73, locked: true },
    { ssid: "Office Guest", rssi: -77, locked: false },
    { ssid: "Very-Long-WiFi-Name-For-Mobile-Layout-Check", rssi: -82, locked: true }
  ].sort((a, b) => b.rssi - a.rssi);

  const fakeApiKey = "DEMO_ONLY_NOT_SECURE_API_KEY!!!!";
  const fakeIp = "192.168.0.0";
  const fakeDashboard = `${fakeIp}/dashboard`;

  let selected = $state("");
  let ssidInput = $state("");
  let password = $state("");
  let expanded = $state(false);
  let message = $state("주변 Wi-Fi 목록을 불러왔습니다.");
  let messageTone = $state<"" | "ok" | "error">("");
  let modalOpen = $state(false);
  let modalMode = $state<"key" | "connecting" | "failed" | "preparing">("key");
  let complete = $state(false);
  let preparing = $state(false);
  let countdown = $state(10);
  let dashboardReady = $state(false);
  let copyText = $state("복사");
  let timer: number | undefined;

  const visibleNetworks = () => (expanded ? networks : networks.slice(0, 5));
  const selectedSsid = () => ssidInput.trim();

  function bars(rssi: number): string {
    if (rssi >= -55) return "강함";
    if (rssi >= -70) return "보통";
    return "약함";
  }

  function setMessage(text: string, tone: "" | "ok" | "error" = ""): void {
    message = text;
    messageTone = tone;
  }

  function selectNetwork(network: MockNetwork): void {
    if (!network.locked) {
      setMessage("오픈 Wi-Fi는 지원하지 않습니다. 비밀번호가 있는 Wi-Fi를 선택하세요.", "error");
      return;
    }
    selected = network.ssid;
    ssidInput = network.ssid;
    setMessage("Wi-Fi가 선택되었습니다. 비밀번호를 입력하세요.");
  }

  function clearSelection(): void {
    selected = "";
    ssidInput = "";
    setMessage("Wi-Fi를 다시 선택하세요.");
  }

  function refreshNetworks(): void {
    setMessage("데모 목록을 다시 표시했습니다. 실제 Wi-Fi 검색은 실행하지 않습니다.");
  }

  function passwordError(): string {
    if (!selectedSsid()) return "Wi-Fi를 선택하세요.";
    if (password.length < 8) return "비밀번호는 8자 이상이어야 합니다.";
    if (password.length > 63) return "비밀번호는 63자 이하이어야 합니다.";
    return "";
  }

  function connect(): void {
    const error = passwordError();
    if (error) {
      setMessage(error, "error");
      return;
    }
    modalMode = "key";
    modalOpen = true;
    copyText = "복사";
    setMessage("API 키를 확인했습니다. 계속 진행하세요.", "ok");
  }

  async function copyKey(): Promise<void> {
    try {
      await navigator.clipboard?.writeText(fakeApiKey);
      copyText = "복사됨";
    } catch {
      copyText = "확인";
    }
  }

  function continueFromKey(): void {
    modalMode = "connecting";
    setMessage("Wi-Fi 연결을 확인하는 중입니다.");
    window.setTimeout(() => {
      modalOpen = false;
      complete = true;
      setMessage("Wi-Fi 연결이 완료되었습니다.", "ok");
    }, 900);
  }

  function prepareDashboard(): void {
    if (preparing) return;
    preparing = true;
    dashboardReady = false;
    countdown = 10;
    setMessage("설정을 마무리하는 중입니다.", "ok");
    timer = window.setInterval(() => {
      countdown -= 1;
      if (countdown <= 0) {
        if (timer) window.clearInterval(timer);
        timer = undefined;
        dashboardReady = true;
        setMessage("데모 대시보드 접속 준비가 완료되었습니다.", "ok");
      }
    }, 1000);
  }
</script>

<main class="setup-shell">
  {#if complete}
    <section class="setup-card complete-card">
      <div class="status-pill">설정 완료</div>
      <h1 class="title">Wi-Fi 연결이 완료되었습니다</h1>
      <p class="desc">데모 화면입니다. 실제 기기 설정은 저장되지 않습니다.</p>

      <div class="handoff-notice">
        <strong>다음 단계</strong>
        <span>대시보드 접속 준비를 누르면 데모 대시보드로 이동할 수 있습니다.</span>
      </div>

      <div class="info-grid">
        <div>
          <strong>기기 IP</strong>
          <span>{fakeIp}</span>
        </div>
        <div>
          <strong>대시보드</strong>
          <span>{fakeDashboard}</span>
        </div>
      </div>

      {#if preparing && !dashboardReady}
        <button class="btn" type="button" disabled>설정을 마무리하는 중입니다... {countdown}초</button>
      {:else if dashboardReady}
        <button class="btn" type="button" onclick={onComplete}>데모 대시보드 접속</button>
      {:else}
        <button class="btn" type="button" onclick={prepareDashboard}>대시보드 접속 준비</button>
      {/if}
      <div class:ok={messageTone === "ok"} class:error={messageTone === "error"} class="message">{message}</div>
    </section>
  {:else}
    <section class="setup-card">
      <p class="eyebrow">초기 Wi-Fi 설정</p>
      <h1 class="title">Presence Sensor Demo</h1>
      <p class="desc">Wi-Fi를 선택하고 비밀번호를 입력하면 초기 설정 화면의 흐름을 확인할 수 있습니다.</p>
      <div class="notice">이 화면은 레이아웃 확인용 데모입니다. 실제 Wi-Fi 연결이나 ESP32 저장은 실행하지 않습니다.</div>

      <div class="status">
        <span>상태</span><b>설정 대기</b>
        <span>설정 주소</span><b>192.168.4.1</b>
      </div>

      <div class="field-label">Wi-Fi 목록</div>
      <div class="networks">
        {#each visibleNetworks() as network}
          <button
            class:selected={network.ssid === selected}
            class:unsupported={!network.locked}
            class="net"
            type="button"
            onclick={() => selectNetwork(network)}
          >
            <span>{network.ssid}</span>
            <small>{network.locked ? "잠금 · " : "오픈 Wi-Fi 지원 안 함 · "}{bars(network.rssi)} · {network.rssi}dBm</small>
          </button>
        {/each}
      </div>
      {#if networks.length > 5}
        <button class="net-more" type="button" onclick={() => (expanded = !expanded)}>
          {expanded ? "접기" : `더 보기 ${networks.length - 5}개`}
        </button>
      {/if}

      <label for="setup-mock-ssid">Wi-Fi 이름</label>
      <div class="ssid-row">
        <input id="setup-mock-ssid" type="text" bind:value={ssidInput} readonly={!!selected} placeholder="Wi-Fi를 선택하세요" />
        {#if selected}
          <button class="icon-button" type="button" onclick={clearSelection}>변경</button>
        {/if}
      </div>

      <label for="setup-mock-password">비밀번호</label>
      <input
        id="setup-mock-password"
        type="password"
        bind:value={password}
        autocomplete="current-password"
        placeholder="Wi-Fi 비밀번호"
      />

      <div class="actions">
        <button class="btn secondary" type="button" onclick={refreshNetworks}>새로고침</button>
        <button class="btn" type="button" disabled={!selectedSsid() || password.length < 8} onclick={connect}>
          연결하기
        </button>
      </div>
      <div class:error={messageTone === "error"} class:ok={messageTone === "ok"} class="message">{message}</div>
    </section>
  {/if}
</main>

{#if modalOpen}
  <div class="modal-backdrop" role="presentation">
    <div class="modal" role="dialog" aria-modal="true" aria-labelledby="setup-modal-title">
      {#if modalMode === "key"}
        <h2 id="setup-modal-title">API 보안 키를 확인했습니다</h2>
        <p>Home Assistant 등록 시 아래 키가 필요합니다. 키를 복사한 뒤 Wi-Fi 설정을 계속 진행하세요.</p>
        <div class="key-box">
          <code>{fakeApiKey}</code>
          <button type="button" onclick={copyKey}>{copyText}</button>
        </div>
        <strong class="warning">API 키를 복사한 뒤 확인 버튼을 눌러 Wi-Fi 설정을 계속 진행하세요.</strong>
        <div class="modal-actions">
          <button class="btn" type="button" onclick={continueFromKey}>확인</button>
        </div>
      {:else}
        <h2 id="setup-modal-title">Wi-Fi 연결 확인 중</h2>
        <p>선택한 Wi-Fi에 연결하고 있습니다.</p>
        <div class="spinner"></div>
      {/if}
    </div>
  </div>
{/if}

<style>
  :global(body) {
    margin: 0;
    background: #0b1117;
  }

  .setup-shell {
    box-sizing: border-box;
    min-height: 100vh;
    min-height: 100dvh;
    background: linear-gradient(180deg, #0b1117, #0e161e);
    color: #e8f0f6;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: clamp(14px, 4vw, 28px);
    font-family:
      system-ui,
      -apple-system,
      BlinkMacSystemFont,
      "Segoe UI",
      sans-serif;
  }

  .setup-card {
    width: min(520px, 100%);
    box-sizing: border-box;
    background: #101923;
    border: 1px solid #263849;
    border-radius: 18px;
    padding: 22px;
    box-shadow: 0 18px 50px rgba(0, 0, 0, 0.35);
  }

  .complete-card {
    display: grid;
    gap: 16px;
  }

  .eyebrow {
    font-size: 13px;
    color: #19b394;
    font-weight: 700;
    margin: 0 0 8px;
  }

  .title {
    font-size: 25px;
    line-height: 1.2;
    font-weight: 800;
    margin: 0 0 8px;
  }

  .desc {
    font-size: 14px;
    line-height: 1.55;
    color: #91a4b5;
    margin: 0 0 18px;
  }

  .notice,
  .handoff-notice {
    margin: 0 0 16px;
    padding: 12px 13px;
    border: 1px solid rgba(25, 179, 148, 0.28);
    border-radius: 12px;
    background: rgba(25, 179, 148, 0.08);
    color: #b8f1e4;
    font-size: 13px;
    line-height: 1.45;
  }

  .handoff-notice {
    display: grid;
    gap: 5px;
    border-color: rgba(255, 209, 102, 0.32);
    background: rgba(255, 209, 102, 0.08);
    color: #ffe4a3;
  }

  .status,
  .info-grid div {
    display: grid;
    grid-template-columns: 1fr auto;
    gap: 8px 14px;
    margin: 0 0 18px;
    padding: 14px;
    border: 1px solid #263849;
    border-radius: 12px;
    background: #0d151d;
  }

  .info-grid {
    display: grid;
    gap: 10px;
  }

  .info-grid div {
    grid-template-columns: 1fr;
    margin: 0;
  }

  .status span,
  .info-grid strong,
  label,
  .field-label {
    font-size: 13px;
    color: #91a4b5;
  }

  .status b,
  .info-grid span {
    font-size: 13px;
    font-weight: 750;
    text-align: right;
    overflow-wrap: anywhere;
  }

  .info-grid span {
    text-align: left;
    color: #e8f0f6;
  }

  label,
  .field-label {
    display: block;
    margin: 14px 0 7px;
  }

  .networks {
    box-sizing: border-box;
    display: grid;
    gap: 8px;
    margin: 8px 0;
    max-height: 36dvh;
    overflow-y: auto;
    overflow-x: hidden;
    padding-right: 2px;
  }

  .net {
    box-sizing: border-box;
    width: 100%;
    max-width: 100%;
    min-width: 0;
    min-height: 50px;
    border: 1px solid #263849;
    border-radius: 12px;
    background: #152330;
    color: #e8f0f6;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 12px;
    text-align: left;
    font: inherit;
  }

  .net span {
    display: block;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .net small {
    color: #91a4b5;
    flex: 0 0 auto;
    white-space: nowrap;
  }

  .net.selected {
    border-color: #19b394;
    box-shadow: 0 0 0 1px #19b394;
  }

  .net.unsupported {
    opacity: 0.62;
  }

  .net-more {
    width: 100%;
    max-width: 100%;
    height: 42px;
    margin: 0 0 12px;
    border: 1px dashed #263849;
    border-radius: 12px;
    background: transparent;
    color: #19b394;
    font: inherit;
    font-weight: 750;
  }

  input {
    width: 100%;
    height: 46px;
    box-sizing: border-box;
    border: 1px solid #263849;
    border-radius: 12px;
    background: #0d151d;
    color: #e8f0f6;
    padding: 0 13px;
    font-size: 16px;
  }

  input:read-only {
    color: #b8f1e4;
  }

  input:focus {
    outline: 2px solid rgba(25, 179, 148, 0.35);
    border-color: #19b394;
  }

  .ssid-row {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px;
  }

  .icon-button {
    height: 46px;
    border: 1px solid #263849;
    border-radius: 12px;
    background: #1a2a38;
    color: #e8f0f6;
    padding: 0 12px;
    font-weight: 750;
  }

  .actions,
  .modal-actions {
    display: flex;
    gap: 10px;
    margin-top: 16px;
  }

  .btn {
    flex: 1;
    height: 46px;
    border: 0;
    border-radius: 12px;
    background: #19b394;
    color: #07110f;
    font-size: 15px;
    font-weight: 750;
  }

  .btn.secondary {
    flex: 0 0 auto;
    border: 1px solid #263849;
    background: #1a2a38;
    color: #e8f0f6;
  }

  .btn:disabled {
    opacity: 0.55;
  }

  .message {
    min-height: 22px;
    margin-top: 13px;
    color: #91a4b5;
    font-size: 14px;
    line-height: 1.45;
  }

  .message.error {
    color: #ff9b9b;
  }

  .message.ok {
    color: #7de2c8;
  }

  .status-pill {
    width: max-content;
    padding: 6px 10px;
    border-radius: 999px;
    background: rgba(25, 179, 148, 0.12);
    color: #7de2c8;
    font-size: 12px;
    font-weight: 800;
  }

  .modal-backdrop {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: rgba(3, 8, 13, 0.72);
    padding: 18px;
  }

  .modal {
    width: min(460px, 100%);
    display: grid;
    gap: 12px;
    background: #101923;
    border: 1px solid #263849;
    border-radius: 18px;
    padding: 20px;
    color: #e8f0f6;
    box-shadow: 0 24px 70px rgba(0, 0, 0, 0.52);
  }

  .modal h2 {
    margin: 0;
    font-size: 21px;
  }

  .modal p {
    margin: 0;
    color: #91a4b5;
    font-size: 14px;
    line-height: 1.55;
  }

  .key-box {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px;
    align-items: center;
    padding: 10px;
    border: 1px solid #263849;
    border-radius: 12px;
    background: #0d151d;
  }

  .key-box code {
    overflow-wrap: anywhere;
    color: #b8f1e4;
    font-size: 13px;
  }

  .key-box button {
    height: 36px;
    border: 0;
    border-radius: 10px;
    background: #19b394;
    color: #07110f;
    font-weight: 750;
    padding: 0 12px;
  }

  .warning {
    color: #ff9b9b;
    font-size: 13px;
    line-height: 1.45;
  }

  .spinner {
    width: 36px;
    height: 36px;
    border: 3px solid rgba(145, 164, 181, 0.25);
    border-top-color: #19b394;
    border-radius: 50%;
    animation: spin 0.85s linear infinite;
    justify-self: center;
    margin: 8px;
  }

  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }

  @media (max-width: 640px) {
    .setup-shell {
      align-items: flex-start;
      padding: max(12px, env(safe-area-inset-top)) max(12px, env(safe-area-inset-right))
        max(16px, env(safe-area-inset-bottom)) max(12px, env(safe-area-inset-left));
    }

    .setup-card {
      border-radius: 14px;
      padding: 16px;
    }

    .title {
      font-size: 22px;
    }

    .status b {
      max-width: 170px;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .networks {
      max-height: 34dvh;
    }

    .net {
      min-height: 54px;
      padding: 13px 12px;
    }

    .actions {
      flex-direction: column-reverse;
    }

    .btn,
    .btn.secondary {
      width: 100%;
      flex: 0 0 auto;
      height: 48px;
    }
  }
</style>
