#include "LeonPreviewController.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "GroomComponent.h"
#include "Animation/MeshDeformer.h"
#include "GroomVisualizationData.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HDRHelper.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/SWidget.h"
#include "Widgets/SViewport.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UnrealClient.h"
#include "LeonStageUI.h"
#include "LeonREHairGuideBufferComponent.h"
#include "LeonVariantDynamicsComponent.h"

namespace LeonPreviewRuntimePrivate
{
    static const FName HeroTag(TEXT("LeonHero"));
    static const FName GroomTag(TEXT("LeonNativeHair"));
    static constexpr float CameraTargetHeightCentimeters = 142.0f;
    static constexpr float DefaultCameraDistanceCentimeters = 300.0f;
    // The variant map lays five clothing/action stations out along X.  Its
    // runtime camera still follows the hair hero at X=-120, so the ordinary
    // 300 cm single-hero framing leaves most stations outside the viewport.
    // Keep the single-hero defaults unchanged and use a wider, centered reset
    // only for LeonVariantStage.
    static constexpr float VariantStageCameraDistanceCentimeters = 1150.0f;
    static constexpr float VariantStageCameraTargetHeightCentimeters = 100.0f;
    static constexpr float VariantStageCameraTargetOffsetXCentimeters = 180.0f;
    static const TCHAR* AnimationRoot = TEXT("/Game/Leon/Animations/FullRig");
    static const TCHAR* IdleAsset = TEXT("/Game/Leon/Animations/FullRig/ch0100_generald_385e2faa/A_ch0100_GeneralD_0000_Idle_caution_id0.A_ch0100_GeneralD_0000_Idle_caution_id0");
    static const TCHAR* WalkAsset = TEXT("/Game/Leon/Animations/FullRig/ch0100_generald_385e2faa/A_ch0100_GeneralD_2270_walk_Loop_id2270.A_ch0100_GeneralD_2270_walk_Loop_id2270");
    static const TCHAR* CombAsset = TEXT("/Game/Leon/Animations/FullRig/ch0100_emote_233d0854/A_ch0100_Emote_0710_Idle_Relax_hair_id710.A_ch0100_Emote_0710_Idle_Relax_hair_id710");

    static FString Lower(const FString& In)
    {
        FString Out = In;
        Out.ToLowerInline();
        return Out;
    }

    static FString AssetPath(const FAssetData& Asset)
    {
        return Asset.GetSoftObjectPath().ToString();
    }

    static bool IsVariantStage(const UWorld* World)
    {
        return World && World->GetMapName().Contains(TEXT("LeonVariantStage"), ESearchCase::IgnoreCase);
    }
}

ALeonPreviewController::ALeonPreviewController()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.0f;
    bAutoManageActiveCameraTarget = false;
    bShowMouseCursor = true;
    bEnableClickEvents = true;
    bEnableMouseOverEvents = true;
    VariantDynamics = CreateDefaultSubobject<ULeonVariantDynamicsComponent>(TEXT("VariantDynamics"));
}

int32 ALeonPreviewController::GetVariantClothStationCount() const
{
    return VariantDynamics ? VariantDynamics->GetActiveStationCount() : 0;
}

int32 ALeonPreviewController::GetVariantClothBoneCount() const
{
    return VariantDynamics ? VariantDynamics->GetActiveBoneCount() : 0;
}

bool ALeonPreviewController::IsVariantClothSimulationActive() const
{
    return VariantDynamics && VariantDynamics->IsSimulationActive();
}

void ALeonPreviewController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // HairStrands computes local-bone velocity in Niagara from the transform
    // difference between its current and previous sample.  A looping source
    // animation can legitimately wrap from its last pose to its first pose;
    // clearing the solver at that point only hides the discontinuity and can
    // still produce a visible snap.  Instead, arm the velocity limiter before
    // the next Groom tick and let the pose transition through with a bounded
    // impulse.
    const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 1.0e-4f, 0.05f);
    if (UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr)
    {
        const float CurrentTime = Instance->GetCurrentTime();
        const float AnimationLength = Instance->GetLength();
        const bool bAnimationPlaying = Instance->IsPlaying() && Instance->GetAnimationAsset() != nullptr;
        const bool bWrapped = PreviousAnimationTime >= 0.0f && bAnimationPlaying &&
            CurrentTime + 0.05f < PreviousAnimationTime;
        // Gate the last frame before a loop as well as the wrapped frame. This
        // protects both possible component tick orders (Groom before or after
        // the player controller) from seeing the end-to-start pose delta.
        const bool bApproachingLoop = bLooping && bAnimationPlaying && AnimationLength > 0.0f &&
            PreviousAnimationTime >= 0.0f && PreviousAnimationTime < AnimationLength - 0.05f &&
            CurrentTime >= PreviousAnimationTime &&
            CurrentTime >= AnimationLength - 0.05f;
        if (bWrapped || bApproachingLoop)
        {
            // This is deliberately a scale change rather than ResetSimulation:
            // the latter discards the strand's useful state and does not make
            // a game animation loop continuous.  The next Head_0 sample will
            // refine the exact scale from its measured angular/linear delta.
            HairAngularVelocityScale = FMath::Min(HairAngularVelocityScale, 0.006f);
            HairLinearVelocityScale = FMath::Min(HairLinearVelocityScale, 0.004f);
        }
        PreviousAnimationTime = bAnimationPlaying ? CurrentTime : -1.0f;
    }

    // Time-based detection covers ordinary looping, while this world-space
    // Head_0 check also catches non-looping scrubs, animation switches, actor
    // turntable motion, and an editor hitch that skips over the time test.
    if (USkeletalMeshComponent* Body = HeroComponent.Get())
    {
        const int32 HeadIndex = Body->GetBoneIndex(FName(TEXT("Head_0")));
        if (HeadIndex != INDEX_NONE)
        {
            // GetBoneTransform with the default LocalToWorld returns world
            // space.  Use that same space as GroomComponent::BuildSimulation-
            // Transform so the limiter covers both animation motion and the
            // preview actor's turntable rotation.
            const FTransform HeadWorldTransform = Body->GetBoneTransform(HeadIndex);
            if (bHasPreviousHeadComponentTransform)
            {
                UpdateHairVelocityLimiter(SafeDeltaSeconds, HeadWorldTransform);
            }
            else
            {
                // The first frame after BeginPlay has no delta to measure.
                // Keep the explicit baseline until the next sample instead of
                // allowing a stale low value from a previous clip to persist.
                // Keep enough of the first valid bone sample to seed the
                // strand solver.  The old 0.12/0.08 seed was effectively a
                // rigid attachment on an idle-to-walk transition; RE's
                // chain2 solver carries a larger inertial impulse before its
                // drag stage settles it.
                HairAngularVelocityScale = FMath::Max(HairAngularVelocityScale, 0.22f);
                HairLinearVelocityScale = FMath::Max(HairLinearVelocityScale, 0.16f);
            }
            PreviousHeadComponentTransform = HeadWorldTransform;
            bHasPreviousHeadComponentTransform = true;
            if (!bREHairPipelineReady)
                InitializeREHairPipeline(HeadWorldTransform);
            TickREHairPipeline(SafeDeltaSeconds, HeadWorldTransform);
        }
    }

    // Groom local simulation derives angular velocity from this actor's
    // transform delta / DeltaSeconds.  A capped step avoids a frame hitch or
    // a fast mouse event producing an impulse large enough to destabilize the
    // Niagara strand solver.  Keep the cap in degrees per frame as well as
    // per second so editor stalls cannot inject a large one-frame jump.
    const float MaxDegreesPerSecond = 90.0f;
    const float MaxStep = FMath::Min(SafeDeltaSeconds * MaxDegreesPerSecond, 2.0f);
    const float PreviousYaw = ModelYaw;
    const float NewYaw = FMath::FixedTurn(ModelYaw, TargetModelYaw, MaxStep);
    const float NewPitch = FMath::FixedTurn(ModelPitch, TargetModelPitch, MaxStep);
    if (!FMath::IsNearlyEqual(NewYaw, ModelYaw, 0.001f) ||
        !FMath::IsNearlyEqual(NewPitch, ModelPitch, 0.001f))
    {
        ModelYaw = NewYaw;
        ModelPitch = NewPitch;
        if (AActor* Hero = HeroActor.Get())
        {
            FRotator Rotation = InitialHeroRotation;
            Rotation.Yaw += ModelYaw;
            Rotation.Pitch += ModelPitch;
            Hero->SetActorRotation(Rotation);
        }

        if (!FMath::IsNearlyEqual(PreviousYaw, ModelYaw, 0.001f))
        {
            RotationStillTime = 0.0f;
            bRotationRecentlyActive = true;
            bHairResetAfterRotation = false;
        }
    }

    // Keep the turntable's follow-through visible.  Numerical energy is
    // bounded by the same velocity limiter used for animation motion, so a
    // delayed ResetSimulation is neither necessary nor desirable here.
    if (bRotationRecentlyActive &&
        FMath::IsNearlyEqual(ModelYaw, TargetModelYaw, 0.001f) &&
        FMath::IsNearlyEqual(ModelPitch, TargetModelPitch, 0.001f))
    {
        RotationStillTime += SafeDeltaSeconds;
    }
}

void ALeonPreviewController::BeginPlay()
{
    Super::BeginPlay();
    DiscoverPreviewActors();
    if (UGroomComponent* Groom = GroomComponent.Get())
    {
        // Attach the saved Optimus deformer so GroomManager enables its guide
        // DeformerBuffer copy stage. The graph is generated with curve-local
        // Catmull-Rom sampling (PointCurveIndex/CurveOffsetPoint/
        // CurveNumPoint), so its full GuidesPoints domain no longer clamps
        // every strand to the fifth compact control. The render-thread writer
        // below then supplies the higher-fidelity root/tangent frame and
        // per-follicle flutter into the same buffer.
        static const TCHAR* REHairDeformerPath = TEXT("/Game/Leon/Groom/DF_Leon_REHair.DF_Leon_REHair");
        if (UMeshDeformer* REHairDeformer = Cast<UMeshDeformer>(
            StaticLoadObject(UMeshDeformer::StaticClass(), nullptr, REHairDeformerPath)))
        {
            Groom->SetMeshDeformer(REHairDeformer);
            Groom->MarkRenderStateDirty();
        }
    }
    SpawnPreviewCamera();
    HideHairCardSections();
    // Runtime games can enter BeginPlay before any editor Groom view has
    // registered the visualization CVar. Use the exported initializer accessor.
    GetGroomVisualizationData();
    if (IConsoleVariable* RestUpdate = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HairStrands.SimulationRestUpdate")))
        RestUpdate->Set(0, ECVF_SetByCode);
    ResetGroom();
    ApplyDefaultAnimationPose();
    SetLightingPreset(TEXT("Split"));
    SetLightRotation(LightRotation);
    SetHDRPaperWhite(HDRPaperWhiteNits);
    ResetCamera();
    AddStageWidget();
    SavePreviewState(TEXT("begin_play"));
}

void ALeonPreviewController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (StageWidget.IsValid() && GEngine && GEngine->GameViewport)
    {
        GEngine->GameViewport->RemoveViewportWidgetContent(StageWidget.ToSharedRef());
        StageWidget.Reset();
    }
    Super::EndPlay(EndPlayReason);
}

