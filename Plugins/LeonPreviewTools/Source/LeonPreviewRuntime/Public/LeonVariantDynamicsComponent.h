#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LeonVariantDynamicsComponent.generated.h"

class UPoseableMeshComponent;
class USkeletalMeshComponent;

/**
 * Runtime secondary-motion bridge for the outfit stations in LeonVariantStage.
 *
 * The extracted RE outfit records expose jacket/collar/fastener secondary
 * bones, but their private chain2/jcns solver is not available to UE.  This
 * component copies each station's evaluated animation pose into a poseable
 * render component and applies a bounded spring/lag solve to those secondary
 * bones.  The source SkeletalMeshComponent continues to evaluate the imported
 * action, while the visible poseable component receives the cloth response.
 */
UCLASS(ClassGroup=(Leon), meta=(BlueprintSpawnableComponent))
class LEONPREVIEWRUNTIME_API ULeonVariantDynamicsComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULeonVariantDynamicsComponent();

    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintPure, Category="Leon Preview|Cloth")
    int32 GetActiveStationCount() const { return ActiveStationCount; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Cloth")
    int32 GetActiveBoneCount() const { return ActiveBoneCount; }

    UFUNCTION(BlueprintPure, Category="Leon Preview|Cloth")
    bool IsSimulationActive() const { return ActiveBoneCount > 0; }

private:
    struct FSecondaryBoneState
    {
        FName BoneName = NAME_None;
        FQuat SimulatedRotation = FQuat::Identity;
        float Lag = 0.20f;
        float AmplitudeDegrees = 2.0f;
        float Phase = 0.0f;
    };

    struct FStationState
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<USkeletalMeshComponent> Source;
        TObjectPtr<UPoseableMeshComponent> Pose = nullptr;
        TArray<FSecondaryBoneState> Bones;
        FTransform PreviousComponentTransform = FTransform::Identity;
        bool bInitialized = false;
    };

    void DiscoverStations();
    bool BuildStation(AActor* Actor);
    void SimulateStation(FStationState& Station, float DeltaTime, float TimeSeconds);
    static bool IsSecondaryMotionBone(const FName& BoneName);
    static float StablePhase(const FName& BoneName);

    UPROPERTY(Transient)
    int32 ActiveStationCount = 0;

    UPROPERTY(Transient)
    int32 ActiveBoneCount = 0;

    TArray<FStationState> Stations;
    bool bDiscovered = false;
};
