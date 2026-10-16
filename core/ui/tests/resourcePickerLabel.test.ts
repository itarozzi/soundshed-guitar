import { describe, expect, it } from "vitest";
import {
  applyResourcePickerLabelState,
  findMatchingResourcePickerLabel,
  getResourcePickerLabelClass,
  getResourcePickerLabelTitle,
  renderResourcePickerLabelContent,
  type ResourcePickerLabelLike,
} from "../ts/resourcePickerLabel";

function makeLabel(dataset: ResourcePickerLabelLike["dataset"]): ResourcePickerLabelLike {
  return { dataset };
}

describe("findMatchingResourcePickerLabel", () => {
  it("matches by node + type + index", () => {
    const labels = [
      makeLabel({ nodeId: "amp-1", resourceType: "nam", resourceIndex: "0" }),
      makeLabel({ nodeId: "amp-1", resourceType: "nam", resourceIndex: "1" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "amp-1", "nam", 1);
    expect(result).toBe(labels[1]);
  });

  it("matches exposed resource id when provided", () => {
    const labels = [
      makeLabel({ nodeId: "comp-1", resourceType: "ir", resourceIndex: "0", exposedResourceId: "a" }),
      makeLabel({ nodeId: "comp-1", resourceType: "ir", resourceIndex: "0", exposedResourceId: "b" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "comp-1", "ir", 0, "b");
    expect(result).toBe(labels[1]);
  });

  it("defaults missing index to zero", () => {
    const labels = [
      makeLabel({ nodeId: "amp-1", resourceType: "nam" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "amp-1", "nam", 0);
    expect(result).toBe(labels[0]);
  });

  it("returns null when no candidate matches", () => {
    const labels = [
      makeLabel({ nodeId: "amp-1", resourceType: "nam", resourceIndex: "0" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "amp-2", "nam", 0);
    expect(result).toBeNull();
  });
});
function render(state: Parameters<typeof renderResourcePickerLabelContent>[0]): HTMLButtonElement {
  const label = document.createElement("button");
  label.className = getResourcePickerLabelClass(state);
  label.title = getResourcePickerLabelTitle(state);
  label.innerHTML = renderResourcePickerLabelContent(state);
  return label;
}

describe("resource picker label states", () => {
  it("asks for a model when the slot is empty, and says a file can be dropped", () => {
    const label = render({ resourceType: "nam", displayName: "" });
    expect(label.classList.contains("is-empty")).toBe(true);
    expect(label.querySelector(".resource-picker-label-text")?.textContent).toBe("Choose a model");
    expect(label.querySelector(".resource-picker-label-hint")?.textContent).toBe("or drop a .nam file here");
    expect(label.querySelector(".resource-picker-label-action")?.textContent).toBe("Browse");
    expect(label.querySelector(".resource-picker-label-caret")).toBeNull();
    expect(label.title).toBe("Choose a model, or drop a .nam file here");
  });

  it("asks for an optional second IR quietly", () => {
    const label = render({ resourceType: "ir", displayName: "", isOptional: true });
    expect(label.className).toBe("resource-picker-label is-empty is-optional");
    expect(label.querySelector(".resource-picker-label-text")?.textContent).toBe("Add a second IR");
  });

  it("names a loaded resource, with a caret and no hint", () => {
    const label = render({ resourceType: "ir", displayName: "4x12 V30", isMissing: true });
    expect(label.className).toBe("resource-picker-label is-missing");
    expect(label.querySelector(".resource-picker-label-text")?.textContent).toBe("4x12 V30");
    expect(label.querySelector(".resource-picker-label-caret")).not.toBeNull();
    expect(label.querySelector(".resource-picker-label-hint")).toBeNull();
    expect(label.title).toBe("4x12 V30");
  });

  it("redraws in place from loaded to empty and back, keeping the slot's optional flag", () => {
    const label = render({ resourceType: "ir", displayName: "Room" });
    label.dataset.resourceType = "ir";
    label.dataset.optional = "true";

    applyResourcePickerLabelState(label, "");
    expect(label.className).toBe("resource-picker-label is-empty is-optional");
    expect(label.textContent).toContain("Add a second IR");

    applyResourcePickerLabelState(label, "<b>Hall</b>");
    expect(label.className).toBe("resource-picker-label");
    expect(label.querySelector("b")).toBeNull();
    expect(label.querySelector(".resource-picker-label-text")?.textContent).toBe("<b>Hall</b>");
  });
});
