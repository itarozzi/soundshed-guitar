import { describe, expect, it } from "vitest";
import { classifyDraggedResourceTypes } from "../ts/signalPath/resourceFileDrag";

describe("classifyDraggedResourceTypes", () => {
  it("treats audio as an IR", () => {
    expect(classifyDraggedResourceTypes(["audio/wav"])).toEqual(["ir"]);
    expect(classifyDraggedResourceTypes(["audio/x-flac"])).toEqual(["ir"]);
  });

  it("treats an unregistered type as a model, since .nam files have none", () => {
    expect(classifyDraggedResourceTypes([""])).toEqual(["nam"]);
    expect(classifyDraggedResourceTypes(["application/octet-stream"])).toEqual(["nam"]);
  });

  it("lights nothing for a preset pack or an image", () => {
    expect(classifyDraggedResourceTypes(["application/x-zip-compressed"])).toEqual([]);
    expect(classifyDraggedResourceTypes(["image/png"])).toEqual([]);
  });

  it("reports both kinds for a mixed drag, once each", () => {
    expect(classifyDraggedResourceTypes(["", "audio/wav", "audio/wav"])).toEqual(["ir", "nam"]);
  });
});
