#include <windows.h>

// Initialization is an explicit exported call made by SpiderlessLauncher after
// the Windows loader completes and before the game's entrypoint executes.
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
