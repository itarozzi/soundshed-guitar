/**
 * The shapes the designer works in: what is selected, what a drag is moving, and
 * the resources a control can be pointed at.
 */

export type SelectedElement =
  | { type: "control"; paramKey: string }
  | { type: "label"; id: string }
  | { type: "overlay"; id: string }
  | { type: "background"; layerIndex: number }
  | null;

export interface DragState {
  active: boolean;
  element: HTMLElement | null;
  startX: number;
  startY: number;
  elementStartX: number;
  elementStartY: number;
  elementStartWidth: number;
  elementStartHeight: number;
  type: "control" | "label" | "overlay" | null;
  mode: "move" | "resize";
  resizeHandle: "top-left" | "top-right" | "bottom-left" | "bottom-right" | null;
  id: string;
}

export interface LayoutResourceCandidate {
  controlKey: string;
  displayName: string;
  resourceType: string;
  resourceIndex: number;
  exposedResourceId?: string;
  allowBrowseFile?: boolean;
}

export interface CopiedTextLabelPayload {
  text: string;
  position: { x: number; y: number };
  fontSize: number;
  fontWeight?: "normal" | "bold";
  fontFamily?: string;
  color?: string;
  textAlign?: "left" | "center" | "right";
}
