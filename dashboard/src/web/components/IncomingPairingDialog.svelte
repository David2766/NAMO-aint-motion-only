<script lang="ts">
  import { onDestroy, untrack } from "svelte";
  import {
    createPlatformPairingClient,
    pairingConnectionGuard,
    PlatformPairingError,
    type PlatformPairingStatus
  } from "../api/platform-pairing";
  import type { Messages } from "../i18n";

  type Props = {
    available: boolean;
    baseUrl: string;
    fetcher?: typeof fetch;
    messages: Messages;
  };

  let { available, baseUrl, fetcher, messages }: Props = $props();
  let status = $state<PlatformPairingStatus | null>(null);
  let busy = $state(false);
  let polling = false;
  let error = $state("");
  let connectionGuard = $state<"owner" | "continuity" | "">("");
  let connectionGuardOwnerUrl = $state("");
  let pollTimer: ReturnType<typeof setTimeout> | undefined;
  let pollGeneration = 0;

  const client = $derived(createPlatformPairingClient({ fetcher }));
  const text = $derived(messages.dashboard.pairing);
  const open = $derived(
    status?.invitation?.state === "pending" ||
      (status?.role === "joiner" &&
        status.state !== "idle" &&
        status.state !== "committed")
  );

  $effect(() => {
    const deviceBaseUrl = baseUrl;
    if (!available) {
      status = null;
      stopPolling();
      return;
    }
    untrack(() => {
      const generation = pollGeneration;
      void refresh(deviceBaseUrl);
      schedulePoll(deviceBaseUrl, generation);
    });
    return stopPolling;
  });

  onDestroy(stopPolling);

  function stopPolling(): void {
    pollGeneration += 1;
    if (pollTimer !== undefined) clearTimeout(pollTimer);
    pollTimer = undefined;
  }

  function schedulePoll(deviceBaseUrl: string, generation = pollGeneration): void {
    if (pollTimer !== undefined) clearTimeout(pollTimer);
    pollTimer = undefined;
    if (!available || generation !== pollGeneration) return;
    pollTimer = setTimeout(async () => {
      pollTimer = undefined;
      await refresh(deviceBaseUrl);
      if (generation === pollGeneration) schedulePoll(deviceBaseUrl, generation);
    }, 1500);
  }

  async function refresh(deviceBaseUrl = baseUrl): Promise<void> {
    if (!available || polling || busy) return;
    polling = true;
    try {
      status = await client.getStatus(deviceBaseUrl);
      error = "";
    } catch {
      // A transient read failure must not dismiss an invitation already shown.
    } finally {
      polling = false;
    }
  }

  function messageForError(cause: unknown): string {
    const code = cause instanceof PlatformPairingError ? cause.code : "pairing_request_failed";
    return text.error(code);
  }

  function ownerPairingUrl(address: string): string {
    if (!address.trim()) return "";
    const origin = /^https?:\/\//i.test(address) ? address : `http://${address}`;
    const url = new URL("/dashboard", origin);
    url.searchParams.set("tab", "pairing");
    url.searchParams.set("returnTo", window.location.href);
    return url.toString();
  }

  async function allowIncoming(): Promise<void> {
    if (busy) return;
    busy = true;
    error = "";
    try {
      const [site, participants] = await Promise.all([
        client.getSiteConfiguration(baseUrl),
        client.getParticipants(baseUrl)
      ]);
      const guard = pairingConnectionGuard(site, participants);
      if (!guard.allowed) {
        connectionGuard = guard.reason;
        connectionGuardOwnerUrl = ownerPairingUrl(guard.ownerAddress);
        return;
      }
      status = await client.prepareJoiner(baseUrl);
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }

  async function rejectIncoming(): Promise<void> {
    if (busy) return;
    busy = true;
    error = "";
    try {
      status = await client.rejectInvitation(baseUrl);
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }

  async function cancelIncoming(): Promise<void> {
    if (busy) return;
    busy = true;
    error = "";
    try {
      status = await client.cancel(baseUrl);
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }
</script>

{#if connectionGuard}
  <div class="dashboard-dialog-backdrop" role="presentation">
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="global-pairing-owner-required-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="global-pairing-owner-required-title">
          {connectionGuard === "owner" ? text.ownerRequiredTitle : text.connectionNotReadyTitle}
        </strong>
        <span>
          {connectionGuard === "owner"
            ? text.ownerRequiredDescription
            : text.connectionNotReadyDescription}
        </span>
      </div>
      {#if connectionGuard === "owner" && !connectionGuardOwnerUrl}
        <div class="dashboard-dialog-section">
          <span>{text.ownerUnavailable}</span>
        </div>
      {/if}
      <div class="dashboard-dialog-actions">
        {#if connectionGuardOwnerUrl}
          <button type="button" onclick={() => window.location.assign(connectionGuardOwnerUrl)}>
            {text.openOwnerDevice}
          </button>
        {/if}
        <button
          type="button"
          class="dashboard-dialog-close-button"
          onclick={() => (connectionGuard = "")}
        >
          {messages.common.close}
        </button>
      </div>
    </div>
  </div>
{:else if open}
  <div class="dashboard-dialog-backdrop" role="presentation">
    <div
      class="floorplan-delete-dialog dashboard-control-dialog pairing-incoming-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="global-pairing-incoming-title"
    >
      {#if status?.invitation?.state === "pending"}
        <div class="dashboard-dialog-heading">
          <strong id="global-pairing-incoming-title">{text.incomingRequestTitle}</strong>
          <span>{text.incomingRequestDescription}</span>
        </div>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>{text.deviceAddress}</dt>
            <dd>{status.invitation.requesterAddress}</dd>
          </div>
        </dl>
        <div class="floorplan-delete-dialog-actions">
          <button type="button" disabled={busy} onclick={rejectIncoming}>
            {text.rejectIncoming}
          </button>
          <button class="pairing-incoming-action" type="button" disabled={busy} onclick={allowIncoming}>
            {text.allowIncoming}
          </button>
        </div>
      {:else}
        <div class="dashboard-dialog-heading">
          <strong id="global-pairing-incoming-title">{text.incomingProgressTitle}</strong>
          <span>{status?.confirmationCode ? text.codeHelp : text.incomingProgressDescription}</span>
        </div>
        {#if status?.confirmationCode}
          <div class="floorplan-edit-tool-card pairing-code" role="status">
            <span>{text.codeLabel}</span>
            <strong>{status.confirmationCode}</strong>
          </div>
        {/if}
        <div class="floorplan-delete-dialog-actions">
          <button type="button" disabled={busy} onclick={cancelIncoming}>{text.cancel}</button>
        </div>
      {/if}
      {#if error}<p class="floorplan-save-status" data-tone="error" role="alert">{error}</p>{/if}
    </div>
  </div>
{/if}

<style>
  .pairing-incoming-dialog {
    width: min(520px, calc(100vw - 32px));
  }

  .pairing-incoming-action {
    min-height: 34px;
    padding: 0 12px;
    font-size: var(--rz-font-button);
    font-weight: var(--rz-weight-button);
    line-height: 1.2;
    letter-spacing: 0;
    white-space: normal;
  }

  .pairing-code {
    text-align: center;
  }

  .pairing-code > strong {
    font-size: 28px;
    letter-spacing: 0;
    font-variant-numeric: tabular-nums;
  }
</style>
