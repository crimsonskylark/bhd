#include <Windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <excpt.h>

#include <array>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")

namespace bhd {

    namespace Types {
        using EndScene_t = HRESULT(WINAPI*)(LPDIRECT3DDEVICE9);
    }

    struct DrawTextArg {
        DrawTextArg(const float _x,
            const float _y,
            const int _alpha,
            const int _r,
            const int _g,
            const int _b)
            : x(_x), y(_y), alpha(_alpha), r(_r), g(_g), b(_b) {}

        float x{ 0 };
        float y{ 0 };
        unsigned int alpha{ 0 };
        unsigned int r{ 0 };
        unsigned int g{ 0 };
        unsigned int b{ 0 };
    };

    struct WeaponInfo {
    public:
        DWORD curr_ammo;     // 0x0000
        DWORD max_ammo;      // 0x0004
        DWORD rate_of_fire;  // 0x0008
    };  // Size=0x000C

    namespace GlobalContext {

        // D3D9 vTable pointer. 119 functions in total.
        static std::array<void*, 119> d3dDevices{ 0 };

        // Bytes to be patched after our jump hook
        static std::array<char, 7> endSceneBytes{ 0 };

        // Game window
        static HWND gWindow{};

        // Game window height and width
        static int gWinHeight{};
        static int gWinWidth{};

        // Used during hooking
        static LPDIRECT3DDEVICE9 gOldD3Ddevice{};

        // All text is white.
        static const D3DCOLOR gTextColor = D3DCOLOR_ARGB(255, 255, 255, 255);

        // Function pointers to our hooked functions
        static Types::EndScene_t EndSceneFn{ nullptr };
        static Types::EndScene_t gameEndScene{ nullptr };

        // Font to draw our text with
        static ID3DXFont* gFont{ nullptr };

        // Buffer to draw information on screen
        static char positionTextBuffer[32] = { 0 };
        static char ammoTextBuffer[32] = { 0 };

        // x, y, z of player character to be drawn.
        static std::array<float, 3> lastPlayerPosition{ 0, 0, 0 };

        // Some information regarding the currently equipped weapon.
        static WeaponInfo* currentWeaponInfo{ nullptr };

        // Padding between information blocks
        constexpr int padding = 15;

        // Where to draw text. The message itself is specified during runtime.
        static const DrawTextArg gInjectedTextArg{ 20, 20, 0, 255, 255, 255 };
        static const DrawTextArg gCoordTextArg{
            20, gInjectedTextArg.y + padding, 0, 255, 255, 255 };
        static const DrawTextArg gWeaponInfoTextArg{
            20, gCoordTextArg.y + padding, 0, 255, 255, 255 };


        // Should we draw coordinates?
        static bool gFlagWriteCoords{ false };

