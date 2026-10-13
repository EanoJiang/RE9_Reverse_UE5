#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "LeonPreviewController.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

namespace
{
ALeonPreviewController* StageController(UWorld* World)
{
    return World ? Cast<ALeonPreviewController>(World->GetFirstPlayerController()) : nullptr;
}

FAutoConsoleCommandWithWorldAndArgs CaptureCommand(
    TEXT("LeonStage.Capture"), TEXT("Capture the current stage including Slate UI. Optional absolute PNG path."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
    {
        if (ALeonPreviewController* Controller = StageController(World))
            Controller->CapturePreviewScreenshot(Args.Num() ? Args[0] : TEXT("Leon_Stage_Final.png"));
    }));

FAutoConsoleCommandWithWorldAndArgs StateCommand(
    TEXT("LeonStage.SaveState"), TEXT("Save actual runtime state to the project Scripts directory. Optional label."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
    {
        if (ALeonPreviewController* Controller = StageController(World))
            Controller->SavePreviewState(Args.Num() ? Args[0] : TEXT("manual_ui_acceptance"));
    }));

FAutoConsoleCommandWithWorldAndArgs DelayedCaptureCommand(
    TEXT("LeonStage.CaptureAfter"), TEXT("Capture after N seconds, once the live stage has rendered. Usage: seconds absolute_png_path"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
    {
        ALeonPreviewController* Controller = StageController(World);
        if (!Controller || Args.Num() < 2) return;
        const TWeakObjectPtr<ALeonPreviewController> WeakController(Controller);
        const FString Path = Args[1];
        const float Delay = FMath::Max(.01f, FCString::Atof(*Args[0]));
        FTimerHandle Handle;
        World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakController, Path]
        {
            if (WeakController.IsValid()) WeakController->CapturePreviewScreenshot(Path);
        }), Delay, false);
    }));
}

class FLeonPreviewRuntimeModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // Optimus data interfaces resolve shader templates through virtual
        // plugin paths.  Unlike /Project, plugin paths are not mapped by the
        // engine bootstrap and must be registered by the owning module.
        if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LeonPreviewTools")))
        {
            const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
            if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/Plugin/LeonPreviewTools")))
            {
                AddShaderSourceDirectoryMapping(TEXT("/Plugin/LeonPreviewTools"), ShaderDir);
            }
        }
    }
};

IMPLEMENT_MODULE(FLeonPreviewRuntimeModule, LeonPreviewRuntime)
