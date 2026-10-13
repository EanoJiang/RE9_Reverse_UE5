#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameModeBase.h"
#include "LeonREHairPipeline.h"
#include "LeonPreviewController.generated.h"

class UGroomComponent;
class USkeletalMeshComponent;
class UAnimSequence;
class ACameraActor;
class SWidget;
class ULeonREHairGuideBufferComponent;
class ULeonVariantDynamicsComponent;

/** Runtime controller for the Leon character stage. The exposed methods are also
 * intentionally usable from Python/editor automation and from the Slate stage UI. */
UCLASS(BlueprintType)
class LEONPREVIEWRUNTIME_API ALeonPreviewController : public APlayerController
{
    GENERATED_BODY()

public:
    ALeonPreviewController();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Animation")
    bool SetPreviewAnimation(const FString& AnimationOrPreset);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Animation")
    void SetPlaying(bool bInPlaying);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Animation")
    void SetLooping(bool bInLooping);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Animation")
    void SetPlaybackSpeed(float InSpeed);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Animation")
    void ScrubAnimation(float NormalizedTime);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Lighting")
    void SetLightingPreset(const FString& PresetName);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Lighting")
    void SetLightRotation(float InYawDegrees);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Lighting")
    void SetHDRPaperWhite(float InPaperWhiteNits);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Groom")
    void SetGroomScalar(FName Parameter, float Value);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Groom")
    void SetGroomWidth(float WidthCentimeters);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Groom")
    void SetHairVisible(bool bVisible);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Groom")
    void SetHairSimulation(bool bEnabled);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Groom")
    void SetHairGuides(bool bEnabled);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Groom")
    void ResetGroom();

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Camera")
    void SetCameraOrbit(float YawDegrees, float PitchDegrees, float DistanceCentimeters);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Camera")
    void ResetCamera();

    /** Rotate the preview character itself. Groom local simulation reads the
     * Head_0 bone motion from this actor, so viewport dragging produces hair
     * inertia instead of orbiting only the camera. */
    UFUNCTION(BlueprintCallable, Category="Leon Preview|Model")
    void SetModelRotation(float YawDegrees, float PitchDegrees);

    /** Absolute world-yaw adapter used by runtime/GPU validation and external
     * automation. Pitch and roll remain at the saved character orientation. */
    UFUNCTION(BlueprintCallable, Category="Leon Preview|Model")
    void SetHeroRotation(float WorldYawDegrees);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Model")
    void ResetModelRotation();