        // Base address of the game. Used as a base to traverse pointers.
        static const uintptr_t gModuleBase =
            reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));

    }  // namespace GlobalContext



    /*
        1. Create a new Direct3D9 interface.
        2. Get the game window's height and width.
        3. Use the interface to create a new D3D9Device. We use this to get the pointer to the original "EndScene" because all devices share the same virtual function table.
    */
    static bool APIENTRY GetD3D9Device() {
        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);

        if (!d3d) {
            std::cout << "Unable to create D3D"
                << "\n";
            return false;
        }

        IDirect3DDevice9* dummyDevice{ nullptr };

        D3DPRESENT_PARAMETERS param{};

        const auto WindowFilter = [](HWND window, LPARAM lp) -> int {
            DWORD pid{ 0 };
            GetWindowThreadProcessId(window, &pid);

            if (GetCurrentProcessId() != pid)
                return 1;

            GlobalContext::gWindow = window;

            return 0;
        };

        auto GetProcessWindow = [WindowFilter]() {
            EnumWindows(WindowFilter, 0);

            if (GlobalContext::gWindow) {
                RECT win_sz;

                GetWindowRect(GlobalContext::gWindow, &win_sz);

                GlobalContext::gWinWidth = win_sz.right - win_sz.left;
                GlobalContext::gWinHeight = win_sz.bottom - win_sz.top;

                return true;
            }

            return false;
        };

        int window = GetProcessWindow();

        if (!window) {
            std::cout << "[!] Unable to find game window"
                << "\n";
            return false;
        }

        param.Windowed = true;
        param.SwapEffect = D3DSWAPEFFECT_DISCARD;
        param.hDeviceWindow = GlobalContext::gWindow;

        const HRESULT createDummyDev = d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, param.hDeviceWindow,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &param, &dummyDevice);

        if (createDummyDev != D3D_OK) {
            std::cout << "[!] Failed creating D3D device: " << createDummyDev << "\n";
            d3d->Release();
            return false;
        }

        std::cout << "[+] Dummy device created. Copying vTable to our array for hooking." << "\n";

        std::memcpy(GlobalContext::d3dDevices.data(),
            *reinterpret_cast<void***>(dummyDevice),
            sizeof(GlobalContext::d3dDevices));

        dummyDevice->Release();
        d3d->Release();

        std::cout << "[+] Found VFT. Releasing unused device and interface."
            << "\n";

        return true;
    }

    /*
        Add the offset to the pointer, dereference for every offset.
    */
    template <typename T = float>
    static T* TraversePointerPath(uintptr_t pointer,
        std::vector<unsigned int> offsets,
        const uintptr_t base_offset) {
        if (!pointer)
            return nullptr;

        auto address = pointer + base_offset;

        for (const auto& offset : offsets) {
            address = *reinterpret_cast<uintptr_t*>(address);
            // If the game has not completely loaded yet, you may get a `0` at the address pointed to by the variable.
            // If that was  the case, just return `nullptr` and try again later.
            if (address)
                address += offset;
            else
                return nullptr;
        }

        return reinterpret_cast<T*>(address);
    }

    static std::array<float, 3> GetPlayerPosition() {
        /*
            1. *(gModuleBase + 0x009e41bc)
            2. *(result + 0x14c)
            3. *(result + 0x484)
            4. *(result + 0x4)
            5. *(result + 0x400)
            6. *(result + 0x1d8)
            7. *(result + 0x40)
            result = Vector3(x, y, z)
        */
        const auto _xaddr = TraversePointerPath<float>(
            GlobalContext::gModuleBase, { 0x14c, 0x484, 0x4, 0x400, 0x1d8, 0x40 }, 0x009E41BC);

        if (_xaddr) {
            /*
              As this information is stored in a vector, we only need to calculate the first element.
            */
            return { *_xaddr, *(_xaddr + 1), *(_xaddr + 2) };
        }

        // Game has not entirely loaded yet.
        return {};
    }

    /*
        Get some information about the currently equipped weapon.
    */
    static struct WeaponInfo* GetWeaponInfo() {
        /*
            1. *(gModuleBase + 0x009e41bc)
            2. *(result + 0x1c4)
            3. *(result + 0xf8)
            4. *(result + 0xe8c)
            result = WeaponInfo*

        */
        const auto _winfo = TraversePointerPath<WeaponInfo>(
            GlobalContext::gModuleBase, { 0x1c4, 0xf8, 0xe8c }, 0x009e41bc);

        if (_winfo)
            return _winfo;

        return nullptr;
    }

    static void WriteText(ID3DXFont* font,
        const DrawTextArg& arg,
        const char* msg) {
        RECT r{ arg.x, arg.y, 200, 200 };
        font->DrawTextA(0, msg, -1, &r, 0, GlobalContext::gTextColor);
    }

    static bool filter(unsigned int code, struct _EXCEPTION_POINTERS* ep) {
        return true;
    }


    static HRESULT __stdcall EndScene(LPDIRECT3DDEVICE9 device) {
        if (!GlobalContext::gFont) {
            D3DXCreateFont(device, 17, 0, FW_BOLD, 0, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, TEXT("Arial"),
                &GlobalContext::gFont);
        }

        WriteText(GlobalContext::gFont, GlobalContext::gInjectedTextArg, "INJECTED");

        if (GlobalContext::gFlagWriteCoords) {
            GlobalContext::lastPlayerPosition = GetPlayerPosition();
            if (!GlobalContext::lastPlayerPosition.empty()) {
                sprintf_s(GlobalContext::positionTextBuffer, "x: %.2f y: %.2f z: %.2f",
                    GlobalContext::lastPlayerPosition[0], GlobalContext::lastPlayerPosition[1],
                    GlobalContext::lastPlayerPosition[2]);
                WriteText(GlobalContext::gFont, GlobalContext::gCoordTextArg, GlobalContext::positionTextBuffer);
            }

            GlobalContext::currentWeaponInfo = GetWeaponInfo();

            if (GlobalContext::currentWeaponInfo) {
                sprintf_s(GlobalContext::ammoTextBuffer, "AMMO: %d\nMAX: %d\n",
                    GlobalContext::currentWeaponInfo->curr_ammo, GlobalContext::currentWeaponInfo->max_ammo);
                WriteText(GlobalContext::gFont, GlobalContext::gWeaponInfoTextArg, GlobalContext::ammoTextBuffer);
            }
        }

        return GlobalContext::EndSceneFn(device);
    }

    static bool Detour(char* src, const char* dst, size_t sz) {
        if (sz < 5) {
            std::cout << "[!] Need at least 5 bytes for relative jump."
                << "\n";
            return false;
        }

        unsigned long mem_protection{ 0 };

        VirtualProtect(src, sz, PAGE_EXECUTE_READWRITE, &mem_protection);

        const uintptr_t relative_addr = dst - src - 5;

        *(src) = 0xe9; // JMP

        *(reinterpret_cast<uintptr_t*>(src + 1)) = relative_addr; // JMP relative_addr

        VirtualProtect(src, sz, mem_protection, &mem_protection);

        return true;
    }

    static char* Trampoline(char* src, char* dst, size_t sz) {
        if (sz < 5)
            return nullptr;

        char* trampoline = static_cast<decltype(trampoline)>(
            VirtualAlloc(0, sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

        memcpy_s(trampoline, sz, src, sz);

        const uintptr_t trampoline_relative_addr = src - trampoline - 5;

        *(trampoline + 7) = 0xe9;

        *(reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(trampoline) + 7 +
            1)) = trampoline_relative_addr;

        Detour(src, dst, sz);

        return trampoline;
    }

    static void WINAPI EntryPoint(HMODULE hModule) {
        if (GetD3D9Device()) {
            std::cout << "[+] Got D3D9Device pointer!"
                << "\n";

            std::cout << "[vtable] EndScene is at: 0x" << GlobalContext::d3dDevices[42]
                << "\n";
            std::cout << "[backup] EndScene is at 0x" << *GlobalContext::EndSceneFn << "\n";

            GlobalContext::EndSceneFn =
                reinterpret_cast<decltype(GlobalContext::EndSceneFn)>(
                    GlobalContext::d3dDevices[42]);
            GlobalContext::EndSceneFn = reinterpret_cast<Types::EndScene_t>(
                Trampoline(reinterpret_cast<char*>(GlobalContext::d3dDevices[42]),
                    reinterpret_cast<char*>(EndScene), 5));

            std::cout << "[patch] Changing EndScene to point to our hook"
                << "\n";

            std::cout << "[vtable] EndScene is at 0x" << GlobalContext::d3dDevices[42]
                << "\n";
        }

        while (true) {

            if (GetAsyncKeyState(VK_END)) break;

            if (GetAsyncKeyState(VK_INSERT)) {
                std::cout << "[+] Toggling coordinates on screen" << "\n";
                GlobalContext::gFlagWriteCoords = !GlobalContext::gFlagWriteCoords;
            }
        }

        FreeConsole();

        GlobalContext::gFont->Release();

        FreeLibraryAndExitThread(hModule, 0);
    }

}  // namespace bhd

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        AllocConsole();

        FILE* buffer{ 0 };

        freopen_s(&buffer, "CONIN$", "r", stdin);
        freopen_s(&buffer, "CONOUT$", "w", stderr);
        freopen_s(&buffer, "CONOUT$", "w", stdout);

        CreateThread(0, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(bhd::EntryPoint),
            hModule, 0, 0);
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}