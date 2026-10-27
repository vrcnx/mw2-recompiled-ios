// The iOS app's entry point and its launcher. The app holds both of the
// game's executables, each recompiled with its own copy of the runtime into a
// framework of its own (ios/build.sh): MW2SP, the campaign and special ops,
// and MW2MP, the multiplayer. Only one can be in a process, so the player
// chooses here, before either is loaded, and the chosen one is handed the
// process (mw2_ios_run, runtime/main.cpp).
//
// UIKit's launch goes through SDL, which is a framework of its own the titles
// link against too (SDL3.framework): the launcher's window is SDL's, with
// UIKit views over it, and the title goes on with the same SDL once the
// launcher has let go of it.
#import <UIKit/UIKit.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*RunFunction)(int argc, char* argv[]);

enum { kCampaign, kMultiplayer, kModes };

static NSString* Documents(NSString* name) {
  NSString* folder =
      NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  return [folder stringByAppendingPathComponent:name];
}

static UIColor* Accent(CGFloat alpha) {
  return [UIColor colorWithRed:0.62 green:0.80 blue:0.30 alpha:alpha];
}

static NSAttributedString* Spaced(NSString* text, UIFont* font, UIColor* colour, CGFloat kern) {
  return [[NSAttributedString alloc] initWithString:text
                                         attributes:@{NSFontAttributeName : font,
                                                      NSForegroundColorAttributeName : colour,
                                                      NSKernAttributeName : @(kern)}];
}

// ---- the launcher's views ---------------------------------------------------

@interface MW2ModeCard : UIControl
@property(nonatomic, readonly) UILabel* titleLabel;
@property(nonatomic, readonly) UILabel* detailLabel;
- (void)setChosen:(BOOL)chosen animated:(BOOL)animated;
@end

