// @vitest-environment jsdom
import { describe, expect, it, vi } from "vitest";

const toneSharingFetch = vi.fn();
vi.mock("../ts/toneSharingPanel/api.js", () => ({ toneSharingFetch }));
let flagOn = true;
vi.mock("../ts/featureFlags.js", () => ({ Features: { ToneBuilder: "toneBuilder" }, isFeatureEnabled: () => flagOn }));

const { isToneBuilderOffered, refreshToneBuilderStatus, TONE_BUILDER_STATUS_EVENT } = await import("../ts/toneBuilder/availability.js");

const answer = (data: unknown, status = 200) => new Response(JSON.stringify({ ok: true, data }), { status });

describe("the Tone Builder service status", () => {
  it("offers the button only once the service offers it to this account, and follows the feature flag", async () => {
    // Not asked yet: hidden, as most accounts do not have it.
    expect(isToneBuilderOffered()).toBe(false);
    const events: unknown[] = [];
    document.addEventListener(TONE_BUILDER_STATUS_EVENT, (event) => events.push((event as CustomEvent).detail));

    toneSharingFetch.mockResolvedValueOnce(answer({ enabled: false, available: false, reason: "not-enabled", message: "Not on your account." }));
    expect(await refreshToneBuilderStatus()).toMatchObject({ enabled: false, reason: "not-enabled" });
    expect(isToneBuilderOffered()).toBe(false);

    toneSharingFetch.mockResolvedValueOnce(answer({ enabled: true, available: false, reason: "spend-limit", message: "Try again tomorrow." }));
    expect(await refreshToneBuilderStatus()).toMatchObject({ available: false, reason: "spend-limit" });
    expect(toneSharingFetch).toHaveBeenCalledWith("/tone-builder/status", { cache: "no-store" });
    // At the spend limit the button stays; opening it says why.
    expect(isToneBuilderOffered()).toBe(true);

    toneSharingFetch.mockResolvedValueOnce(answer({ enabled: false, available: false, reason: "disabled", message: "Off." }));
    await refreshToneBuilderStatus();
    expect(isToneBuilderOffered()).toBe(false);
    expect(events).toHaveLength(3);

    // Unreachable: the last answer stands.
    toneSharingFetch.mockRejectedValueOnce(new Error("offline"));
    expect(await refreshToneBuilderStatus()).toMatchObject({ enabled: false });
    toneSharingFetch.mockResolvedValueOnce(answer(null, 500));
    expect(await refreshToneBuilderStatus()).toMatchObject({ enabled: false });
    expect(events).toHaveLength(3);

    toneSharingFetch.mockResolvedValueOnce(answer({ enabled: true, available: true, reason: null, message: null }));
    await refreshToneBuilderStatus();
    expect(isToneBuilderOffered()).toBe(true);
    flagOn = false;
    expect(isToneBuilderOffered()).toBe(false);
  });
});
