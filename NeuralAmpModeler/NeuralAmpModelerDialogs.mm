#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "NeuralAmpModelerDialogs.h"

bool NAM_ShowOpenFileDialog(const char* title, const char* extension, WDL_String& outPath)
{
  @autoreleasepool
  {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = [NSString stringWithUTF8String:title];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;

    if (extension)
    {
      NSString* ext = [NSString stringWithUTF8String:extension];
      if (@available(macOS 11.0, *))
      {
        UTType* type = [UTType typeWithFilenameExtension:ext];
        if (type)
          panel.allowedContentTypes = @[type];
      }
      else
      {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        panel.allowedFileTypes = @[ext];
#pragma clang diagnostic pop
      }
    }

    if ([panel runModal] == NSModalResponseOK)
    {
      NSURL* url = panel.URL;
      outPath.Set([url.path UTF8String]);
      return true;
    }
    return false;
  }
}

bool NAM_ShowOpenDirectoryDialog(const char* title, WDL_String& outPath)
{
  @autoreleasepool
  {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = [NSString stringWithUTF8String:title];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;

    if ([panel runModal] == NSModalResponseOK)
    {
      NSURL* url = panel.URL;
      outPath.Set([url.path UTF8String]);
      return true;
    }
    return false;
  }
}
#endif // __APPLE__