@implementation MW2ModeCard
- (instancetype)initWithTitle:(NSString*)title detail:(NSString*)detail {
  if ((self = [super initWithFrame:CGRectZero])) {
    self.layer.cornerRadius = 18;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    self.layer.borderWidth = 2;
    _titleLabel = [[UILabel alloc] init];
    _titleLabel.attributedText = Spaced(title, [UIFont systemFontOfSize:26 weight:UIFontWeightHeavy],
                                        UIColor.whiteColor, 2.5);
    _detailLabel = [[UILabel alloc] init];
    _detailLabel.attributedText = Spaced(detail, [UIFont systemFontOfSize:16 weight:UIFontWeightMedium],
                                         [UIColor colorWithWhite:1 alpha:0.6], 0.3);
    [self addSubview:_titleLabel];
    [self addSubview:_detailLabel];
    self.isAccessibilityElement = YES;
    self.accessibilityLabel = title;
    self.accessibilityTraits = UIAccessibilityTraitButton;
    [self setChosen:NO animated:NO];
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat inset = 28, width = self.bounds.size.width - inset * 2;
  [self.titleLabel sizeToFit];
  [self.detailLabel sizeToFit];
  const CGFloat titleHeight = self.titleLabel.bounds.size.height;
  const CGFloat detailHeight = self.detailLabel.bounds.size.height;
  const CGFloat top = (self.bounds.size.height - titleHeight - 6 - detailHeight) / 2;
  self.titleLabel.frame = CGRectMake(inset, top, width, titleHeight);
  self.detailLabel.frame = CGRectMake(inset, top + titleHeight + 6, width, detailHeight);
}

- (void)setChosen:(BOOL)chosen animated:(BOOL)animated {
  void (^change)(void) = ^{
    self.backgroundColor = chosen ? Accent(0.20) : [UIColor colorWithWhite:1 alpha:0.06];
    self.layer.borderColor = (chosen ? Accent(0.95) : [UIColor colorWithWhite:1 alpha:0.12]).CGColor;
    self.transform = chosen ? CGAffineTransformMakeScale(1.03, 1.03) : CGAffineTransformIdentity;
    self.detailLabel.alpha = chosen ? 1.0 : 0.75;
  };
  if (animated)
    [UIView animateWithDuration:0.18 delay:0 options:UIViewAnimationOptionBeginFromCurrentState
                     animations:change completion:nil];
  else
    change();
}
@end

@interface MW2LauncherView : UIView
@property(nonatomic) int chosen;
@property(nonatomic) BOOL started;
- (instancetype)initWithChoice:(int)choice;
- (void)move:(int)by;
@end

@implementation MW2LauncherView {
  UIImageView* _backdrop;
  UIVisualEffectView* _blur;
  CAGradientLayer* _shade;
  UIImageView* _art;
  UILabel* _title;
  UILabel* _subtitle;
  UILabel* _hint;
  NSArray<MW2ModeCard*>* _cards;
}

- (instancetype)initWithChoice:(int)choice {
  if ((self = [super initWithFrame:CGRectZero])) {
    self.backgroundColor = [UIColor colorWithRed:0.078 green:0.086 blue:0.071 alpha:1];
    UIImage* art = [UIImage imageNamed:@"LauncherArt"];

    _backdrop = [[UIImageView alloc] initWithImage:art];
    _backdrop.contentMode = UIViewContentModeScaleAspectFill;
    _backdrop.clipsToBounds = YES;
    _backdrop.alpha = 0.55;
    [self addSubview:_backdrop];
    _blur = [[UIVisualEffectView alloc]
        initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
    [self addSubview:_blur];
    _shade = [CAGradientLayer layer];
    _shade.startPoint = CGPointMake(0, 0.5);
    _shade.endPoint = CGPointMake(1, 0.5);
    _shade.colors = @[ (id)[UIColor colorWithWhite:0 alpha:0.25].CGColor,
                       (id)[UIColor colorWithWhite:0 alpha:0.75].CGColor ];
    [self.layer addSublayer:_shade];

    _art = [[UIImageView alloc] initWithImage:art];
    _art.contentMode = UIViewContentModeScaleAspectFill;
    _art.clipsToBounds = YES;
    _art.layer.cornerRadius = 36;
    _art.layer.cornerCurve = kCACornerCurveContinuous;
    _art.layer.borderWidth = 1;
    _art.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.15].CGColor;
    [self addSubview:_art];

    _title = [[UILabel alloc] init];
    _title.attributedText = Spaced(@"MODERN WARFARE 2", [UIFont systemFontOfSize:46 weight:UIFontWeightBlack],
                                   UIColor.whiteColor, 4);
    _title.adjustsFontSizeToFitWidth = YES;
    _title.minimumScaleFactor = 0.6;
    [self addSubview:_title];
    _subtitle = [[UILabel alloc] init];
    _subtitle.attributedText = Spaced(@"CHOOSE A MODE", [UIFont systemFontOfSize:15 weight:UIFontWeightBold],
                                      Accent(1), 3);
    [self addSubview:_subtitle];

    MW2ModeCard* campaign = [[MW2ModeCard alloc] initWithTitle:@"CAMPAIGN & SPECIAL OPS"
                                                        detail:@"The story missions and the Spec Ops challenges"];
    MW2ModeCard* multiplayer = [[MW2ModeCard alloc] initWithTitle:@"MULTIPLAYER"
                                                           detail:@"System Link, split screen and bots"];
    _cards = @[ campaign, multiplayer ];
    for (MW2ModeCard* card in _cards) {
      [card addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventTouchUpInside];
      [self addSubview:card];
    }

    _hint = [[UILabel alloc] init];
    _hint.attributedText = Spaced(@"Ⓐ  Start        ✛  Choose        or tap a mode",
                                  [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold],
                                  [UIColor colorWithWhite:1 alpha:0.5], 0.5);
    [self addSubview:_hint];

    _chosen = choice;
    [_cards[choice] setChosen:YES animated:NO];
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGRect bounds = self.bounds;
  const CGFloat width = bounds.size.width, height = bounds.size.height;
  _backdrop.frame = bounds;
  _blur.frame = bounds;
  _shade.frame = bounds;

  // The art on the left, the title and the modes on the right, the two
  // centred together.
  const CGFloat artSide = MIN(height * 0.52, width * 0.36);
  const CGFloat gap = width * 0.06;
  const CGFloat column = MIN(width * 0.46, 620);
  const CGFloat left = (width - artSide - gap - column) / 2;
  _art.frame = CGRectMake(left, (height - artSide) / 2, artSide, artSide);

  const CGFloat x = left + artSide + gap;
  const CGFloat cardHeight = 104, cardGap = 18;
  [_title sizeToFit];
  [_subtitle sizeToFit];
  const CGFloat titleHeight = _title.bounds.size.height, subtitleHeight = _subtitle.bounds.size.height;
  const CGFloat block = subtitleHeight + 8 + titleHeight + 36 + cardHeight * kModes + cardGap * (kModes - 1);
  CGFloat y = (height - block) / 2;
  _subtitle.frame = CGRectMake(x, y, column, subtitleHeight);
  y += subtitleHeight + 8;
  _title.frame = CGRectMake(x, y, column, titleHeight);
  y += titleHeight + 36;
  for (MW2ModeCard* card in _cards) {
    const CGAffineTransform transform = card.transform;
    card.transform = CGAffineTransformIdentity;
    card.frame = CGRectMake(x, y, column, cardHeight);
    card.transform = transform;
    y += cardHeight + cardGap;
  }
  [_hint sizeToFit];
  _hint.frame = CGRectMake((width - _hint.bounds.size.width) / 2, height - 64,
                           _hint.bounds.size.width, _hint.bounds.size.height);
}

- (void)move:(int)by {
  const int next = MAX(0, MIN(kModes - 1, self.chosen + by));
  if (next == self.chosen) return;
  [_cards[self.chosen] setChosen:NO animated:YES];
  self.chosen = next;
  [_cards[next] setChosen:YES animated:YES];
  [[UISelectionFeedbackGenerator new] selectionChanged];
}

- (void)tapped:(MW2ModeCard*)card {
  [self move:(int)[_cards indexOfObject:card] - self.chosen];
  self.started = YES;
}
@end

// ---- the launcher -----------------------------------------------------------

static int g_argc;
static char** g_argv;

static RunFunction Load(NSString* framework) {
  NSString* path = [[[NSBundle mainBundle] privateFrameworksPath]
      stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.framework/%@", framework, framework]];
  // Global: SDL finds MoltenVK's vkGetInstanceProcAddr among the process's
  // symbols.
  void* handle = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    fprintf(stderr, "cannot load %s: %s\n", path.UTF8String, dlerror());
    return NULL;
  }
  return (RunFunction)dlsym(handle, "mw2_ios_run");
}

// The mode to show chosen: the one the game asked for before the app closed
// (Documents/title, written by runtime/install/install.cpp), else the last
// one started here.
static int FirstChoice(void) {
  NSString* asked = [[NSString stringWithContentsOfFile:Documents(@"title")
                                               encoding:NSUTF8StringEncoding
                                                  error:NULL]
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  [[NSFileManager defaultManager] removeItemAtPath:Documents(@"title") error:NULL];
  if ([asked isEqualToString:@"mp"]) return kMultiplayer;
  if ([asked isEqualToString:@"sp"]) return kCampaign;
  return [[NSUserDefaults standardUserDefaults] integerForKey:@"MW2LastMode"] == kMultiplayer ? kMultiplayer
                                                                                               : kCampaign;
}

// Shows the launcher until a mode is started, and returns it.
static int Choose(int choice) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return choice;
  SDL_Window* window = SDL_CreateWindow("MW2", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  UIWindow* uiWindow = window ? (__bridge UIWindow*)SDL_GetPointerProperty(
                                    SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL)
                              : nil;
  UIView* root = uiWindow.rootViewController.view;
  if (!root) {
    if (window) SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    return choice;
  }
  MW2LauncherView* launcher = [[MW2LauncherView alloc] initWithChoice:choice];
  launcher.frame = root.bounds;
  launcher.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [root addSubview:launcher];

  // The controller: the d-pad or the left stick chooses, A or Start starts.
  // SDL's waits run the main run loop, so taps and animations go on too.
  bool stickUp = false, stickDown = false;
  while (!launcher.started) {
    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, 50)) continue;
    switch (event.type) {
      case SDL_EVENT_GAMEPAD_ADDED:
        SDL_OpenGamepad(event.gdevice.which);
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) [launcher move:-1];
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) [launcher move:1];
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || event.gbutton.button == SDL_GAMEPAD_BUTTON_START)
          launcher.started = YES;
        break;
      case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) break;
        // A gap between pushing and letting go, so a stick resting near the
        // line does not step twice.
        if (!stickUp && event.gaxis.value < -20000) { stickUp = true; [launcher move:-1]; }
        if (stickUp && event.gaxis.value > -12000) stickUp = false;
        if (!stickDown && event.gaxis.value > 20000) { stickDown = true; [launcher move:1]; }
        if (stickDown && event.gaxis.value < 12000) stickDown = false;
        break;
    }
  }
  choice = launcher.chosen;
  [[UIImpactFeedbackGenerator new] impactOccurred];

  // Out, then everything the launcher opened is let go of: the title starts
  // SDL's video and controllers again itself.
  __block bool faded = false;
  [UIView animateWithDuration:0.25 animations:^{ launcher.alpha = 0; } completion:^(BOOL done) { faded = true; }];
  while (!faded) SDL_PumpEvents();
  [launcher removeFromSuperview];
  int count = 0;
  SDL_JoystickID* pads = SDL_GetGamepads(&count);
  for (int i = 0; pads && i < count; i++) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(pads[i]);
    if (pad) SDL_CloseGamepad(pad);
  }
  SDL_free(pads);
  SDL_DestroyWindow(window);
  SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
  return choice;
}

