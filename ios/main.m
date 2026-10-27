// The iOS app's entry point. The app holds both of the game's executables,
// each recompiled with its own copy of the runtime into a framework of its
// own (ios/build.sh): MW2SP, the campaign and special ops, and MW2MP, the
// multiplayer. Only one can be in a process, so this loads the one the
// player's copy was left in -- Documents/title, written when the game asks
// for its other executable (runtime/install/install.cpp) -- and hands the
// process to it (mw2_ios_main, runtime/main.cpp).
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*MainFunction)(int argc, char* argv[]);

static NSString* Documents(NSString* name) {
  NSString* folder =
      NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  return [folder stringByAppendingPathComponent:name];
}

static MainFunction Load(NSString* framework) {
  NSString* path = [[[NSBundle mainBundle] privateFrameworksPath]
      stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.framework/%@", framework, framework]];
  void* handle = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "cannot load %s: %s\n", path.UTF8String, dlerror());
    return NULL;
  }
  return (MainFunction)dlsym(handle, "mw2_ios_main");
}

int main(int argc, char* argv[]) {
  @autoreleasepool {
    NSString* title = [[NSString stringWithContentsOfFile:Documents(@"title")
                                                 encoding:NSUTF8StringEncoding
                                                    error:NULL]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    const BOOL multiplayer = [title isEqualToString:@"mp"];
    // What the last title left for this one (XamLoaderSetLaunchData), once.
    NSString* data = [NSString stringWithContentsOfFile:Documents(@"launch_data")
                                               encoding:NSUTF8StringEncoding
                                                  error:NULL];
    if (data.length) setenv("MW2_LAUNCH_DATA", data.UTF8String, 1);
    [[NSFileManager defaultManager] removeItemAtPath:Documents(@"launch_data") error:NULL];

    MainFunction start = Load(multiplayer ? @"MW2MP" : @"MW2SP");
    if (!start) start = Load(multiplayer ? @"MW2SP" : @"MW2MP");
    if (!start) return 1;
    return start(argc, argv);
  }
}