AActor* ALeonPreviewController::FindActorByTagOrName(const FName Tag, const FString& NameFragment) const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor->ActorHasTag(Tag))
        {
            return Actor;
        }
    }
    const FString Needle = LeonPreviewRuntimePrivate::Lower(NameFragment);
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        bool bMatches = Actor && LeonPreviewRuntimePrivate::Lower(Actor->GetName()).Contains(Needle);
#if WITH_EDITOR
        bMatches |= Actor && LeonPreviewRuntimePrivate::Lower(Actor->GetActorLabel()).Contains(Needle);
#endif
        if (bMatches)
        {
            return Actor;
        }
    }
    return nullptr;
}

void ALeonPreviewController::DiscoverPreviewActors()
{
    HeroActor = FindActorByTagOrName(LeonPreviewRuntimePrivate::HeroTag, TEXT("Leon_Assembled_Hero"));
    if (HeroActor.IsValid())
    {
        InitialHeroRotation = HeroActor->GetActorRotation();
        HeroComponent = HeroActor->FindComponentByClass<USkeletalMeshComponent>();
    }
    GroomActor = FindActorByTagOrName(LeonPreviewRuntimePrivate::GroomTag, TEXT("GroomActor"));
    if (!GroomActor.IsValid() && HeroActor.IsValid())
    {
        for (UActorComponent* Component : HeroActor->GetComponents())
        {
            if (UGroomComponent* Groom = Cast<UGroomComponent>(Component))
            {
                GroomComponent = Groom;
                GroomActor = HeroActor;
                break;
            }
        }
    }
    if (GroomActor.IsValid() && !GroomComponent.IsValid())
    {
        GroomComponent = GroomActor->FindComponentByClass<UGroomComponent>();
    }
    if (HeroComponent.IsValid() && GroomComponent.IsValid())
    {
        GroomComponent->AttachToComponent(HeroComponent.Get(), FAttachmentTransformRules::KeepRelativeTransform);
        GroomComponent->SetRelativeTransform(FTransform::Identity);
        GroomComponent->SetUseCards(false);

        // The limiter samples the post-animation Head_0 pose and writes the
        // Groom velocity scale for the same frame.  Make the dependency
        // explicit: skeletal evaluation -> controller limiter -> Groom/
        // Niagara tick.  Without this chain an engine tick-order change can
        // make the scale arrive one frame late, which is enough to reintroduce
        // a loop-boundary spike.
        AddTickPrerequisiteComponent(HeroComponent.Get());
        GroomComponent->AddTickPrerequisiteActor(this);
    }
    for (const FString& Label : {FString(TEXT("Key_Split")), FString(TEXT("Fill_Softbox")), FString(TEXT("Rim_Hair"))})
    {
        if (AActor* Light = FindActorByTagOrName(FName(*FString(TEXT("LeonStage_")+Label)), Label))
            InitialLightRotations.Add(Label, Light->GetActorRotation());
    }
}

void ALeonPreviewController::SpawnPreviewCamera()
{
    if (PreviewCamera.IsValid() || !GetWorld())
    {
        return;
    }
    FActorSpawnParameters Params;
    Params.Name = TEXT("LeonPreview_RuntimeCamera");
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, Params);
    if (Camera)
    {
        PreviewCamera = Camera;
        SetViewTarget(Camera);
        Camera->GetCameraComponent()->FieldOfView = 35.0f;
        Camera->GetCameraComponent()->bConstrainAspectRatio = false;
        Camera->GetCameraComponent()->AspectRatio = 16.0f / 9.0f;
        Camera->GetCameraComponent()->bOverrideAspectRatioAxisConstraint = true;
        Camera->GetCameraComponent()->AspectRatioAxisConstraint = AspectRatio_MaintainYFOV;
    }
}

void ALeonPreviewController::AddStageWidget()
{
    if (bStageWidgetAdded || !GEngine || !GEngine->GameViewport)
    {
        return;
    }
    StageWidget = MakeLeonStageUI(this);
    if (StageWidget.IsValid())
    {
        GEngine->GameViewport->AddViewportWidgetContent(StageWidget.ToSharedRef(), 1000);
        FInputModeGameAndUI Mode;
        Mode.SetWidgetToFocus(StageWidget);
        Mode.SetHideCursorDuringCapture(false);
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        SetInputMode(Mode);
        bStageWidgetAdded = true;
    }
}

void ALeonPreviewController::HideHairCardSections()
{
    USkeletalMeshComponent* Component = HeroComponent.Get();
    if (!Component || !Component->GetSkeletalMeshAsset())
    {
        return;
    }
    // Hair card material slots are 8/9/10 in the assembled preview mesh. Apply
    // this to every available LOD and every section so switching LOD cannot
    // resurrect the cards after a Groom or animation update.
    const int32 LodCount = Component->GetSkeletalMeshAsset()->GetLODNum();
    for (int32 Lod = 0; Lod < LodCount; ++Lod)
    {
        for (const int32 MaterialSlot : {8, 9, 10})
        {
            Component->ShowMaterialSection(MaterialSlot, INDEX_NONE, false, Lod);
        }
    }
}

UAnimSequence* ALeonPreviewController::ResolveAnimation(const FString& Query) const
{
    if (Query.IsEmpty())
    {
        return nullptr;
    }
    if (Query.StartsWith(TEXT("/Game/")))
    {
        return LoadObject<UAnimSequence>(nullptr, *Query);
    }
    const FString Needle = LeonPreviewRuntimePrivate::Lower(Query);
    for (const FString& Path : GetAnimationPaths())
    {
        if (Path.Contains(Needle, ESearchCase::IgnoreCase)) return LoadObject<UAnimSequence>(nullptr, *Path);
    }
    return nullptr;
}

bool ALeonPreviewController::PlayAnimationAsset(UAnimSequence* Animation)
{
    USkeletalMeshComponent* Component = HeroComponent.Get();
    if (!Component || !Animation)
    {
        return false;
    }
    CurrentAnimation = Animation;
    Component->bPauseAnims = false;
    Component->SetForceRefPose(false);
    Component->SetComponentTickEnabled(true);
    Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    Component->PlayAnimation(Animation, bLooping);
    if (UAnimSingleNodeInstance* Instance = Component->GetSingleNodeInstance())
    {
        Instance->SetLooping(bLooping);
        Instance->SetPlayRate(PlaybackSpeed);
        Instance->SetPlaying(true);
    }
    ResetGroomForAnimationDiscontinuity();
    HideHairCardSections();
    return true;
}

bool ALeonPreviewController::SetPreviewAnimation(const FString& AnimationOrPreset)
{
    FString Query = AnimationOrPreset;
    const FString LowerQuery = LeonPreviewRuntimePrivate::Lower(Query);
    if (LowerQuery == TEXT("bind"))
    {
        const bool bResult = ApplyOriginalReferencePose(TEXT("bind"));
        SavePreviewState(TEXT("animation_bind"));
        return bResult;
    }
    if (LowerQuery == TEXT("native"))
    {
        const bool bResult = ApplyDefaultAnimationPose();
        SavePreviewState(TEXT("animation_native"));
        return bResult;
    }
    if (LowerQuery == TEXT("idle")) Query = LeonPreviewRuntimePrivate::IdleAsset;
    else if (LowerQuery == TEXT("walk")) Query = LeonPreviewRuntimePrivate::WalkAsset;
    else if (LowerQuery == TEXT("comb") || LowerQuery == TEXT("hair")) Query = LeonPreviewRuntimePrivate::CombAsset;
    UAnimSequence* Animation = ResolveAnimation(Query);
    const bool bResult = PlayAnimationAsset(Animation);
    if (bResult) AnimationSelection = (LowerQuery == TEXT("idle") || LowerQuery == TEXT("walk") || LowerQuery == TEXT("comb"))
        ? LowerQuery : Animation->GetPathName();
    SavePreviewState(TEXT("animation_")+AnimationOrPreset);
    return bResult;
}

bool ALeonPreviewController::ApplyOriginalReferencePose(const FString& Selection)
{
    USkeletalMeshComponent* Component = HeroComponent.Get();
    if (!Component || !Component->GetSkeletalMeshAsset()) return false;
    CurrentAnimation = nullptr;
    AnimationSelection = Selection;
    Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    Component->SetAnimation(nullptr);
    Component->SetForceRefPose(true);
    if (UAnimSingleNodeInstance* Instance = Component->GetSingleNodeInstance())
    {
        Instance->SetLooping(bLooping);
        Instance->SetPlayRate(PlaybackSpeed);
        Instance->SetPlaying(false);
    }
    Component->bPauseAnims = false;
    Component->TickAnimation(0.0f, false);
    Component->RefreshBoneTransforms();
    Component->bPauseAnims = true;
    Component->MarkRenderTransformDirty();
    ResetGroomForAnimationDiscontinuity();
    HideHairCardSections();
    return true;
}

bool ALeonPreviewController::ApplyDefaultAnimationPose()
{
    // The additive variant stage is an action comparison board rather than
    // the paused gallery preview. Keep its hero station on the imported axe
    // hold loop so the runtime cloth audit and the visible preview both show
    // a moving action; the original gallery map keeps the relaxed frame-0
    // pose below.
    if (GetWorld() && GetWorld()->GetMapName().Contains(TEXT("LeonVariantStage"), ESearchCase::IgnoreCase))
    {
        const FString AxeHold = TEXT("/Game/Leon/Animations/FullRig/ch0100_axe00hfps_28104e8a/A_ch0100_Axe00H_0160_Hold_Loop_id6000.A_ch0100_Axe00H_0160_Hold_Loop_id6000");
        if (PlayAnimationAsset(LoadObject<UAnimSequence>(nullptr, *AxeHold)))
        {
            AnimationSelection = TEXT("axe_hold");
            return true;
        }
    }
    // Native display uses the verified exported CombHair frame zero. It is a
    // real source animation asset and is stopped at t=0 for a relaxed gallery
    // stance. Bind remains the original mesh reference pose.
    if (!PlayAnimationAsset(ResolveAnimation(LeonPreviewRuntimePrivate::CombAsset))) return false;
    AnimationSelection = TEXT("native");
    ScrubAnimation(0.0f);
    return true;
}

void ALeonPreviewController::SetPlaying(bool bInPlaying)
{
    if (UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr)
    {
        const bool bCanPlay = bInPlaying && Instance->GetAnimationAsset();
        Instance->SetPlaying(bCanPlay);
        if (HeroComponent.IsValid()) HeroComponent->bPauseAnims = !bCanPlay;
    }
    SavePreviewState(bInPlaying ? TEXT("playing_on") : TEXT("playing_off"));
}

void ALeonPreviewController::SetLooping(bool bInLooping)
{
    bLooping = bInLooping;
    if (UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr)
    {
        Instance->SetLooping(bLooping);
    }
    SavePreviewState(TEXT("loop_changed"));
}

void ALeonPreviewController::SetPlaybackSpeed(float InSpeed)
{
    PlaybackSpeed = FMath::Clamp(InSpeed, 0.01f, 8.0f);
    if (UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr)
    {
        Instance->SetPlayRate(PlaybackSpeed);
    }
    SavePreviewState(TEXT("playback_speed_changed"));
}