static int LauncherMain(int argc, char* argv[]) {
  // MW2_MODE=sp|mp starts that one without showing the launcher, for runs
  // started from the Mac (devicectl's --environment-variables).
  const char* mode = getenv("MW2_MODE");
  int choice = FirstChoice();
  if (mode && !strcmp(mode, "mp")) choice = kMultiplayer;
  else if (mode && !strcmp(mode, "sp")) choice = kCampaign;
  else choice = Choose(choice);
  [[NSUserDefaults standardUserDefaults] setInteger:choice forKey:@"MW2LastMode"];

  // What the other title left for this one (XamLoaderSetLaunchData), once.
  NSString* data = [NSString stringWithContentsOfFile:Documents(@"launch_data")
                                             encoding:NSUTF8StringEncoding
                                                error:NULL];
  if (data.length) setenv("MW2_LAUNCH_DATA", data.UTF8String, 1);
  [[NSFileManager defaultManager] removeItemAtPath:Documents(@"launch_data") error:NULL];

  RunFunction run = Load(choice == kMultiplayer ? @"MW2MP" : @"MW2SP");
  if (!run) return 1;
  return run(g_argc, g_argv);
}

int main(int argc, char* argv[]) {
  g_argc = argc;
  g_argv = argv;
  return SDL_RunApp(argc, argv, LauncherMain, NULL);
}
