// The macOS half of FileDialogs.cpp: NSOpenPanel and NSSavePanel. AppKit panels live on the
// main thread, so the request hops there and the panel runs without blocking it
// (beginWithCompletionHandler); `done` is called from the completion handler, which is as good as
// "a thread of its own" to callers: they hand the result on, and drop it if the engine has gone.

#include "FileDialogs.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <memory>
#include <vector>

namespace nanoq
{
namespace
{
// The same file types as the Windows dialog (FiltersFor in FileDialogs.cpp).
std::vector<NSString*> ExtensionsFor(guitarfx::BrowseFileType type)
{
    using T = guitarfx::BrowseFileType;
    switch (type)
    {
    case T::NAMModel:
        return {@"nam"};
    case T::IRFile:
        return {@"wav"};
    case T::PresetFile:
        return {@"json", @"soundshed"};
    case T::ImageFile:
        return {@"png", @"jpg", @"jpeg", @"webp"};
    case T::AudioFile:
        return {@"wav", @"mp3", @"flac", @"ogg", @"aif", @"aiff"};
    case T::ArchiveFile:
        return {@"zip"};
    case T::PluginFile:
        return {@"vst3", @"component"};
    default:
        return {};
    }
}

NSArray<UTType*>* ContentTypesFor(guitarfx::BrowseFileType type)
{
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (NSString* extension : ExtensionsFor(type))
        if (UTType* uttype = [UTType typeWithFilenameExtension:extension])
            [types addObject:uttype];
    return types;
}

NSString* ToNSString(const std::string& utf8)
{
    return [NSString stringWithUTF8String:utf8.c_str()] ?: @"";
}
} // namespace

void RunAppleDialog(bool save, guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                    std::function<void(const guitarfx::BrowseFileResult&)> done)
{
    // Shared, so the block below owns the callback whatever thread it is copied on.
    auto callback = std::make_shared<std::function<void(const guitarfx::BrowseFileResult&)>>(std::move(done));

    dispatch_async(dispatch_get_main_queue(), ^{
        NSSavePanel* panel = nil;
        if (save)
        {
            panel = [NSSavePanel savePanel];
            if (!defaultName.empty())
                panel.nameFieldStringValue = ToNSString(defaultName);
        }
        else
        {
            NSOpenPanel* open = [NSOpenPanel openPanel];
            const bool folder = type == guitarfx::BrowseFileType::Folder;
            open.canChooseFiles = !folder;
            open.canChooseDirectories = folder;
            open.allowsMultipleSelection = NO;
            panel = open;
        }

        if (!title.empty())
            panel.message = ToNSString(title);

        NSArray<UTType*>* types = ContentTypesFor(type);
        if (types.count > 0)
            panel.allowedContentTypes = types;

        [panel beginWithCompletionHandler:^(NSModalResponse response) {
            guitarfx::BrowseFileResult result;
            if (response == NSModalResponseOK && panel.URL.fileURL)
            {
                result.success = true;
                result.path = std::filesystem::path(panel.URL.fileSystemRepresentation);
            }
            (*callback)(result);
        }];
    });
}
} // namespace nanoq
