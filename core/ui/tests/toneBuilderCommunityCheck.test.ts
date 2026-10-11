// @vitest-environment node
import { afterEach, describe, expect, it, vi } from "vitest";

const apiFetch = vi.fn();
vi.mock("../ts/toneSharingPanel/api.js", () => ({ apiFetch }));
vi.mock("../ts/toneSharingPanel/presetLinks.js", () => ({
  buildInstalledToneSharingLookup: () => ({ itemIds: new Set(["itm-2"]), packIds: new Set() }),
}));

const { findCommunityMatches } = await import("../ts/toneBuilder/communityCheck.js");

afterEach(() => apiFetch.mockReset());

describe("findCommunityMatches", () => {
  it("asks for the artist and the song apart, merges, and ranks what names both first", async () => {
    apiFetch.mockImplementation(async (path: string) => {
      if (path.startsWith("/items") && path.includes("q=Dick%20Dale")) {
        return { items: [
          { id: "itm-1", title: "Dick Dale style lead", description: "" },
          { id: "itm-2", title: "Misirlou", description: "Dick Dale surf rhythm" },
        ] };
      }
      if (path.startsWith("/items") && path.includes("q=Misirlou")) {
        return { items: [{ id: "itm-2", title: "Misirlou", description: "Dick Dale surf rhythm" }] };
      }
      if (path.startsWith("/packs") && path.includes("q=Misirlou")) {
        return { packs: [{ id: "pak-1", title: "Misirlou covers", description: null, creatorDisplayName: "Ann" }] };
      }
      return { items: [], packs: [] };
    });
    const matches = await findCommunityMatches("Dick Dale", "Misirlou");
    expect(apiFetch).toHaveBeenCalledTimes(4);
    expect(matches.map((m) => `${m.kind}:${m.id}:${m.score}`)).toEqual(["item:itm-2:2", "pack:pak-1:1", "item:itm-1:1"]);
    expect(matches[0].installed).toBe(true);
    expect(matches[1].creator).toBe("Ann");
  });

  it("carries on when a lookup fails, and asks nothing without an artist or song", async () => {
    apiFetch.mockImplementation(async (path: string) => {
      if (path.startsWith("/packs")) throw new Error("offline");
      return { items: [{ id: "itm-1", title: "Surf Tone", description: "" }] };
    });
    expect(await findCommunityMatches("surf", "")).toHaveLength(1);
    apiFetch.mockClear();
    expect(await findCommunityMatches(" ", "")).toEqual([]);
    expect(apiFetch).not.toHaveBeenCalled();
  });
});