    UFUNCTION(BlueprintPure, Category="Leon Preview|Model")
    float GetModelYaw() const { return ModelYaw; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Model")
    float GetModelPitch() const { return ModelPitch; }

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Simulation")
    void SetProjectionInterpolation(bool bEnabled);

    /** The assembled preview mesh has no stable full-body PhysicsAsset. This
     * records the request and returns false rather than enabling unsafe rigid-body simulation. */
    UFUNCTION(BlueprintCallable, Category="Leon Preview|Simulation")
    bool SetBodyPhysics(bool bEnabled);

    UFUNCTION(BlueprintPure, Category="Leon Preview|State")
    FString GetPreviewStateJson() const;

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Evidence")
    void CapturePreviewScreenshot(const FString& AbsolutePath);

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Evidence")
    void SavePreviewState(const FString& Label);

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    TArray<FString> GetAnimationPaths() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    bool IsPlaying() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    bool IsLooping() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    bool GetPlaying() const { return IsPlaying(); }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    bool GetLooping() const { return IsLooping(); }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    FString GetAnimationSelection() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    float GetAnimationTime() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    float GetAnimationLength() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Animation")
    float GetPlaybackSpeed() const { return PlaybackSpeed; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Camera")
    FVector GetCameraOrbit() const { return FVector(CameraYaw, CameraPitch, CameraDistance); }

    UFUNCTION(BlueprintCallable, Category="Leon Preview|Camera")
    void OffsetCameraPan(float DeltaX, float DeltaY);

    UFUNCTION(BlueprintPure, Category="Leon Preview|Camera")
    float GetCameraYaw() const { return CameraYaw; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Camera")
    float GetCameraPitch() const { return CameraPitch; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Camera")
    float GetCameraDistance() const { return CameraDistance; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Lighting")
    float GetLightRotation() const { return LightRotation; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Lighting")
    FString GetLightingPreset() const { return LightingPreset; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Groom")
    bool GetHairVisible() const { return bHairVisible; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Groom")
    bool GetHairSimulation() const { return bHairSimulation; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Groom")
    bool GetHairGuides() const { return bHairGuides; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Groom")
    float GetGroomWidth() const { return GroomWidthCentimeters; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Groom")
    float GetGroomScalar(FName Parameter) const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Simulation")
    bool GetProjectionInterpolation() const { return bProjectionInterpolation; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Simulation")
    bool GetBodyPhysics() const { return bBodyPhysicsRequested; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Simulation")
    bool GetBodyPhysicsSupported() const { return true; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Simulation")
    bool GetProjectionInterpolationSupported() const { return true; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Simulation")
    float GetREHairChainEnergy() const { return REHairChainEnergy; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|HDR")
    float GetHDRPaperWhite() const { return HDRPaperWhiteNits; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Simulation")
    bool IsBodyPhysicsRequested() const { return bBodyPhysicsRequested; }

    /** Accessors used by the Slate stage and automation tests. */
    USkeletalMeshComponent* GetHeroComponent() const { return HeroComponent.Get(); }
    UGroomComponent* GetGroomComponent() const { return GroomComponent.Get(); }
    AActor* GetHeroActor() const { return HeroActor.Get(); }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Cloth")
    int32 GetVariantClothStationCount() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Cloth")
    int32 GetVariantClothBoneCount() const;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Cloth")
    bool IsVariantClothSimulationActive() const;

protected:
    TWeakObjectPtr<AActor> HeroActor;
    TWeakObjectPtr<USkeletalMeshComponent> HeroComponent;
    TWeakObjectPtr<AActor> GroomActor;
    TWeakObjectPtr<UGroomComponent> GroomComponent;
    TWeakObjectPtr<ULeonREHairGuideBufferComponent> REHairGuideBuffer;
    TWeakObjectPtr<ACameraActor> PreviewCamera;

    /** Runtime RE-style secondary-motion bridge for the multi-outfit stage. */
    UPROPERTY(Transient)
    TObjectPtr<ULeonVariantDynamicsComponent> VariantDynamics;

    UPROPERTY(Transient)
    TObjectPtr<UAnimSequence> CurrentAnimation;

    UPROPERTY(Transient)
    float PlaybackSpeed = 1.0f;

    UPROPERTY(Transient)
    float CameraYaw = 90.0f;

    UPROPERTY(Transient)
    float CameraPitch = 0.0f;

    UPROPERTY(Transient)
    float CameraDistance = 300.0f;

    UPROPERTY(Transient)
    float ModelYaw = 0.0f;

    UPROPERTY(Transient)
    float ModelPitch = 0.0f;

    UPROPERTY(Transient)
    float TargetModelYaw = 0.0f;

    UPROPERTY(Transient)
    float TargetModelPitch = 0.0f;

    /**
     * Runtime turntable motion bookkeeping.  HairStrands derives the local
     * head velocity from the transform delta supplied each frame; keeping a
     * bounded turn rate and resetting once after a completed turn prevents a
     * long drag or a frame hitch from feeding an unbounded impulse into the
     * Groom solver.
     */
    UPROPERTY(Transient)
    float RotationStillTime = 0.0f;

    UPROPERTY(Transient)
    bool bRotationRecentlyActive = false;

    UPROPERTY(Transient)
    bool bHairResetAfterRotation = false;

    /** Last single-node animation time used to detect loop/scrub jumps. */
    UPROPERTY(Transient)
    float PreviousAnimationTime = -1.0f;

    /** World-space Head_0 sample used to limit animation/actor pose deltas. */
    UPROPERTY(Transient)
    FTransform PreviousHeadComponentTransform = FTransform::Identity;

    UPROPERTY(Transient)
    bool bHasPreviousHeadComponentTransform = false;

    /**
     * HairStrands multiplies the measured local-bone velocity by the Groom
     * SimulationSetup velocity scales.  Imported clips can contain a genuine
     * end-to-start pose discontinuity, so keeping a fixed scale would turn one
     * bad sample into an unbounded acceleration.  These values are a soft
     * limiter: they attack immediately on a spike and recover gradually while
     * normal head and turntable motion keeps its visible inertia.
     */
    UPROPERTY(Transient)
    float HairAngularVelocityScale = 0.12f;

    UPROPERTY(Transient)
    float HairLinearVelocityScale = 0.08f;

    UPROPERTY(Transient)
    FVector HairLimitedAngularVelocity = FVector::ZeroVector;

    UPROPERTY(Transient)
    FVector HairLimitedLinearVelocity = FVector::ZeroVector;

    UPROPERTY(Transient)
    bool bLooping = true;

    UPROPERTY(Transient)
    bool bHairVisible = true;

    UPROPERTY(Transient)
    bool bHairSimulation = true;

    UPROPERTY(Transient)
    bool bHairGuides = false;

    UPROPERTY(Transient)
    // Default to the high-quality RE four-point path.  The enabled branch
    // performs the compact-chain Catmull-Rom projection with five substeps
    // and eight XPBD iterations; the UI can still turn it off for a cheaper
    // preview profile.
    bool bProjectionInterpolation = true;

    UPROPERTY(Transient)
    bool bBodyPhysicsRequested = false;

    /** Reconstructed RE9 chain/interpolation stages. */
    UPROPERTY(Transient)
    bool bREHairPipelineReady = false;

    UPROPERTY(Transient)
    float REHairChainEnergy = 0.0f;

    /**
     * Guide-to-dense bridge state.  UE Groom does not expose writable guide
     * positions from a component; the solved chain therefore drives the
     * actual HairStrands Niagara solver through its AirVelocity input.  Keep
     * these values visible in the state snapshot so validation can distinguish
     * a live dense propagation path from a guide-only approximation.
     */
    UPROPERTY(Transient)
    FVector REHairTransportVelocity = FVector::ZeroVector;

    UPROPERTY(Transient)
    FVector REHairTipDirection = FVector::UpVector;

    UPROPERTY(Transient)
    FVector REHairDenseAirVelocity = FVector::ZeroVector;

    UPROPERTY(Transient)
    bool bREHairDensePropagation = false;

    /** True after a render-thread guide DeformerBuffer UAV write was queued. */
    UPROPERTY(Transient)
    bool bREHairGuideDirectWrite = false;

    /** True while the reconstructed render-thread guide pass owns simulation.
     *  UE's Niagara strand solver must be disabled in this mode so it cannot
     * overwrite the guide DeformerBuffer with a second, conflicting solve. */
    UPROPERTY(Transient)
    bool bREHairGuideSolverOwnsSimulation = false;

    TArray<FVector> REHairChainRestOffsets;
    TArray<struct LeonREHairPipeline::FChainState> REHairChain;

    UPROPERTY(Transient)
    bool bStageWidgetAdded = false;

    UPROPERTY(Transient)
    float GroomWidthCentimeters = 0.02f;

    UPROPERTY(Transient)
    float LightRotation = -131.0f;

    UPROPERTY(Transient)
    float HDRPaperWhiteNits = 120.0f;

    UPROPERTY(Transient)
    FString LightingPreset = TEXT("Split");

    FRotator InitialHeroRotation = FRotator::ZeroRotator;

    UPROPERTY(Transient)
    FString AnimationSelection = TEXT("native");

    UPROPERTY(Transient)
    FVector2D CameraPan = FVector2D::ZeroVector;

    TMap<FString, FRotator> InitialLightRotations;
    TSharedPtr<SWidget> StageWidget;
    mutable TArray<FString> AnimationPaths;
    mutable bool bAnimationCatalogInitialized = false;

    void DiscoverPreviewActors();
    void SpawnPreviewCamera();
    void AddStageWidget();
    void HideHairCardSections();
    void ApplyCameraTransform();
    UAnimSequence* ResolveAnimation(const FString& Query) const;
    bool PlayAnimationAsset(UAnimSequence* Animation);
    bool ApplyOriginalReferencePose(const FString& Selection);
    bool ApplyDefaultAnimationPose();
    void ApplyLightColorsAndIntensity(const FString& PresetName);
    AActor* FindActorByTagOrName(const FName Tag, const FString& NameFragment) const;
    void SetActorLightRotation(AActor* Actor, const FRotator& Rotation) const;
    void SetHairScalarOnComponent(FName Parameter, float Value) const;
    void ResetGroomForAnimationDiscontinuity();
    void UpdateHairVelocityLimiter(float DeltaSeconds, const FTransform& HeadWorldTransform);
    void InitializeREHairPipeline(const FTransform& HeadWorldTransform);
    void TickREHairPipeline(float DeltaSeconds, const FTransform& HeadWorldTransform);
    void ApplyREHairPipelineSettings();
};

/** Optional GameMode for launching the preview map directly in PIE or a packaged build. */
UCLASS(BlueprintType)
class LEONPREVIEWRUNTIME_API ALeonPreviewGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ALeonPreviewGameMode();
};