void ALeonPreviewController::ScrubAnimation(float NormalizedTime)
{
    if (UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr)
    {
        const float Length = Instance->GetLength();
        Instance->SetPlaying(false);
        Instance->SetPosition(FMath::Clamp(NormalizedTime, 0.0f, 1.0f) * Length, false);
        if (HeroComponent.IsValid())
        {
            HeroComponent->bPauseAnims = false;
            HeroComponent->TickAnimation(0.0f, false);
            HeroComponent->RefreshBoneTransforms();
            HeroComponent->bPauseAnims = true;
            HeroComponent->MarkRenderTransformDirty();
        }
        ResetGroomForAnimationDiscontinuity();
    }
    SavePreviewState(TEXT("animation_scrubbed"));
}

bool ALeonPreviewController::IsPlaying() const
{
    UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr;
    return Instance && Instance->GetAnimationAsset() && Instance->IsPlaying();
}

bool ALeonPreviewController::IsLooping() const
{
    UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr;
    return Instance ? Instance->IsLooping() : bLooping;
}

FString ALeonPreviewController::GetAnimationSelection() const
{
    return AnimationSelection;
}

float ALeonPreviewController::GetAnimationTime() const
{
    UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr;
    return Instance ? Instance->GetCurrentTime() : 0.0f;
}

float ALeonPreviewController::GetAnimationLength() const
{
    UAnimSingleNodeInstance* Instance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr;
    return Instance ? Instance->GetLength() : 0.0f;
}

TArray<FString> ALeonPreviewController::GetAnimationPaths() const
{
    if (bAnimationCatalogInitialized) return AnimationPaths;
    FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    RegistryModule.Get().ScanPathsSynchronous({LeonPreviewRuntimePrivate::AnimationRoot}, false);
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(LeonPreviewRuntimePrivate::AnimationRoot));
    Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    TArray<FAssetData> Assets;
    RegistryModule.Get().GetAssets(Filter, Assets);
    AnimationPaths.Reserve(Assets.Num());
    for (const FAssetData& Asset : Assets) AnimationPaths.Add(LeonPreviewRuntimePrivate::AssetPath(Asset));
    AnimationPaths.Sort();
    bAnimationCatalogInitialized = true;
    return AnimationPaths;
}

void ALeonPreviewController::SetHairScalarOnComponent(FName Parameter, float Value) const
{
    if (UGroomComponent* Groom = GroomComponent.Get())
    {
        for (int32 Index = 0; Index < Groom->GetNumMaterials(); ++Index)
        {
            UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Groom->GetMaterial(Index));
            if (!MID) MID = Groom->CreateDynamicMaterialInstance(Index);
            if (MID) MID->SetScalarParameterValue(Parameter, Value);
        }
        Groom->MarkRenderStateDirty();
    }
}

void ALeonPreviewController::SetGroomScalar(FName Parameter, float Value)
{
    SetHairScalarOnComponent(Parameter, Value);
    SavePreviewState(TEXT("groom_scalar_")+Parameter.ToString());
}

float ALeonPreviewController::GetGroomScalar(FName Parameter) const
{
    if (const UGroomComponent* Groom = GroomComponent.Get())
    {
        for (int32 Index = 0; Index < Groom->GetNumMaterials(); ++Index)
        {
            if (const UMaterialInterface* Material = Groom->GetMaterial(Index))
            {
                float Value = 0;
                if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter)), Value)) return Value;
            }
        }
    }
    return 0.0f;
}

void ALeonPreviewController::SetGroomWidth(float WidthCentimeters)
{
    GroomWidthCentimeters = FMath::Clamp(WidthCentimeters, 0.001f, 0.2f);
    if (UGroomComponent* Groom = GroomComponent.Get())
    {
        Groom->SetHairWidth(GroomWidthCentimeters);
        Groom->MarkRenderStateDirty();
    }
    SavePreviewState(TEXT("groom_width_changed"));
}

void ALeonPreviewController::SetHairVisible(bool bVisible)
{
    bHairVisible = bVisible;
    if (UGroomComponent* Groom = GroomComponent.Get()) Groom->SetVisibility(bVisible, true);
    SavePreviewState(TEXT("hair_visibility_changed"));
}

void ALeonPreviewController::ResetGroomForAnimationDiscontinuity()
{
    // Do not reset the solver here.  ResetSimulation clears useful strand
    // state but leaves the underlying animation discontinuity untouched.  A
    // bounded velocity scale lets the solver carry the hair through a clip
    // switch/loop naturally.  Keep the previous Head_0 sample so the next
    // frame can measure the actual delta and choose a precise scale.
    HairAngularVelocityScale = FMath::Min(HairAngularVelocityScale, 0.006f);
    HairLinearVelocityScale = FMath::Min(HairLinearVelocityScale, 0.004f);
    HairLimitedAngularVelocity = FVector::ZeroVector;
    HairLimitedLinearVelocity = FVector::ZeroVector;
    PreviousAnimationTime = -1.0f;
}

void ALeonPreviewController::UpdateHairVelocityLimiter(
    float DeltaSeconds,
    const FTransform& HeadWorldTransform)
{
    UGroomComponent* Groom = GroomComponent.Get();
    if (!Groom)
    {
        return;
    }

    // HairStrands' Niagara data interface applies these scales to the raw
    // transform delta.  Keep the ordinary imported-asset values for normal
    // motion, while limiting the *resulting* velocity on a bad sample.  This
    // is independent of animation time, so it also covers scrubbing, montage
    // transitions, network corrections, and a hitch that skips a loop test.
    // HairStrands multiplies the measured Head_0 motion by these scales.  A
    // very small scale makes 20--27 cm RE9 strands follow the scalp with no
    // visible phase lag, especially during ordinary walk cycles.  The
    // velocity and acceleration limits below still guard loop/scrub spikes,
    // so the normal scale can retain the inertial follow-through expected of
    // the native chain2 path.
    constexpr float BaseLinearScale = 0.16f;
    constexpr float BaseAngularScale = 0.22f;
    // These caps only affect abnormal pose deltas.  Normal motion at the
    // baseline 0.08/0.12 scales remains unchanged; the limiter keeps a
    // one-frame spike from becoming a large velocity that would then have to
    // be removed on the following frame.
    constexpr float MaxInjectedLinearVelocity = 80.0f; // cm/s
    constexpr float MaxInjectedAngularVelocity = 1.00f; // rad/s
    constexpr float MaxInjectedLinearAcceleration = 900.0f; // cm/s^2
    constexpr float MaxInjectedAngularAcceleration = 14.0f; // rad/s^2
    constexpr float ScaleRecoverySeconds = 0.35f;

    const float SafeDelta = FMath::Max(DeltaSeconds, 1.0e-4f);
    const FTransform Delta = HeadWorldTransform * PreviousHeadComponentTransform.Inverse();
    const FVector RawLinearVelocity = Delta.GetTranslation() / SafeDelta;
    const FQuat DeltaRotation = Delta.GetRotation();
    const float RawAngularAngle = DeltaRotation.AngularDistance(FQuat::Identity);
    const FVector RawAngularVelocity = RawAngularAngle > KINDA_SMALL_NUMBER
        ? DeltaRotation.GetRotationAxis() * (RawAngularAngle / SafeDelta)
        : FVector::ZeroVector;
    const float RawLinearSpeed = RawLinearVelocity.Size();
    const float RawAngularSpeed = RawAngularVelocity.Size();

    float DesiredLinearScale = RawLinearSpeed > KINDA_SMALL_NUMBER
        ? FMath::Min(BaseLinearScale, MaxInjectedLinearVelocity / RawLinearSpeed)
        : BaseLinearScale;
    float DesiredAngularScale = RawAngularSpeed > KINDA_SMALL_NUMBER
        ? FMath::Min(BaseAngularScale, MaxInjectedAngularVelocity / RawAngularSpeed)
        : BaseAngularScale;

    // The HairStrands data interface also exposes acceleration to the solver
    // as (current velocity - previous velocity) / DeltaSeconds.  A velocity
    // cap alone would therefore still create a sharp acceleration on the
    // first frame of a bad pose.  Find the largest scalar that stays inside a
    // conservative acceleration envelope relative to the value we sent on
    // the previous sample.  The binary search is only eight iterations and
    // runs once per frame, which is negligible next to Groom/renderer work.
    const float MaxLinearDelta = MaxInjectedLinearAcceleration * SafeDelta;
    const float MaxAngularDelta = MaxInjectedAngularAcceleration * SafeDelta;
    auto LimitScaleByAcceleration = [](const FVector& RawVelocity, const FVector& PreviousVelocity,
                                       float InitialScale, float MaxDelta) -> float
    {
        if (InitialScale <= 0.0f || RawVelocity.IsNearlyZero())
        {
            return 0.0f;
        }
        const FVector Candidate = RawVelocity * InitialScale;
        if ((Candidate - PreviousVelocity).SizeSquared() <= FMath::Square(MaxDelta))
        {
            return InitialScale;
        }

        float Low = 0.0f;
        float High = InitialScale;
        for (int32 Iteration = 0; Iteration < 8; ++Iteration)
        {
            const float Mid = 0.5f * (Low + High);
            if ((RawVelocity * Mid - PreviousVelocity).SizeSquared() <= FMath::Square(MaxDelta))
            {
                Low = Mid;
            }
            else
            {
                High = Mid;
            }
        }
        return Low;
    };
    if (!RawLinearVelocity.IsNearlyZero())
    {
        DesiredLinearScale = LimitScaleByAcceleration(
            RawLinearVelocity, HairLimitedLinearVelocity, DesiredLinearScale, MaxLinearDelta);
    }
    if (!RawAngularVelocity.IsNearlyZero())
    {
        DesiredAngularScale = LimitScaleByAcceleration(
            RawAngularVelocity, HairLimitedAngularVelocity, DesiredAngularScale, MaxAngularDelta);
    }

    // Attack immediately when the source pose spikes.  Recovery is smooth so
    // that the scale itself cannot pump energy into the solver on the frame
    // after a loop; the strand state remains alive throughout.
    HairLinearVelocityScale = FMath::Min(HairLinearVelocityScale, DesiredLinearScale);
    HairAngularVelocityScale = FMath::Min(HairAngularVelocityScale, DesiredAngularScale);

    const float RecoveryAlpha = FMath::Clamp(SafeDelta / ScaleRecoverySeconds, 0.0f, 1.0f);
    HairLinearVelocityScale = FMath::Lerp(HairLinearVelocityScale, DesiredLinearScale, RecoveryAlpha);
    HairAngularVelocityScale = FMath::Lerp(HairAngularVelocityScale, DesiredAngularScale, RecoveryAlpha);

    HairLimitedLinearVelocity = RawLinearVelocity * HairLinearVelocityScale;
    HairLimitedAngularVelocity = RawAngularVelocity * HairAngularVelocityScale;

    Groom->SimulationSettings.SimulationSetup.LinearVelocityScale =
        FMath::Clamp(HairLinearVelocityScale, 0.0f, BaseLinearScale);
    Groom->SimulationSettings.SimulationSetup.AngularVelocityScale =
        FMath::Clamp(HairAngularVelocityScale, 0.0f, BaseAngularScale);
}

