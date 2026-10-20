/**
 * Blend definitions as the engine stores them (controller/internal/BlendSupport.cpp reads them):
 * the models a blend plays and where each sits. Re-exported from types.ts.
 */

export type BlendMode = "snap" | "interpolate";

export interface BlendModelMapping {
  id: string;
  parameterId?: string;
  parameterValue?: number;
  parameters?: Record<string, number>;
  /** Level trim for this model in dB, to even out models captured at different levels. */
  levelTrimDb?: number;
}

export interface BlendDefinition {
  id: string;
  name: string;
  category: string;
  models: string[];
  blendMode?: BlendMode;
  controlMap?: Record<string, number>;
  modelMappings?: BlendModelMapping[];
  parameters?: string[];
  toneGroupId?: string;
  toneGroupTitle?: string;
  /** Registered from a bundled factory archive: it cannot be deleted, and saving an edit
   * keeps the user's copy in its place. */
  factory?: boolean;
}

export type BlendLibrary = BlendDefinition[];
