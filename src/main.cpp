#include <Windows.h>
#include <atomic>
#include "unity.hpp"

#define PIPE_NAME "\\\\.\\pipe\\PolytoriaPipe"

std::atomic<bool> g_Running = true;
HANDLE g_ShutdownEvent = NULL;

using U = UnityResolve;
using UT = U::UnityType;
using UC = U::UnityType::Component;


void RunLuaScript(const std::string& script)
{
    const auto pAssembly = U::Get("Assembly-CSharp.dll");
    if (!pAssembly) { return; }

    const auto pScriptService = pAssembly->Get("ScriptService");
    const auto pGame = pAssembly->Get("Game");
    const auto pScriptInstance = pAssembly->Get("ScriptInstance");
    const auto pBaseScript = pAssembly->Get("BaseScript");

    if (pScriptService && pGame && pScriptInstance && pBaseScript) {
        const auto pGetScriptServiceInstanceField = pScriptService->Get<U::Field>("<Instance>k__BackingField");
        const auto pGameInstanceField = pGame->Get<U::Field>("singleton");

        UC* gameInstance = nullptr;
        if (pGameInstanceField) {
            pGameInstanceField->GetStaticValue<UC*>(&gameInstance);
        } else {
            return;
        }

        UC* scriptServiceInstance = nullptr;
        if (pGetScriptServiceInstanceField) {
            pGetScriptServiceInstanceField->GetStaticValue<UC*>(&scriptServiceInstance);
        } else {
            return;
        }

        if (scriptServiceInstance && gameInstance) {
            const auto pGameObject = gameInstance->GetGameObject();
            if (pGameObject) {
                UC* scriptInstance = pGameObject->AddComponent<UC*>(pScriptInstance);
                if (scriptInstance) {
                    const auto Script = UT::String::New(script);
                    if (Script) {
                        // Recursive lambda to set field value in script instance or its base script
                        // This is necessary because of wierd il2cpp inheritance
                        auto setFieldRecursive = [&](const std::string& name, auto value) -> bool {
                            if (pScriptInstance->Get<U::Field>(name)) {
                                pScriptInstance->SetValue(scriptInstance, name, value);
                                return true;
                            }
                            if (pBaseScript && pBaseScript->Get<U::Field>(name)) {
                                pBaseScript->SetValue(scriptInstance, name, value);
                                return true;
                            }
                            return false;
                        };

                        // idk why Source with cap is not inherited ig , but empirical data show source works too so...
                        setFieldRecursive("source", Script);
                        setFieldRecursive("running", false);

                        const auto pRunScriptMethod = pScriptService->Get<U::Method>("RunScript");
                        if (pRunScriptMethod) {
                            const auto pRunScript = pRunScriptMethod->Cast<void, UC*, UC*>();
                            if (pRunScript) pRunScript(scriptServiceInstance, scriptInstance);
                        }
                    }
                }
            }

        }
    }
}


DWORD WINAPI MainThread(LPVOID param)
{
    // Full working executor

    HANDLE g = GetModuleHandleA("GameAssembly.dll");
    U::Init(g, U::Mode::Il2Cpp);
    U::ThreadAttach();

    char* buffer = (char*)malloc(9999999);
    if (!buffer) return 1;

    while (g_Running)
    {
        HANDLE hPipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            9999999,
            9999999,
            0,
            NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            if (!g_Running) break;
            Sleep(100);
            continue;
        }

        OVERLAPPED overlapped = { 0 };
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!overlapped.hEvent)
        {
            CloseHandle(hPipe);
            break;
        }

        BOOL connected = ConnectNamedPipe(hPipe, &overlapped);
        if (!connected)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                HANDLE waitHandles[2] = { overlapped.hEvent, g_ShutdownEvent };
                DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0)
                {
                    connected = TRUE;
                }
                else
                {
                    CancelIo(hPipe);
                    connected = FALSE;
                }
            }
            else if (err == ERROR_PIPE_CONNECTED)
            {
                connected = TRUE;
            }
        }

        if (connected && g_Running)
        {
            DWORD bytesRead;
            ResetEvent(overlapped.hEvent);
            if (ReadFile(hPipe, buffer, 9999998, &bytesRead, &overlapped) || GetLastError() == ERROR_IO_PENDING)
            {
                HANDLE waitHandles[2] = { overlapped.hEvent, g_ShutdownEvent };
                DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0)
                {
                    if (GetOverlappedResult(hPipe, &overlapped, &bytesRead, FALSE))
                    {
                        buffer[bytesRead] = '\0';
                        RunLuaScript(std::string(buffer));
                    }
                }
                else
                {
                    CancelIo(hPipe);
                }
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        CloseHandle(overlapped.hEvent);
    }

    free(buffer);
    U::ThreadDetach();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            g_ShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
            break;
        
        case DLL_PROCESS_DETACH:
            g_Running = false;
            if (g_ShutdownEvent) {
                SetEvent(g_ShutdownEvent);
            }
            // Give the thread a moment to exit (optional but good practice)
            Sleep(100); 
            if (g_ShutdownEvent) CloseHandle(g_ShutdownEvent);
            break;
    }
    return TRUE;
}