void ALeonPreviewController::SetHairSimulation(bool bEnabled)
{
    bHairSimulation = bEnabled;
    if (UGroomComponent* Groom = GroomComponent.Get())
    {
        // Keep the runtime override explicit. The imported asset is allowed to
        // provide solver capability, while these values suppress the unstable
        // low-damping component defaults during interactive preview.
        Groom->SimulationSettings.bOverrideSettings = true;
        // GSTR records an air-drag coefficient near 0.5.  UE expects the
        // normalized HairStrands value, so keep it at the same midpoint and
        // add the deterministic wind field in TickREHairPipeline.
        Groom->SimulationSettings.ExternalForces.AirDrag = 0.50f;
        Groom->SimulationSettings.ExternalForces.GravityVector = FVector(0.0f, 0.0f, -981.0f);
        Groom->SimulationSettings.MaterialConstraints.BendDamping = 0.24f;
        Groom->SimulationSettings.MaterialConstraints.BendStiffness = 0.030f;
        Groom->SimulationSettings.MaterialConstraints.StretchDamping = 0.18f;
        Groom->SimulationSettings.MaterialConstraints.StretchStiffness = 0.90f;
        Groom->SimulationSettings.MaterialConstraints.StaticFriction = 0.80f;
        Groom->SimulationSettings.MaterialConstraints.KineticFriction = 0.60f;
        Groom->SimulationSettings.MaterialConstraints.StrandsViscosity = 0.65f;
        Groom->SimulationSettings.MaterialConstraints.CollisionRadius = 0.010f;
        // These are velocity *scales*, not caps.  The controller's per-frame
        // limiter supplies a hard cap for imported pose spikes.  Values below
        // roughly 0.1 made the native 26 cm strands read as glued to the
        // scalp during walk/idle clips; use the chain2-compatible baseline so
        // the dense Groom retains a visible phase lag.
        HairLinearVelocityScale = 0.16f;
        HairAngularVelocityScale = 0.22f;
        HairLimitedAngularVelocity = FVector::ZeroVector;
        HairLimitedLinearVelocity = FVector::ZeroVector;
        Groom->SimulationSettings.SimulationSetup.LinearVelocityScale = HairLinearVelocityScale;
        Groom->SimulationSettings.SimulationSetup.AngularVelocityScale = HairAngularVelocityScale;
        Groom->SimulationSettings.SimulationSetup.TeleportDistance = 500.0f;
        // Do not tear down and recreate Niagara when the UI reports the same
        // state more than once. Reinitializing every mouse/UI event is a
        // common source of visible popping and solver jitter.
        // Once the reconstructed guide owner is active, a UI refresh or
        // animation reset must not re-enable Niagara behind its back.  Keep
        // the requested hair switch true for the preview, but leave native
        // simulation disabled until the custom owner is lost.
        const bool bNativeSimulationEnabled = bEnabled && !bREHairGuideSolverOwnsSimulation;
        const bool bStateChanged = (Groom->IsSimulationEnable(0, 0) != bNativeSimulationEnabled);
        Groom->SetEnableSimulation(bNativeSimulationEnabled);
        if (bStateChanged) Groom->ResetSimulation();
        ApplyREHairPipelineSettings();
    }
    SavePreviewState(TEXT("hair_simulation_changed"));
}

void ALeonPreviewController::SetHairGuides(bool bEnabled)
{
    bHairGuides = bEnabled;
    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HairStrands.ViewMode")))
    {
        CVar->Set(bEnabled ? 21 : 0, ECVF_SetByConsole);
    }
    SavePreviewState(TEXT("hair_guides_changed"));
}

void ALeonPreviewController::ResetGroom()
{
    SetHairVisible(true);
    // Groom simulation is the expected preview behavior: dragging the hero
    // should impart a bounded impulse to the strands immediately.  The
    // explicit damping/velocity limits in SetHairSimulation keep this stable
    // at rest while still leaving a visible follow-through.
    SetHairSimulation(true);
    SetHairGuides(false);
    SetGroomWidth(0.02f);
    SetGroomScalar(TEXT("DryRoughness"), 0.42f);
    SetGroomScalar(TEXT("WetRoughness"), 0.05f);
    SetGroomScalar(TEXT("WetAmount"), 0.0f);
    SetGroomScalar(TEXT("Specular"), 0.4f);
    SetGroomScalar(TEXT("Scatter"), 0.35f);
    SetGroomScalar(TEXT("Backlit"), 1.0f);
    SetGroomScalar(TEXT("AmbientOcclusion"), 0.6f);
    HideHairCardSections();
    SavePreviewState(TEXT("groom_reset"));
}

void ALeonPreviewController::SetCameraOrbit(float YawDegrees, float PitchDegrees, float DistanceCentimeters)
{
    CameraYaw = YawDegrees;
    CameraPitch = FMath::Clamp(PitchDegrees, -89.0f, 89.0f);
    CameraDistance = FMath::Max(1.0f, DistanceCentimeters);
    ApplyCameraTransform();
    SavePreviewState(TEXT("camera_orbit_changed"));
}

void ALeonPreviewController::OffsetCameraPan(float DeltaX, float DeltaY)
{
    CameraPan += FVector2D(DeltaX, DeltaY);
    ApplyCameraTransform();
    SavePreviewState(TEXT("camera_pan_changed"));
}

void ALeonPreviewController::ResetCamera()
{
    CameraYaw = 90.0f;
    CameraPitch = 0.0f;
    // MaintainYFOV already preserves vertical framing as the window changes.
    // A fixed distance matches the relaxed CombHair display pose; multiplying
    // by viewport aspect would shrink the character in wider windows.
    CameraDistance = LeonPreviewRuntimePrivate::IsVariantStage(GetWorld())
        ? LeonPreviewRuntimePrivate::VariantStageCameraDistanceCentimeters
        : LeonPreviewRuntimePrivate::DefaultCameraDistanceCentimeters;
    CameraPan = FVector2D::ZeroVector;
    ResetModelRotation();
    ApplyCameraTransform();
    SavePreviewState(TEXT("camera_reset"));
}

void ALeonPreviewController::SetModelRotation(float YawDegrees, float PitchDegrees)
{
    TargetModelYaw = FMath::Clamp(FMath::UnwindDegrees(YawDegrees), -180.0f, 180.0f);
    // The preview turntable is intentionally yaw-only.  Keep pitch at zero
    // even if an older caller still supplies a vertical drag value.
    TargetModelPitch = 0.0f;
    ModelPitch = 0.0f;
    SavePreviewState(TEXT("model_rotation_changed"));
}

void ALeonPreviewController::SetHeroRotation(float WorldYawDegrees)
{
    const float RelativeYaw = FMath::UnwindDegrees(WorldYawDegrees - InitialHeroRotation.Yaw);
    ModelYaw = TargetModelYaw = FMath::Clamp(RelativeYaw, -180.0f, 180.0f);
    RotationStillTime = 0.0f;
    bRotationRecentlyActive = true;
    bHairResetAfterRotation = false;
    if (AActor* Hero = HeroActor.Get())
    {
        FRotator Rotation = Hero->GetActorRotation();
        Rotation.Yaw = WorldYawDegrees;
        Rotation.Pitch = InitialHeroRotation.Pitch;
        Rotation.Roll = InitialHeroRotation.Roll;
        Hero->SetActorRotation(Rotation);
        ModelPitch = TargetModelPitch = 0.0f;
    }
}

void ALeonPreviewController::ResetModelRotation()
{
    ModelYaw = 0.0f;
    ModelPitch = 0.0f;
    TargetModelYaw = 0.0f;
    TargetModelPitch = 0.0f;
    RotationStillTime = 0.0f;
    bRotationRecentlyActive = false;
    bHairResetAfterRotation = false;
    if (AActor* Hero = HeroActor.Get()) Hero->SetActorRotation(InitialHeroRotation);
}

void ALeonPreviewController::ApplyCameraTransform()
{
    ACameraActor* Camera = PreviewCamera.Get();
    if (!Camera) return;
    FVector Target = HeroActor.IsValid() ? HeroActor->GetActorLocation() : FVector::ZeroVector;
    if (LeonPreviewRuntimePrivate::IsVariantStage(GetWorld()))
    {
        // The imported stations span X=-120..240.  Offset the hero target by
        // +180 cm so the five-station midpoint (X=60) is centered at reset.
        Target.X += LeonPreviewRuntimePrivate::VariantStageCameraTargetOffsetXCentimeters;
    }
    Target.Z += LeonPreviewRuntimePrivate::IsVariantStage(GetWorld())
        ? LeonPreviewRuntimePrivate::VariantStageCameraTargetHeightCentimeters
        : LeonPreviewRuntimePrivate::CameraTargetHeightCentimeters;
    Target.Z += CameraPan.Y;
    const float YawRadians = FMath::DegreesToRadians(CameraYaw);
    const float PitchRadians = FMath::DegreesToRadians(CameraPitch);
    const FVector Forward(FMath::Cos(PitchRadians) * FMath::Cos(YawRadians), FMath::Cos(PitchRadians) * FMath::Sin(YawRadians), FMath::Sin(PitchRadians));
    const FVector Right(-FMath::Sin(YawRadians), FMath::Cos(YawRadians), 0.0f);
    Target += Right * CameraPan.X;
    const FVector Location = Target + Forward * CameraDistance;
    Camera->SetActorLocation(Location);
    Camera->SetActorRotation((Target - Location).Rotation());
    Camera->GetCameraComponent()->FieldOfView = 35.0f;
}

void ALeonPreviewController::ApplyLightColorsAndIntensity(const FString& PresetName)
{
    LightingPreset = PresetName;
    const FString Key = LeonPreviewRuntimePrivate::Lower(PresetName);
    FLinearColor KeyColor = FLinearColor::White, FillColor = FLinearColor::White, RimColor = FLinearColor::White;
    float KeyPower=1.05f, FillPower=6.75f, RimPower=5.2f, SkyPower=.6f;
    if(Key==TEXT("re9"))
    {
        // The native RE9 renderer is unavailable here. Keep the named preset
        // selectable while applying a neutral three-light gallery approximation.
        KeyColor=FillColor=RimColor=FLinearColor::White;
        KeyPower=.9f; FillPower=3.6f; RimPower=5.2f; SkyPower=.6f;
    }
    else if(Key==TEXT("fireside")){KeyColor=FLinearColor(1,.35f,.07f);FillColor=FLinearColor(.18f,.28f,.55f);RimColor=FLinearColor(1,.62f,.2f);KeyPower=.9f;FillPower=1.26f;RimPower=6;SkyPower=.18f;}
    else if(Key==TEXT("moonlight")){KeyColor=FLinearColor(.32f,.52f,1);FillColor=FLinearColor(.12f,.18f,.35f);RimColor=FLinearColor(.65f,.82f,1);KeyPower=.45f;FillPower=.9f;RimPower=4.4f;SkyPower=.22f;}
    else if(Key==TEXT("tungsten")){KeyColor=FLinearColor(1,.62f,.28f);FillColor=FLinearColor(1,.76f,.48f);RimColor=FLinearColor(.65f,.78f,1);KeyPower=1.2f;FillPower=2.34f;RimPower=4.8f;SkyPower=.35f;}
    else if(Key==TEXT("portrait")){KeyColor=FLinearColor(1,.9f,.82f);FillColor=FLinearColor(.72f,.82f,1);RimColor=FLinearColor(1,.86f,.75f);KeyPower=.9f;FillPower=3.6f;RimPower=6;SkyPower=.7f;}
    else if(Key==TEXT("redlantern")){KeyColor=FLinearColor(1,.06f,.025f);FillColor=FLinearColor(.18f,.32f,.65f);RimColor=FLinearColor(1,.28f,.08f);KeyPower=.75f;FillPower=1.8f;RimPower=7.2f;SkyPower=.15f;}
    else if(Key==TEXT("texturebooth")){KeyColor=FillColor=RimColor=FLinearColor::White;KeyPower=.75f;FillPower=4.5f;RimPower=4;SkyPower=1;}
    const TArray<FString> Names{TEXT("Key_Split"), TEXT("Fill_Softbox"), TEXT("Rim_Hair")};
    const TArray<FLinearColor> Colors{KeyColor, FillColor, RimColor};
    const TArray<float> Intensities{KeyPower, FillPower, RimPower};
    for (int32 Index = 0; Index < Names.Num(); ++Index)
    {
        if (AActor* Actor = FindActorByTagOrName(FName(*FString(TEXT("LeonStage_")+Names[Index])), Names[Index]))
        {
            if (ULightComponent* Light = Actor->FindComponentByClass<ULightComponent>())
            {
                Light->SetUseTemperature(false);
                Light->SetLightColor(Colors[Index], false);
                Light->SetIntensity(Intensities[Index]);
            }
        }
    }
    if (AActor* Sky = FindActorByTagOrName(TEXT("LeonStage_Sky_Studio"), TEXT("Sky_Studio")))
        if (USkyLightComponent* Light = Sky->FindComponentByClass<USkyLightComponent>()) Light->SetIntensity(SkyPower);
}

