// The iOS app's entry point: everything else is in libmw2_ios.a, where
// mw2_ios_main hands the process to SDL and UIKit (runtime/main.cpp).
int mw2_ios_main(int argc, char* argv[]);

int main(int argc, char* argv[]) {
  return mw2_ios_main(argc, argv);
}
