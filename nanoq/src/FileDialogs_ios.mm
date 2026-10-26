// The iOS half of FileDialogs.cpp: a document picker for opening a file. As on macOS, the request
// hops to the main thread and `done` is called from the picker's delegate, which callers treat
// as a thread of its own.
//
// The picked file is copied into the app's own container (asCopy / Import mode), so the path
// handed back is an ordinary file the engine can read and import, with no security-scoped
// access to manage. Saving and choosing a folder are not offered on iOS yet: a picker cannot
// hand back a path to write a new file at, which is what the engine's save expects, so both
// report that nothing was chosen.

#include "FileDialogs.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <memory>
#include <vector>

using DoneCallback = std::function<void(const guitarfx::BrowseFileResult&)>;

// Holds itself while the picker is up: the picker keeps its delegate weakly.
@interface NanoQDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, strong) NanoQDocumentPickerDelegate* keepAlive;
@end

@implementation NanoQDocumentPickerDelegate
{
    std::shared_ptr<DoneCallback> mDone;
}

- (instancetype)initWithDone:(std::shared_ptr<DoneCallback>)done
{
    if ((self = [super init]))
    {
        mDone = std::move(done);
        _keepAlive = self;
    }
    return self;
}

- (void)finish:(NSURL*)url
{
    guitarfx::BrowseFileResult result;
    if (url != nil && url.fileURL)
    {
        result.success = true;
        result.path = std::filesystem::path(url.fileSystemRepresentation);
    }
    if (mDone)
        (*mDone)(result);
    mDone.reset();
    _keepAlive = nil;
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
    [self finish:urls.firstObject];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
    [self finish:nil];
}
@end

namespace nanoq
{
namespace
{
// The same file types as the other platforms' dialogs.
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
    default:
        return {};
    }
}

UIViewController* TopViewController()
{
    UIWindow* window = nil;
    if (@available(iOS 13.0, *))
    {
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        {
            if (![scene isKindOfClass:[UIWindowScene class]])
                continue;
            for (UIWindow* w in ((UIWindowScene*)scene).windows)
                if (w.isKeyWindow)
                    window = w;
        }
    }
    if (window == nil)
        window = UIApplication.sharedApplication.delegate.window;

    UIViewController* top = window.rootViewController;
    while (top.presentedViewController != nil)
        top = top.presentedViewController;
    return top;
}

UIDocumentPickerViewController* MakeOpenPicker(guitarfx::BrowseFileType type)
{
    if (@available(iOS 14.0, *))
    {
        NSMutableArray<UTType*>* types = [NSMutableArray array];
        for (NSString* extension : ExtensionsFor(type))
            if (UTType* uttype = [UTType typeWithFilenameExtension:extension])
                [types addObject:uttype];
        // A .nam has no registered type, and an extension's dynamic type matches only files the
        // system tags with it, so anything is offered as well and the engine checks the file.
        [types addObject:UTTypeItem];
        return [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
    }
    return [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.item"]
                                                                  inMode:UIDocumentPickerModeImport];
}
} // namespace

void RunAppleDialog(bool save, guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                    DoneCallback done)
{
    (void)title;
    (void)defaultName;
    if (save || type == guitarfx::BrowseFileType::Folder)
    {
        done({});
        return;
    }

    auto callback = std::make_shared<DoneCallback>(std::move(done));
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController* presenter = TopViewController();
        if (presenter == nil)
        {
            (*callback)({});
            return;
        }
        UIDocumentPickerViewController* picker = MakeOpenPicker(type);
        auto* delegate = [[NanoQDocumentPickerDelegate alloc] initWithDone:callback];
        picker.delegate = delegate;
        picker.allowsMultipleSelection = NO;
        [presenter presentViewController:picker animated:YES completion:nil];
    });
}
} // namespace nanoq