void ALeonPreviewController::SetLightingPreset(const FString& PresetName)
{
    ApplyLightColorsAndIntensity(PresetName);
    SavePreviewState(TEXT("lighting_preset_changed"));
}

void ALeonPreviewController::SetActorLightRotation(AActor* Actor, const FRotator& Rotation) const
{
    if (Actor) Actor->SetActorRotation(Rotation);
}

void ALeonPreviewController::SetLightRotation(float InYawDegrees)
{
    LightRotation = InYawDegrees;
    for (const TPair<FString,FRotator>& Pair : InitialLightRotations)
    {
        if (AActor* Actor = FindActorByTagOrName(FName(*FString(TEXT("LeonStage_")+Pair.Key)), Pair.Key))
        {
            FRotator Rotation = Pair.Value;
            // The reference video label has its own angular origin. Preserve
            // the saved stage's usable frontal directions at displayed -131.
            Rotation.Yaw += InYawDegrees + 131.0f;
            SetActorLightRotation(Actor, Rotation);
        }
    }
    SavePreviewState(TEXT("lighting_rotation_changed"));
}

void ALeonPreviewController::SetHDRPaperWhite(float InPaperWhiteNits)
{
    HDRPaperWhiteNits = FMath::Clamp(InPaperWhiteNits, 80.0f, 1000.0f);
    if (IConsoleVariable* Mode = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HDR.PaperWhite.Mode"))) Mode->Set(1, ECVF_SetByConsole);
    if (IConsoleVariable* White = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HDR.PaperWhite"))) White->Set(HDRPaperWhiteNits, ECVF_SetByConsole);
    SavePreviewState(TEXT("hdr_paper_white_changed"));
}

void ALeonPreviewController::SetProjectionInterpolation(bool bEnabled)
{
    bProjectionInterpolation = bEnabled;
    ApplyREHairPipelineSettings();
    SavePreviewState(TEXT("re_four_point_interpolation_changed"));
}

bool ALeonPreviewController::SetBodyPhysics(bool bEnabled)
{
    // This is a local kinematic chain driven from Head_0. It deliberately
    // leaves the hero's full-body PhysicsAsset in animation mode, avoiding a
    // ragdoll while reproducing the chain2 follow-through used by RE9 hair.
    bBodyPhysicsRequested = bEnabled && HeroComponent.IsValid() && HeroComponent->GetPhysicsAsset() != nullptr;
    ApplyREHairPipelineSettings();
    if (bBodyPhysicsRequested && HeroComponent.IsValid())
    {
        const int32 HeadIndex = HeroComponent->GetBoneIndex(FName(TEXT("Head_0")));
        if (HeadIndex != INDEX_NONE)
            InitializeREHairPipeline(HeroComponent->GetBoneTransform(HeadIndex));
    }
    SavePreviewState(TEXT("re_bone_chain_physics_changed"));
    return !bEnabled || bBodyPhysicsRequested;
}

void ALeonPreviewController::InitializeREHairPipeline(const FTransform& HeadWorldTransform)
{
    REHairChain.Reset();
    // Build the compact proxy from the imported native hair-bone chain when it
    // is present.  The old fixed offsets were only a diagnostic approximation
    // (about 13 cm long) and made the XPBD stage behave like a short tuft even
    // though the STRD curves reach roughly 26 cm.  TopHairC is one of the
    // verified four-segment chains in the assembled RE skeleton; sampling its
    // current pose and converting it to Head_0 space preserves the native
    // scalp-to-tail direction for every character scale and animation pose.
    TArray<FVector> Offsets;
    Offsets.Add(FVector::ZeroVector);
    if (HeroComponent.IsValid())
    {
        const TArray<FName> NativeHairBones = {
            FName(TEXT("TopHairC_00")), FName(TEXT("TopHairC_01")),
            FName(TEXT("TopHairC_02")), FName(TEXT("TopHairC_03"))
        };
        for (const FName& BoneName : NativeHairBones)
        {
            const int32 BoneIndex = HeroComponent->GetBoneIndex(BoneName);
            if (BoneIndex == INDEX_NONE)
                break;
            const FVector LocalOffset = HeadWorldTransform.InverseTransformPosition(
                HeroComponent->GetBoneTransform(BoneIndex).GetLocation());
            if (LocalOffset.ContainsNaN() || LocalOffset.SizeSquared() < 0.25f)
                break;
            Offsets.Add(LocalOffset);
        }
    }
    // Lightweight meshes can omit the native hair bones. Keep a stable
    // fallback so the preview still has a valid five-point RE curve proxy.
    if (Offsets.Num() < 5)
    {
        Offsets = {
            FVector::ZeroVector, FVector(-4.8f, 2.7f, 14.5f),
            FVector(-8.5f, 2.2f, 10.6f), FVector(-9.3f, 1.3f, 5.2f),
            FVector(-8.1f, 1.2f, -0.5f)
        };
    }
    for (int32 Index = 0; Index < Offsets.Num(); ++Index)
    {
        LeonREHairPipeline::FChainState State;
        State.RestOffset = Offsets[Index];
        State.Position = HeadWorldTransform.TransformPosition(Offsets[Index]);
        State.Length = Index > 0 ? (Offsets[Index] - Offsets[Index - 1]).Size() : 0.0f;
        State.Stiffness = FMath::Lerp(0.96f, 0.62f, float(Index) / float(Offsets.Num() - 1));
        State.Damping = FMath::Lerp(0.90f, 0.66f, float(Index) / float(Offsets.Num() - 1));
        REHairChain.Add(State);
    }
    REHairChainRestOffsets = Offsets;
    REHairChainEnergy = 0.0f;
    REHairTransportVelocity = FVector::ZeroVector;
    REHairTipDirection = HeadWorldTransform.GetRotation().RotateVector(FVector::UpVector);
    REHairDenseAirVelocity = FVector::ZeroVector;
    bREHairDensePropagation = false;
    bREHairGuideDirectWrite = false;
    bREHairGuideSolverOwnsSimulation = false;
    bREHairPipelineReady = true;

    // The Optimus input interface discovers this staging component from the
    // Groom binding's owner.  Keep it on the hero actor so it follows the
    // same scene/component lifetime as the native groom.
    if (AActor* Hero = HeroActor.Get())
    {
        REHairGuideBuffer = Hero->FindComponentByClass<ULeonREHairGuideBufferComponent>();
        if (!REHairGuideBuffer.IsValid())
        {
            ULeonREHairGuideBufferComponent* NewBuffer = NewObject<ULeonREHairGuideBufferComponent>(Hero, TEXT("REHairGuideBuffer"));
            Hero->AddInstanceComponent(NewBuffer);
            NewBuffer->RegisterComponent();
            REHairGuideBuffer = NewBuffer;
        }
    }
}

void ALeonPreviewController::ApplyREHairPipelineSettings()
{
    if (UGroomComponent* Groom = GroomComponent.Get())
    {
        Groom->SimulationSettings.bOverrideSettings = true;
        Groom->SimulationSettings.SimulationSetup.bLocalSimulation = true;
        Groom->SimulationSettings.SimulationSetup.LocalBone = TEXT("Head_0");
        // The native path is tuned as a damped chain. Niagara remains the
        // fallback dense integrator; when the corrected Optimus/RDG guide
        // writer owns the deformed buffer, TickREHairPipeline disables
        // Niagara for that frame so the guide state has one authority.
        // The stock Groom defaults were visibly overdamped for RE9's long
        // guide chains.  Keep the RE chain2 collision/friction model, but let
        // the guides carry a little more momentum between substeps.
        // The stock values were tuned for short, high-density guides and made
        // Leon's 26 cm strands look glued to the scalp.  RE's chain2 path
        // keeps a small amount of bend compliance and lets the tail retain
        // momentum; these lower damping values preserve that follow-through
        // while the XPBD proxy still supplies the hard length/collision limits.
        Groom->SimulationSettings.ExternalForces.AirDrag = bBodyPhysicsRequested ? 0.24f : 0.28f;
        Groom->SimulationSettings.ExternalForces.GravityVector = FVector(0.0f, 0.0f, -981.0f);
        Groom->SimulationSettings.MaterialConstraints.BendDamping = bBodyPhysicsRequested ? 0.07f : 0.10f;
        Groom->SimulationSettings.MaterialConstraints.BendStiffness = bProjectionInterpolation ? 0.012f : 0.018f;
        Groom->SimulationSettings.MaterialConstraints.StretchDamping = bBodyPhysicsRequested ? 0.045f : 0.065f;
        Groom->SimulationSettings.MaterialConstraints.StretchStiffness = bProjectionInterpolation ? 0.80f : 0.84f;
        Groom->SimulationSettings.MaterialConstraints.StrandsViscosity = bProjectionInterpolation ? 0.28f : 0.32f;
    }
}

void ALeonPreviewController::TickREHairPipeline(float DeltaSeconds, const FTransform& HeadWorldTransform)
{
    if (!bREHairPipelineReady || REHairChain.Num() < 5)
        return;
    // The reconstructed local chain follows the native hair whenever Groom
    // simulation is enabled. Body Physics only changes stiffness/iterations;
    // requiring it here used to make the hair appear frozen in the normal
    // preview because the full Leon PhysicsAsset is intentionally kinematic.
    if (bHairSimulation)
    {
        const float Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
        // A low-frequency, bounded wind field gives the strand solver the same
        // continuous motion as RE's velocity-field advect stage without making
        // the preview camera or animation responsible for movement.  The old
        // The old 55/38/18 cm/s² field was only a few percent of gravity and
        // became imperceptible once the high-damping proxy settled.  RE's
        // velocity-field stage keeps a low-frequency gust around the guides;
        // use a bounded field large enough to create a soft phase lag without
        // becoming a visible external force.
        const FVector Wind(
            110.0f * FMath::Sin(Time * 0.73f),
            78.0f * FMath::Cos(Time * 0.51f),
            36.0f * FMath::Sin(Time * 0.37f + 0.9f));
        const int32 Substeps = bProjectionInterpolation ? 5 : 3;
        const int32 Iterations = bProjectionInterpolation ? 8 : 5;
        const float Compliance = bProjectionInterpolation ? 0.00008f : 0.00015f;
        LeonREHairPipeline::SimulateChain(REHairChain, HeadWorldTransform.GetLocation(),
            HeadWorldTransform.GetRotation(), DeltaSeconds, FVector(0.0f, 0.0f, -981.0f),
            // The extracted chain2 block records low material stiffness and
            // damping (0.1 / 0.05); the previous 0.76--0.90 / 0.68--0.84
            // proxy settings overdamped the five-point chain before its
            // transport velocity reached Groom.  Keep enough compliance for
            // stable XPBD projection while letting the tail retain momentum.
            bProjectionInterpolation ? 0.72f : (bBodyPhysicsRequested ? 0.62f : 0.56f),
            bProjectionInterpolation ? 0.52f : (bBodyPhysicsRequested ? 0.46f : 0.40f),
            Iterations, 7.7f, Substeps, Compliance, Wind, 0.45f);
        REHairChainEnergy = LeonREHairPipeline::ComputeChainEnergy(REHairChain);
        REHairTransportVelocity = LeonREHairPipeline::ComputeChainTransportVelocity(REHairChain);
        REHairTipDirection = LeonREHairPipeline::ComputeChainTipDirection(REHairChain);

        if (ULeonREHairGuideBufferComponent* Buffer = REHairGuideBuffer.Get())
        {
            // Groom guide deformer positions are component-local.  The
            // staging chain remains world-space for collision/velocity math,
            // then gets converted once before the render-thread upload.
            TArray<FVector> LocalPoints;
            LocalPoints.Reserve(REHairChain.Num());
            const FTransform GroomTransform = GroomComponent.IsValid()
                ? GroomComponent->GetComponentTransform() : FTransform::Identity;
            for (const LeonREHairPipeline::FChainState& State : REHairChain)
                LocalPoints.Add(GroomTransform.InverseTransformPosition(State.Position));
            Buffer->SetGuidePoints(LocalPoints);

            // Write the compact RE chain directly into Groom's guide
            // DeformerBuffer on the render thread.  The GPU pass expands the
            // five controls over every imported guide with four overlapping
            // four-control-point Catmull-Rom segments, preserving each guide's
            // authored curl/radius before GroomManager interpolates dense
            // strands.  Keep the existing Optimus staging buffer as a fallback
            // for projects where the guide resources are not yet allocated.
            TArray<FVector4f> CurrentControls;
            TArray<FVector4f> RestControls;
            CurrentControls.Reserve(5);
            RestControls.Reserve(5);
            for (int32 Index = 0; Index < 5; ++Index)
            {
                const FVector CurrentLocal = GroomTransform.InverseTransformPosition(
                    REHairChain[Index].Position);
                const FVector RestWorld = HeadWorldTransform.TransformPosition(
                    REHairChainRestOffsets.IsValidIndex(Index)
                        ? REHairChainRestOffsets[Index] : FVector::ZeroVector);
                const FVector RestLocal = GroomTransform.InverseTransformPosition(RestWorld);
                CurrentControls.Add(FVector4f(
                    static_cast<float>(CurrentLocal.X),
                    static_cast<float>(CurrentLocal.Y),
                    static_cast<float>(CurrentLocal.Z), 1.0f));
                RestControls.Add(FVector4f(
                    static_cast<float>(RestLocal.X),
                    static_cast<float>(RestLocal.Y),
                    static_cast<float>(RestLocal.Z), 1.0f));
            }
            // A live Optimus Groom deformer owns the same DeformerBuffer that
            // the render-thread fallback writes. Never enqueue both paths in
            // one frame: the last UAV writer would otherwise win
            // nondeterministically and reintroduce the old clamp-to-tip
            // deformation. The corrected Optimus graph evaluates all guide
            // points; use the compact RDG writer only when no graph is bound.
            const bool bOptimusDeformerActive = GroomComponent->MeshDeformer != nullptr;
            bREHairGuideDirectWrite = !bOptimusDeformerActive &&
                LeonREHairPipeline::EnqueueGuidePositionWrite(
                    GroomComponent.Get(), 0, CurrentControls, RestControls, Time);
        }
        else
        {
            static bool bReportedMissingGuideBuffer = false;
            if (!bReportedMissingGuideBuffer)
            {
                bReportedMissingGuideBuffer = true;
                UE_LOG(LogTemp, Warning, TEXT("[LeonREHair] guide direct write skipped: REHairGuideBuffer component is null"));
            }
        }
    }
    else
    {
        REHairChainEnergy = FMath::FInterpTo(REHairChainEnergy, 0.0f, DeltaSeconds, 8.0f);
        REHairTransportVelocity = FMath::VInterpTo(
            REHairTransportVelocity, FVector::ZeroVector, DeltaSeconds, 8.0f);
        REHairTipDirection = FMath::VInterpTo(
            REHairTipDirection, FVector::UpVector, DeltaSeconds, 8.0f).GetSafeNormal();
        REHairDenseAirVelocity = FMath::VInterpTo(
            REHairDenseAirVelocity, FVector::ZeroVector, DeltaSeconds, 8.0f);
        bREHairGuideDirectWrite = false;
        bREHairGuideSolverOwnsSimulation = false;
    }
    if (UGroomComponent* Groom = GroomComponent.Get())
    {
        // The render-thread guide writer and UE's Niagara strand simulation
        // are alternative owners of the same guide state.  Once the custom
        // pass is confirmed live, disable Niagara so its later simulation
        // pass cannot overwrite the freshly reconstructed positions.  If the
        // RHI resource disappears (load/LOD transition), restore Niagara and
        // keep the supported AirVelocity bridge as a safe fallback.
        const bool bOptimusDeformerActive = Groom->MeshDeformer != nullptr &&
            Groom->MeshDeformerInstance != nullptr;
        const bool bCustomGuideOwner = bHairSimulation &&
            (bREHairGuideDirectWrite || bOptimusDeformerActive);
        if (bCustomGuideOwner)
        {
            if (!bREHairGuideSolverOwnsSimulation)
            {
                Groom->SetEnableSimulation(false);
                bREHairGuideSolverOwnsSimulation = true;
            }
        }
        else
        {
            if (bREHairGuideSolverOwnsSimulation)
            {
                Groom->SetEnableSimulation(bHairSimulation);
                Groom->ResetSimulation();
            }
            bREHairGuideSolverOwnsSimulation = false;
        }

        // Feed the solved guide transport into Niagara's dense STRD solver.
        // UE does not expose a writable per-guide position buffer on
        // UGroomComponent; AirVelocity is the supported runtime bridge into
        // the same drag stage that advances every dense strand.  Convert the
        // world-space proxy velocity into the local Head_0 simulation frame so
        // the bridge remains correct while the actor turns.
        if (bHairSimulation)
        {
            const FVector LocalTransport =
                HeadWorldTransform.InverseTransformVectorNoScale(REHairTransportVelocity);
            FVector LocalTipDirectionVelocity = FVector::ZeroVector;
            if (REHairChainRestOffsets.Num() > 1)
            {
                const FVector RestTipDirectionWorld = HeadWorldTransform.GetRotation().RotateVector(
                    REHairChainRestOffsets.Last().GetSafeNormal());
                // The direction delta carries the solved chain's bend phase
                // into the dense drag stage even during a low-speed idle. It
                // is intentionally a velocity-sized term rather than a
                // position write: HairStrands owns the dense curve buffer,
                // while this supported input preserves the proxy's heading.
                const FVector TipDirectionVelocityWorld =
                    // Direction change is the compact representation of the
                    // four-control-point bend.  A 35 cm/s gain was too small
                    // compared with the solver's air-drag response; this
                    // gain keeps tip follow-through visible in a close-up.
                    (REHairTipDirection - RestTipDirectionWorld) * 60.0f;
                LocalTipDirectionVelocity =
                    HeadWorldTransform.InverseTransformVectorNoScale(TipDirectionVelocityWorld);
            }
            const FVector LocalWind(
                110.0f * FMath::Sin((GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f) * 0.73f),
                78.0f * FMath::Cos((GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f) * 0.51f),
                36.0f * FMath::Sin((GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f) * 0.37f + 0.9f));
            // HairStrands' local solver reads both fields in the simulation
            // frame selected by LocalBone.  Keep gravity world-down when the
            // head tilts by transforming it into that frame, then add the
            // reconstructed wind separately as AirVelocity.  Mixing a rotated
            // wind vector into GravityVector changes the acceleration
            // direction with every head rotation and is the main source of the
            // stiff, glued-looking response seen in the previous preview.
            Groom->SimulationSettings.ExternalForces.GravityVector =
                HeadWorldTransform.InverseTransformVectorNoScale(FVector(0.0f, 0.0f, -981.0f));
            // The transport term is deliberately blended rather than copied:
            // AirVelocity represents surrounding air, while the proxy's
            // velocity represents strand motion.  A 0.75 blend keeps the
            // dense curves phase-locked to the guides without making the
            // drag solver chase a high-frequency XPBD correction.
            // The render-thread guide pass already owns the compact chain bend
            // and per-guide flutter. Feeding the same proxy velocity into
            // Niagara as a second full-strength solve makes dense strands
            // overshoot and then look glued by the corrective pass. Keep only
            // a small residual gust on the direct path; use the full bridge
            // when guide resources are unavailable.
            FVector DenseAirVelocity = bREHairGuideDirectWrite
                ? LocalWind * 0.18f + LocalTransport * 0.08f + LocalTipDirectionVelocity * 0.10f
                : LocalWind + LocalTransport * 0.90f + LocalTipDirectionVelocity * 0.55f;
            const float DenseAirSpeed = DenseAirVelocity.Size();
            constexpr float MaxDenseAirSpeed = 260.0f; // cm/s
            if (DenseAirSpeed > MaxDenseAirSpeed)
                DenseAirVelocity *= MaxDenseAirSpeed / DenseAirSpeed;
            Groom->SimulationSettings.ExternalForces.AirVelocity = DenseAirVelocity;
            REHairDenseAirVelocity = DenseAirVelocity;
            // Report the bridge as active only when Groom actually owns a
            // simulated group.  This avoids a false-positive during the
            // short BeginPlay window before Niagara creates its component (or
            // when an imported Groom asset has simulation disabled).
            bREHairDensePropagation = bREHairGuideDirectWrite ||
                bREHairGuideSolverOwnsSimulation ||
                (Groom->GroomAsset && Groom->IsSimulationEnable(0, 0));
        }
        else
        {
            Groom->SimulationSettings.ExternalForces.AirVelocity = FVector::ZeroVector;
            REHairDenseAirVelocity = FVector::ZeroVector;
            bREHairDensePropagation = false;
        }
        // Chain energy drives guide motion, while the measured Head_0 velocity
        // limiter supplies actor/animation inertia. Keep a visible floor so a
        // relaxed idle still has soft, continuous sway.
        const float EnergyScale = FMath::Clamp(0.095f + REHairChainEnergy * 0.00090f, 0.045f, 0.24f);
        Groom->SimulationSettings.SimulationSetup.LinearVelocityScale =
            FMath::Max(HairLinearVelocityScale, EnergyScale);
        Groom->SimulationSettings.SimulationSetup.AngularVelocityScale =
            FMath::Max(HairAngularVelocityScale, FMath::Clamp(EnergyScale * 1.45f, 0.035f, 0.24f));
    }
}

FString ALeonPreviewController::GetPreviewStateJson() const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("animation_selection"), GetAnimationSelection());
    Root->SetStringField(TEXT("native_display_pose_source"), TEXT("comb_hair_asset_frame0"));
    Root->SetStringField(TEXT("native_display_asset"), LeonPreviewRuntimePrivate::CombAsset);
    Root->SetNumberField(TEXT("native_display_time_seconds"), 0.0f);
    Root->SetBoolField(TEXT("native_display_equals_bind_pose"), false);
    Root->SetBoolField(TEXT("native_display_original_gallery_animation_recovered"), false);
    Root->SetStringField(TEXT("native_display_pose_note"),
        TEXT("Relax hair emote frame 0, chosen as a paused preview pose; not a recovered original gallery animation."));
    Root->SetStringField(TEXT("selected_pose_source"),
        AnimationSelection == TEXT("bind") ? TEXT("original_mesh_reference_pose") :
        (AnimationSelection == TEXT("native") ? TEXT("comb_hair_source_animation") : TEXT("native_animation_asset")));
    const UAnimSingleNodeInstance* ActualInstance = HeroComponent.IsValid() ? HeroComponent->GetSingleNodeInstance() : nullptr;
    const UAnimationAsset* ActualAnimation = ActualInstance ? ActualInstance->GetAnimationAsset() : nullptr;
    Root->SetStringField(TEXT("animation_asset"), ActualAnimation ? ActualAnimation->GetPathName() : FString());
    Root->SetBoolField(TEXT("playing"), IsPlaying());
    Root->SetBoolField(TEXT("looping"), IsLooping());
    Root->SetNumberField(TEXT("animation_time"), GetAnimationTime());
    Root->SetNumberField(TEXT("animation_length"), GetAnimationLength());
    Root->SetNumberField(TEXT("playback_speed"), PlaybackSpeed);
    Root->SetNumberField(TEXT("light_rotation"), LightRotation);
    Root->SetNumberField(TEXT("light_rotation_origin_offset_degrees"), 131.0f);
    Root->SetNumberField(TEXT("light_rotation_delta_from_saved_stage_degrees"), LightRotation + 131.0f);
    Root->SetStringField(TEXT("lighting_preset"), LightingPreset);
    Root->SetStringField(TEXT("lighting_preset_provenance"),
        LightingPreset.Equals(TEXT("RE9"), ESearchCase::IgnoreCase) ? TEXT("ue5_neutral_gallery_approximation") :
        (LightingPreset.Equals(TEXT("Split"), ESearchCase::IgnoreCase) ? TEXT("ue5_split_reference_approximation") : TEXT("ue5_preview_preset")));
    Root->SetBoolField(TEXT("native_re9_lighting_exact"), false);
    Root->SetBoolField(TEXT("hair_visible"), GroomComponent.IsValid() && GroomComponent->IsVisible());
    // Keep this field as the effective preview switch used by the UI and
    // validation.  The reconstructed guide solver may own the guide state
    // while native Niagara is intentionally disabled; report that ownership
    // separately below instead of making the hair appear "off" to callers.
    Root->SetBoolField(TEXT("hair_simulation"), bHairSimulation);
    Root->SetBoolField(TEXT("hair_simulation_requested"), bHairSimulation);
    Root->SetBoolField(TEXT("hair_native_niagara_simulation"),
        GroomComponent.IsValid() && GroomComponent->IsSimulationEnable(0, 0));
    const IConsoleVariable* GuidesCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HairStrands.ViewMode"));
    Root->SetNumberField(TEXT("hair_view_mode"), GuidesCVar ? GuidesCVar->GetInt() : -1);
    Root->SetBoolField(TEXT("hair_guides"), GuidesCVar && GuidesCVar->GetInt() == 21);
    Root->SetNumberField(TEXT("groom_width_cm"), GroomWidthCentimeters);
    Root->SetNumberField(TEXT("camera_yaw"), CameraYaw);
    Root->SetNumberField(TEXT("camera_pitch"), CameraPitch);
    Root->SetNumberField(TEXT("camera_distance_cm"), CameraDistance);
    Root->SetNumberField(TEXT("model_yaw"), ModelYaw);
    Root->SetNumberField(TEXT("model_pitch"), ModelPitch);
    Root->SetNumberField(TEXT("camera_reset_distance_cm"),
        LeonPreviewRuntimePrivate::IsVariantStage(GetWorld())
            ? LeonPreviewRuntimePrivate::VariantStageCameraDistanceCentimeters
            : LeonPreviewRuntimePrivate::DefaultCameraDistanceCentimeters);
    Root->SetNumberField(TEXT("camera_variant_stage_target_offset_x_cm"),
        LeonPreviewRuntimePrivate::IsVariantStage(GetWorld())
            ? LeonPreviewRuntimePrivate::VariantStageCameraTargetOffsetXCentimeters : 0.0f);
    Root->SetNumberField(TEXT("camera_target_base_height_cm"),
        LeonPreviewRuntimePrivate::IsVariantStage(GetWorld())
            ? LeonPreviewRuntimePrivate::VariantStageCameraTargetHeightCentimeters
            : LeonPreviewRuntimePrivate::CameraTargetHeightCentimeters);
    Root->SetStringField(TEXT("camera_projection_provenance"),
        TEXT("explicit_maintain_y_fov_from_35_degree_horizontal_at_16_9"));
    const IConsoleVariable* LegacyYFOV = IConsoleManager::Get().FindConsoleVariable(TEXT("r.UseLegacyMaintainYFOVViewMatrix"));
    Root->SetBoolField(TEXT("camera_legacy_maintain_y_fov"), LegacyYFOV && LegacyYFOV->GetInt() != 0);
    if (const ACameraActor* Camera = PreviewCamera.Get())
    {
        const UCameraComponent* CameraComponent = Camera->GetCameraComponent();
        Root->SetNumberField(TEXT("camera_fov_degrees"), CameraComponent->FieldOfView);
        Root->SetNumberField(TEXT("camera_reference_aspect_ratio"), CameraComponent->AspectRatio);
        Root->SetBoolField(TEXT("camera_axis_constraint_override"), CameraComponent->bOverrideAspectRatioAxisConstraint);
        Root->SetBoolField(TEXT("camera_constrain_aspect_ratio"), CameraComponent->bConstrainAspectRatio);
        Root->SetStringField(TEXT("camera_aspect_ratio_axis_constraint"),
            CameraComponent->AspectRatioAxisConstraint == AspectRatio_MaintainYFOV ? TEXT("MaintainYFOV") : TEXT("Other"));
        const float TanHalfYFOV = FMath::Tan(FMath::DegreesToRadians(CameraComponent->FieldOfView * .5f)) / CameraComponent->AspectRatio;
        Root->SetNumberField(TEXT("camera_vertical_fov_degrees"), FMath::RadiansToDegrees(2.0f * FMath::Atan(TanHalfYFOV)));
        const FVector Target = Camera->GetActorLocation() + Camera->GetActorForwardVector() * CameraDistance;
        TArray<TSharedPtr<FJsonValue>> TargetCoordinates;
        TargetCoordinates.Add(MakeShared<FJsonValueNumber>(Target.X));
        TargetCoordinates.Add(MakeShared<FJsonValueNumber>(Target.Y));
        TargetCoordinates.Add(MakeShared<FJsonValueNumber>(Target.Z));
        Root->SetArrayField(TEXT("camera_target_world_cm"), TargetCoordinates);
    }
    Root->SetNumberField(TEXT("hdr_paper_white_nits"), HDRPaperWhiteNits);
    const IConsoleVariable* HDREnabled = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HDR.EnableHDROutput"));
    Root->SetBoolField(TEXT("hdr_output_requested"), HDREnabled && HDREnabled->GetInt() != 0);
    Root->SetBoolField(TEXT("hdr_output_active"), GEngine && GEngine->GameViewport &&
        GEngine->GameViewport->Viewport && GEngine->GameViewport->Viewport->IsHDRViewport() && IsHDREnabled());
    const IConsoleVariable* PaperWhiteCVar=IConsoleManager::Get().FindConsoleVariable(TEXT("r.HDR.PaperWhite"));
    Root->SetNumberField(TEXT("hdr_paper_white_cvar_nits"), PaperWhiteCVar ? PaperWhiteCVar->GetFloat() : -1);
    Root->SetBoolField(TEXT("projection_interpolation_supported"), true);
    Root->SetBoolField(TEXT("body_physics_supported"), true);
    Root->SetBoolField(TEXT("body_physics"), bBodyPhysicsRequested);
    Root->SetBoolField(TEXT("projection_interpolation"), bProjectionInterpolation);
    Root->SetBoolField(TEXT("re_hair_pipeline_ready"), bREHairPipelineReady);
    Root->SetNumberField(TEXT("re_hair_chain_energy"), REHairChainEnergy);
    Root->SetNumberField(TEXT("re_hair_chain_points"), REHairChain.Num());
    Root->SetBoolField(TEXT("re_hair_dense_propagation"), bREHairDensePropagation);
    Root->SetStringField(TEXT("re_hair_dense_propagation_path"),
        bREHairGuideDirectWrite
            ? TEXT("Render-thread DeformerBuffer UAV -> GroomManager guide interpolation -> dense strands")
            : (bREHairGuideSolverOwnsSimulation
                ? TEXT("Optimus full-curve DeformerBuffer UAV -> GroomManager guide interpolation -> dense strands")
                : TEXT("HairStrands Niagara AirVelocity + Head_0 local velocity scales")));
    Root->SetBoolField(TEXT("re_hair_optimus_asset_saved"),
        StaticLoadObject(UMeshDeformer::StaticClass(), nullptr,
            TEXT("/Game/Leon/Groom/DF_Leon_REHair.DF_Leon_REHair")) != nullptr);
    Root->SetBoolField(TEXT("re_hair_optimus_runtime_attached"),
        GroomComponent.IsValid() && GroomComponent->MeshDeformer != nullptr);
    // Report the effective owner, not merely an attached asset.  BeginPlay
    // can bind the deformer before its MeshDeformerInstance is created; in
    // that short window Niagara still owns simulation and the status must not
    // claim that Optimus already wrote the current guide buffer.
    const bool bOptimusOwner = GroomComponent.IsValid() &&
        GroomComponent->MeshDeformer != nullptr &&
        GroomComponent->MeshDeformerInstance != nullptr &&
        bREHairGuideSolverOwnsSimulation;
    Root->SetStringField(TEXT("re_hair_solver_ownership"),
        bOptimusOwner
            ? TEXT("Optimus full-curve Catmull-Rom guide deformation -> GroomManager guide-to-dense interpolation")
            : (bREHairGuideDirectWrite
                ? TEXT("Custom render-thread guide root/tangent frame -> GroomManager guide-to-dense interpolation")
                : TEXT("HairStrands Niagara AirVelocity + Head_0 local velocity scales")));
    Root->SetBoolField(TEXT("re_hair_guide_solver_owns_simulation"), bREHairGuideSolverOwnsSimulation);
    Root->SetBoolField(TEXT("re_hair_dense_positions_direct_write"), bREHairGuideDirectWrite);
    Root->SetBoolField(TEXT("re_hair_dense_direction_propagated"), bREHairDensePropagation);
    TArray<TSharedPtr<FJsonValue>> TransportVelocity;
    TransportVelocity.Add(MakeShared<FJsonValueNumber>(REHairTransportVelocity.X));
    TransportVelocity.Add(MakeShared<FJsonValueNumber>(REHairTransportVelocity.Y));
    TransportVelocity.Add(MakeShared<FJsonValueNumber>(REHairTransportVelocity.Z));
    Root->SetArrayField(TEXT("re_hair_transport_velocity_world_cm_s"), TransportVelocity);
    TArray<TSharedPtr<FJsonValue>> DenseAirVelocity;
    DenseAirVelocity.Add(MakeShared<FJsonValueNumber>(REHairDenseAirVelocity.X));
    DenseAirVelocity.Add(MakeShared<FJsonValueNumber>(REHairDenseAirVelocity.Y));
    DenseAirVelocity.Add(MakeShared<FJsonValueNumber>(REHairDenseAirVelocity.Z));
    Root->SetArrayField(TEXT("re_hair_dense_air_velocity_local_cm_s"), DenseAirVelocity);
    TArray<TSharedPtr<FJsonValue>> TipDirection;
    TipDirection.Add(MakeShared<FJsonValueNumber>(REHairTipDirection.X));
    TipDirection.Add(MakeShared<FJsonValueNumber>(REHairTipDirection.Y));
    TipDirection.Add(MakeShared<FJsonValueNumber>(REHairTipDirection.Z));
    Root->SetArrayField(TEXT("re_hair_tip_direction_world"), TipDirection);
    Root->SetStringField(TEXT("re_hair_pipeline_provenance"),
        TEXT("RE9 STRD/SBD + chain2 reconstruction: damped Head_0 chain and four-point Catmull-Rom projection; UE HairStrands raster/BSDF remains active."));
    Root->SetBoolField(TEXT("native_re9_renderer_supported"), false);
    Root->SetBoolField(TEXT("native_re9_renderer"), false);
    Root->SetBoolField(TEXT("variant_cloth_simulation_active"), IsVariantClothSimulationActive());
    Root->SetNumberField(TEXT("variant_cloth_station_count"), GetVariantClothStationCount());
    Root->SetNumberField(TEXT("variant_cloth_bone_count"), GetVariantClothBoneCount());
    Root->SetStringField(TEXT("variant_cloth_simulation_path"),
        TEXT("PoseableMesh copy of imported animation pose + bounded RE-style secondary-bone spring/lag"));
    Root->SetBoolField(TEXT("stage_ui_added"), bStageWidgetAdded);
    Root->SetNumberField(TEXT("animation_registry_count"), AnimationPaths.Num());
    Root->SetBoolField(TEXT("animation_registry_initialized"), bAnimationCatalogInitialized);
    if (USkeletalMeshComponent* Hero = HeroComponent.Get())
    {
        Root->SetStringField(TEXT("preview_mesh"), Hero->GetSkeletalMeshAsset() ? Hero->GetSkeletalMeshAsset()->GetPathName() : FString());
        Root->SetNumberField(TEXT("preview_bones"), Hero->GetNumBones());
        Root->SetNumberField(TEXT("preview_morphs"), Hero->GetSkeletalMeshAsset() ? Hero->GetSkeletalMeshAsset()->GetMorphTargets().Num() : 0);
        Root->SetNumberField(TEXT("predicted_mesh_lod"), Hero->GetPredictedLODLevel());
        Root->SetBoolField(TEXT("paused_anims"), Hero->bPauseAnims);
        Root->SetBoolField(TEXT("force_ref_pose"), Hero->bForceRefpose);
        TArray<TSharedPtr<FJsonValue>> HairSections;
        bool bAllHidden = true;
        const int32 Lods = Hero->GetSkeletalMeshAsset() ? Hero->GetSkeletalMeshAsset()->GetLODNum() : 0;
        for (int32 Lod=0;Lod<Lods;++Lod)
        {
            for(const int32 Material : {8,9,10})
            {
                TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
                const bool bShown=Hero->IsMaterialSectionShown(Material,Lod);
                Section->SetNumberField(TEXT("material_index"),Material);
                Section->SetNumberField(TEXT("lod"),Lod);
                Section->SetBoolField(TEXT("shown"),bShown);
                bAllHidden &= !bShown;
                HairSections.Add(MakeShared<FJsonValueObject>(Section));
            }
        }
        Root->SetArrayField(TEXT("hair_card_sections"),HairSections);
        Root->SetBoolField(TEXT("all_hair_card_sections_hidden"),Lods > 0 && bAllHidden);
        Root->SetStringField(TEXT("head_world_cm"),Hero->GetSocketLocation(TEXT("Head_0")).ToString());
    }
    if (UGroomComponent* Groom=GroomComponent.Get())
    {
        Root->SetStringField(TEXT("groom_asset"),Groom->GroomAsset ? Groom->GroomAsset->GetPathName() : FString());
        Root->SetStringField(TEXT("groom_binding"),Groom->BindingAsset ? Groom->BindingAsset->GetPathName() : FString());
        Root->SetStringField(TEXT("groom_physics_asset"),Groom->PhysicsAsset ? Groom->PhysicsAsset->GetPathName() : FString());
        Root->SetStringField(TEXT("groom_parent"),Groom->GetAttachParent() ? Groom->GetAttachParent()->GetPathName() : FString());
        TSharedRef<FJsonObject> Scalars=MakeShared<FJsonObject>();
        for(const FName Parameter:{FName(TEXT("DryRoughness")),FName(TEXT("WetRoughness")),FName(TEXT("WetAmount")),FName(TEXT("Specular")),FName(TEXT("Scatter")),FName(TEXT("Backlit")),FName(TEXT("AmbientOcclusion"))})
            Scalars->SetNumberField(Parameter.ToString(),GetGroomScalar(Parameter));
        Root->SetObjectField(TEXT("groom_scalars"),Scalars);
        TArray<TSharedPtr<FJsonValue>> Groups;
        for(const FHairGroupDesc& Desc:Groom->GroomGroupsDesc)
        {
            TSharedRef<FJsonObject> Group=MakeShared<FJsonObject>();
            Group->SetNumberField(TEXT("width_cm"),Desc.HairWidth);
            Group->SetBoolField(TEXT("width_override"),Desc.HairWidth_Override);
            Groups.Add(MakeShared<FJsonValueObject>(Group));
        }
        Root->SetArrayField(TEXT("groom_width_groups"),Groups);
        if(Groom->GroomAsset)
        {
            int32 Curves=0,Guides=0;
            for(const FHairGroupInfoWithVisibility& Group:Groom->GroomAsset->GetHairGroupsInfo()){Curves+=Group.NumCurves;Guides+=Group.NumGuides;}
            Root->SetNumberField(TEXT("groom_render_curves"),Curves);
            Root->SetNumberField(TEXT("groom_guide_curves"),Guides);
            Root->SetBoolField(TEXT("ue_native_global_interpolation"),Groom->GroomAsset->GetEnableGlobalInterpolation());
        }
    }
    TArray<TSharedPtr<FJsonValue>> Lights;
    for(const FString& Label:{FString(TEXT("Key_Split")),FString(TEXT("Fill_Softbox")),FString(TEXT("Rim_Hair"))})
    {
        if(AActor* Actor=FindActorByTagOrName(FName(*FString(TEXT("LeonStage_")+Label)),Label))
        {
            TSharedRef<FJsonObject> Light=MakeShared<FJsonObject>();
            Light->SetStringField(TEXT("name"),Label);
            Light->SetStringField(TEXT("rotation"),Actor->GetActorRotation().ToString());
            if(const ULightComponent* Component=Actor->FindComponentByClass<ULightComponent>())
            {
                Light->SetNumberField(TEXT("intensity"),Component->Intensity);
                Light->SetBoolField(TEXT("use_temperature"),Component->bUseTemperature);
                const FLinearColor Color=Component->GetLightColor();
                TArray<TSharedPtr<FJsonValue>> RGB;
                RGB.Add(MakeShared<FJsonValueNumber>(Color.R));
                RGB.Add(MakeShared<FJsonValueNumber>(Color.G));
                RGB.Add(MakeShared<FJsonValueNumber>(Color.B));
                Light->SetArrayField(TEXT("color_linear_rgb"),RGB);
            }
            Lights.Add(MakeShared<FJsonValueObject>(Light));
        }
    }
    Root->SetArrayField(TEXT("lights"),Lights);
    FString Output;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
    FJsonSerializer::Serialize(Root, Writer);
    return Output;
}

