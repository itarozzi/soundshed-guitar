#pragma once

// ScaleGuideMessages — the Scale Guide's one UI message (docs/plans/scale-guide.md).
//
// `scaleGuide` with `action: "catalog"` answers `scaleGuideCatalog`: the scales, chords,
// progressions and tunings for the guide's pickers. With `action: "view"` and a request
// (music/ScaleGuideView.h) it answers `scaleGuideView`, the model the UI draws, with the
// request's `requestId` echoed so the UI can drop an answer a later change has overtaken.
//
// The guide holds no state of its own (the UI keeps its choices in app settings), so this is
// a registration, not a service: there is nothing for the controller to own.

#include <functional>
#include <string>

namespace guitarfx
{
class MessageHandlerRegistry;

void RegisterScaleGuideMessages(MessageHandlerRegistry& registry, std::function<void(const std::string&)> sendToUI);
} // namespace guitarfx
