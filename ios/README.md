# iPadOS

The recompiled game as an iPad app, for the M-series iPads (Apple silicon). The
game itself is the same recompiled tree as on Linux and Windows; what differs
is the host underneath it:

- **Memory.** The guest's 4 GiB space is one reservation with the physical bank
  mapped into it twice ([runtime.md](../docs/runtime.md)). On Apple's systems
  the bank is a Mach named memory entry mapped with `vm_map`: there is no
  `memfd`, and the iOS sandbox refuses POSIX shared memory. The reservation
  needs the *extended virtual addressing* entitlement, and the game's memory
  with the renderer's the *increased memory limit* one (`MW2.entitlements`);
  both need a paid Apple developer team.
- **Threads.** The recompiled code nests deep on the host stack, and a
  secondary thread on iOS gets 512 KB of it. Every thread that runs guest code
  is a `platform::GuestThread`, with 16 MB, as the Windows build asks of its
  linker.
- **The main thread.** UIKit takes windows and events only on the main thread,
  so the guest runs on a thread of its own and the main thread runs SDL's
  events and the presenter (`vk::RunMainThread`). The app enters through
  `SDL_RunApp` (`ios/main.m` → `mw2_ios_main`).
- **Pages.** Apple silicon's pages are 16 KB, and the GPU's watch on guest
  memory protects whole ones (`gpu/memory_watch.h`). A protection fault arrives
  as `SIGBUS` there.
- **Vulkan** is MoltenVK, linked into the app (`cmake/moltenvk.cmake`).
- **No launcher.** The game is installed on the Mac by `build.sh`, and its
  folder copied into the app's Documents.

## Building

On a Mac with Apple silicon:

    brew install llvm cmake ninja xcodegen

and MoltenVK: extract `MoltenVK-all.tar` from
[MoltenVK's releases](https://github.com/KhronosGroup/MoltenVK/releases) so that
`../deps/moltenvk/MoltenVK/MoltenVK/static/MoltenVK.xcframework` exists beside
this checkout (or pass `-DMW2_MOLTENVK=<its MoltenVK folder>`).

1. Recompile the executable and extract the game, on the Mac, as on Linux
   ([building.md](../docs/building.md)). The multiplayer, with title update 6:

       TITLE=mp ISO=<the disc image> TU=<the update's package> ./build.sh

   or the disc's own version, without the update:

       VERSION=tu0 TITLE=mp ISO=<the disc image> ./build.sh

   This also builds the game for macOS, which runs as on Linux:
   `./build-mp/mw2 mw2/default_mp.pe mw2/game`.

2. Your signing settings, in `ios/local.env` (git-ignored):

       MW2_IOS_TEAM=ABCDE12345
       MW2_IOS_BUNDLE_ID=com.you.mw2

3. Build, install and start the app on a connected iPad, with the same `TITLE`
   (and `VERSION`) as step 1:

       TITLE=mp ios/build.sh device --run

   `ios/build.sh sim --run` builds for the simulator instead.

4. Copy the game in:

       TITLE=mp ios/build.sh stage

   stages `ios-game/game/` (APFS clones, no space taken): the executable the
   app was built from and every fastfile, archive and movie, about 7 GB. In
   Finder, select the iPad, open *Files*, and drag the `game` folder onto the
   app. `devicectl` gives up on files this size.

The multiplayer and the campaign are separate apps (`TITLE=mp`, `TITLE=sp`),
as they are separate executables on the disc; a title asking for the other one
from its menus says the other app has to be opened. Saves, `.env` settings and
logs live in the app's Documents too, which the Files app shows.