void ALeonPreviewController::CapturePreviewScreenshot(const FString& AbsolutePath)
{
    const FString Path = AbsolutePath.IsEmpty()
        ? FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved/LeonPreview.png"))
        : AbsolutePath;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    UGameViewportClient* GameViewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    TSharedPtr<SViewport> ViewportWidget = GameViewport ? GameViewport->GetGameViewportWidget() : nullptr;
    TArray<FColor> Pixels;
    FIntVector Size(0, 0, 0);
    // A global FScreenshotRequest can be consumed by an editor viewport before
    // PIE draws. Capture this world's actual game widget, including its Slate UI.
    if (!ViewportWidget.IsValid() || !FSlateApplication::IsInitialized() ||
        !FSlateApplication::Get().TakeScreenshot(ViewportWidget.ToSharedRef(), Pixels, Size))
    {
        UE_LOG(LogTemp, Error, TEXT("Leon stage screenshot could not capture its game viewport: %s"), *Path);
        SavePreviewState(TEXT("screenshot_failed"));
        return;
    }
    for (FColor& Pixel : Pixels) Pixel.A = 255;
    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
        TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
    const bool bSaved = FFileHelper::SaveArrayToFile(Png, *Path);
    UE_LOG(LogTemp, Log, TEXT("Leon stage screenshot %s: %s (%d x %d)"),
        bSaved ? TEXT("saved") : TEXT("failed"), *Path, Size.X, Size.Y);
    SavePreviewState(bSaved ? TEXT("screenshot_saved") : TEXT("screenshot_failed"));
}

void ALeonPreviewController::SavePreviewState(const FString& Label)
{
    const FString ScriptsDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Scripts")));
    IFileManager::Get().MakeDirectory(*ScriptsDir, true);
    const FString HistoryPath = FPaths::Combine(ScriptsDir, TEXT("leon_stage_ui_snapshots.jsonl"));
    const FString LatestPath = FPaths::Combine(ScriptsDir, TEXT("leon_stage_ui_snapshot_latest.json"));
    TSharedPtr<FJsonObject> State;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GetPreviewStateJson());
    FJsonSerializer::Deserialize(Reader, State);
    if (!State.IsValid()) State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("generated"), FDateTime::UtcNow().ToIso8601());
    State->SetStringField(TEXT("label"), Label);
    State->SetStringField(TEXT("world"), GetWorld() ? GetWorld()->GetPathName() : FString());
    State->SetStringField(TEXT("hero_actor"), HeroActor.IsValid() ? HeroActor->GetPathName() : FString());
    State->SetStringField(TEXT("groom_component"), GroomComponent.IsValid() ? GroomComponent->GetPathName() : FString());
    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(State.ToSharedRef(), Writer);
    FFileHelper::SaveStringToFile(Json + LINE_TERMINATOR, *LatestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(Json + LINE_TERMINATOR, *HistoryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
        &IFileManager::Get(), FILEWRITE_Append);
}

ALeonPreviewGameMode::ALeonPreviewGameMode()
{
    PlayerControllerClass = ALeonPreviewController::StaticClass();
    DefaultPawnClass = nullptr;
}


