#include "LeonVariantDynamicsComponent.h"

#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ULeonVariantDynamicsComponent::ULeonVariantDynamicsComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.0f;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void ULeonVariantDynamicsComponent::BeginPlay()
{
    Super::BeginPlay();
    DiscoverStations();
}

void ULeonVariantDynamicsComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    for (FStationState& Station : Stations)
    {
        if (Station.Source.IsValid())
        {
            Station.Source->SetVisibility(true, true);
        }
        if (Station.Pose)
        {
            Station.Pose->DestroyComponent();
            Station.Pose = nullptr;
        }
    }
    Stations.Reset();
    ActiveStationCount = 0;
    ActiveBoneCount = 0;
    Super::EndPlay(EndPlayReason);
}

bool ULeonVariantDynamicsComponent::IsSecondaryMotionBone(const FName& BoneName)
{
    const FString Name = BoneName.ToString().ToLower();
    if (Name.Contains(TEXT("eyebag")) || Name.Contains(TEXT("mouthbag")))
    {
        return false;
    }
    // These names are present in the imported RE jacket/chain skeletons.  The
    // primary JacketA..K bones follow the body; their Sec/Collar descendants
    // are the authored secondary-motion controls that need lag and wind.
    const bool bJacketSecondary = Name.Contains(TEXT("jacket")) &&
        (Name.Contains(TEXT("sec")) || Name.Contains(TEXT("collor")) ||
         Name.Contains(TEXT("coller")));
    const bool bAccessoryChain = Name.Contains(TEXT("collerchain")) ||
        Name.Contains(TEXT("fastener_chain")) || Name.Contains(TEXT("rope"));
    return (bJacketSecondary || bAccessoryChain) && !Name.Contains(TEXT("offset"));
}

float ULeonVariantDynamicsComponent::StablePhase(const FName& BoneName)
{
    const uint32 Hash = GetTypeHash(BoneName);
    return static_cast<float>(Hash & 0xFFFFu) / 65535.0f * UE_TWO_PI;
}

void ULeonVariantDynamicsComponent::DiscoverStations()
{
    if (bDiscovered || !GetWorld())
    {
        return;
    }
    bDiscovered = true;

    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || !Actor->ActorHasTag(FName(TEXT("LeonVariant"))) ||
            !Actor->ActorHasTag(FName(TEXT("LeonClothSim"))))
        {
            continue;
        }
        BuildStation(Actor);
    }
}

bool ULeonVariantDynamicsComponent::BuildStation(AActor* Actor)
{
    if (!Actor)
    {
        return false;
    }
    USkeletalMeshComponent* Source = Actor->FindComponentByClass<USkeletalMeshComponent>();
    if (!Source || !Source->GetSkeletalMeshAsset())
    {
        return false;
    }

    // The source is deliberately invisible after the PoseableMesh copy is
    // created. In a -game world it therefore never becomes RecentlyRendered.
    // AlwaysTickPose alone can advance the single-node time while leaving its
    // bone-space transforms at the reference pose, which made every visible
    // station stay in A pose despite the animation counters advancing.
    Source->VisibilityBasedAnimTickOption =
        EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Source->bEnableUpdateRateOptimizations = false;
    Source->bNoSkeletonUpdate = false;
    Source->bRenderStatic = false;
    Source->SetForceRefPose(false);
    Source->SetComponentTickEnabled(true);
    Source->PrimaryComponentTick.TickInterval = 0.0f;

    // Level assets created through the editor Python API can retain the
    // animation assignment but lose the transient single-node instance when
    // PIE duplicates the world. Rebind the authored action at runtime from
    // the stable imported paths so every preview station actually advances.
    FString Label = Actor->GetName();
#if WITH_EDITOR
    Label = Actor->GetActorLabel();
#endif
    FString ActionPath;
    if (Actor->ActorHasTag(FName(TEXT("LeonVariantRole_Assembled_Axe"))) ||
        Label.Contains(TEXT("Assembled_Axe"), ESearchCase::IgnoreCase))
    {
        ActionPath = TEXT("/Game/Leon/Animations/FullRig/ch0100_axe00hfps_28104e8a/A_ch0100_Axe00H_8000_Phot_Stand_id8000.A_ch0100_Axe00H_8000_Phot_Stand_id8000");
    }
    else if (Actor->ActorHasTag(FName(TEXT("LeonVariantRole_45_Body_Handgun"))) ||
             Actor->ActorHasTag(FName(TEXT("LeonVariantRole_45_Jacket_Overlay"))) ||
             Label.Contains(TEXT("45_"), ESearchCase::IgnoreCase))
    {
        ActionPath = TEXT("/Game/Leon/Animations/FullRig/ch0100_chainsaw00fps_f0d70908/A_ch0100_Handgun00fps_0800_Hold_walk_Loop_VerA_id800.A_ch0100_Handgun00fps_0800_Hold_walk_Loop_VerA_id800");
    }
    else if (Actor->ActorHasTag(FName(TEXT("LeonVariantRole_Shotgun_Walk"))) ||
             Label.Contains(TEXT("Shotgun"), ESearchCase::IgnoreCase))
    {
        ActionPath = TEXT("/Game/Leon/Animations/FullRig/ch0100_chainsaw00m_e9188e4a/A_ch0100_Shotgun00M_2270_walk_Loop_id2270.A_ch0100_Shotgun00M_2270_walk_Loop_id2270");
    }
    else if (Actor->ActorHasTag(FName(TEXT("LeonVariantRole_REHair_Hero"))) ||
             Label.Contains(TEXT("REHair_Hero"), ESearchCase::IgnoreCase))
    {
        ActionPath = TEXT("/Game/Leon/Animations/FullRig/ch0100_axe00hfps_28104e8a/A_ch0100_Axe00H_0160_Hold_Loop_id6000.A_ch0100_Axe00H_0160_Hold_Loop_id6000");
    }
    if (!ActionPath.IsEmpty())
    {
        if (UAnimSequence* Action = LoadObject<UAnimSequence>(nullptr, *ActionPath))
        {
            Source->SetAnimationMode(EAnimationMode::AnimationSingleNode);
            Source->SetAnimation(Action);
            Source->SetPlayRate(1.0f);
            Source->bPauseAnims = false;
            Source->SetComponentTickEnabled(true);
            Source->PlayAnimation(Action, true);
        }
    }

    // Seed the copy with the authored frame-zero pose before hiding the
    // source, rather than waiting for its first ordinary component tick.
    Source->TickAnimation(0.0f, false);
    Source->RefreshBoneTransforms();

    FStationState Station;
    Station.Actor = Actor;
    Station.Source = Source;
    Station.PreviousComponentTransform = Source->GetComponentTransform();

    UPoseableMeshComponent* Pose = NewObject<UPoseableMeshComponent>(Actor,
        MakeUniqueObjectName(Actor, UPoseableMeshComponent::StaticClass(),
            FName(TEXT("LeonVariantClothPose"))));
    if (!Pose)
    {
        return false;
    }
    Pose->SetSkeletalMesh(Source->GetSkeletalMeshAsset());
    Pose->AttachToComponent(Source, FAttachmentTransformRules::KeepRelativeTransform);
    Pose->SetRelativeTransform(FTransform::Identity);
    Pose->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Pose->SetCastShadow(Source->CastShadow);
    for (int32 MaterialIndex = 0; MaterialIndex < Source->GetNumMaterials(); ++MaterialIndex)
    {
        Pose->SetMaterial(MaterialIndex, Source->GetMaterial(MaterialIndex));
    }
    // Preserve per-LOD material-section visibility from the animated source.
    // The hero controller hides native hair cards on the source mesh; a
    // Poseable copy otherwise resurrects those cards and causes a second,
    // rigid hair layer to sit over the Groom strands.
    if (const USkeletalMesh* Mesh = Source->GetSkeletalMeshAsset())
    {
        const int32 LODCount = Mesh->GetLODNum();
        for (int32 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
        {
            for (int32 MaterialIndex = 0; MaterialIndex < Source->GetNumMaterials(); ++MaterialIndex)
            {
                Pose->ShowMaterialSection(MaterialIndex, INDEX_NONE,
                    Source->IsMaterialSectionShown(MaterialIndex, LODIndex), LODIndex);
            }
        }
    }
    // Hair cards occupy slots 8..10 on the imported Leon meshes. Groom is the
    // active hair representation in this stage, so keep those rigid cards
    // hidden on both the animated source and visible Poseable copy for every
    // LOD. This also prevents a LOD transition from resurrecting them.
    if (const USkeletalMesh* Mesh = Source->GetSkeletalMeshAsset())
    {
        const int32 LODCount = Mesh->GetLODNum();
        for (int32 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
        {
            for (const int32 HairSlot : {8, 9, 10})
            {
                if (HairSlot < Source->GetNumMaterials())
                {
                    Source->ShowMaterialSection(HairSlot, INDEX_NONE, false, LODIndex);
                    Pose->ShowMaterialSection(HairSlot, INDEX_NONE, false, LODIndex);
                }
            }
        }
    }
    Pose->RegisterComponent();
    Pose->CopyPoseFromSkeletalComponent(Source);
    Pose->RefreshBoneTransforms();

    const int32 BoneCount = Source->GetNumBones();
    for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const FName BoneName = Source->GetBoneName(BoneIndex);
        if (!IsSecondaryMotionBone(BoneName))
        {
            continue;
        }
        const FTransform BoneTransform = Pose->GetBoneTransformByName(
            BoneName, EBoneSpaces::ComponentSpace);
        FSecondaryBoneState State;
        State.BoneName = BoneName;
        State.SimulatedRotation = BoneTransform.GetRotation();
        const FString LowerName = BoneName.ToString().ToLower();
        const bool bLongChain = LowerName.Contains(TEXT("collerchain")) ||
            LowerName.Contains(TEXT("fastener_chain"));
        State.Lag = bLongChain ? 0.24f : 0.18f;
        State.AmplitudeDegrees = bLongChain ? 4.0f : 2.5f;
        State.Phase = StablePhase(BoneName);
        Station.Bones.Add(State);
    }

    if (Station.Bones.Num() == 0)
    {
        Pose->DestroyComponent();
        return false;
    }

    // The source still evaluates its single-node animation; only the visible
    // poseable copy is rendered and receives the secondary-motion correction.
    // Hide only the source mesh.  Propagating visibility to attached children
    // would also hide the visible poseable copy created above.
    Source->SetVisibility(false, false);
    Pose->SetVisibility(true, true);
    // Animation must evaluate before this post-physics secondary pass.
    AddTickPrerequisiteComponent(Source);
    Station.Pose = Pose;
    Stations.Add(MoveTemp(Station));
    ActiveStationCount = Stations.Num();
    ActiveBoneCount += Stations.Last().Bones.Num();
    return true;
}

void ULeonVariantDynamicsComponent::SimulateStation(FStationState& Station,
    float DeltaTime, float TimeSeconds)
{
    USkeletalMeshComponent* Source = Station.Source.Get();
    UPoseableMeshComponent* Pose = Station.Pose;
    if (!Source || !Pose || !Source->IsRegistered() || !Pose->IsRegistered())
    {
        return;
    }

    // The prerequisite orders the component ticks; finish any outstanding
    // parallel evaluation before reading the local transforms as well.
    Source->HandleExistingParallelEvaluationTask(true, true);
    Pose->CopyPoseFromSkeletalComponent(Source);
    const float SafeDelta = FMath::Clamp(DeltaTime, 1.0e-4f, 0.05f);
    const FVector ComponentVelocity =
        (Source->GetComponentLocation() - Station.PreviousComponentTransform.GetLocation()) /
        SafeDelta;
    Station.PreviousComponentTransform = Source->GetComponentTransform();

    // RE's secondary passes are low-frequency and strongly damped.  This is
    // a pose-space spring: the imported action is the target, while a bounded
    // gust and measured station motion create the soft cloth follow-through.
    const float GustX = FMath::Sin(TimeSeconds * 0.73f) * 0.65f;
    const float GustY = FMath::Cos(TimeSeconds * 0.51f) * 0.48f;
    const float GustZ = FMath::Sin(TimeSeconds * 0.37f + 0.9f) * 0.22f;
    const float MotionGain = FMath::Clamp(ComponentVelocity.Size() * 0.0025f, 0.0f, 0.50f);
    for (FSecondaryBoneState& State : Station.Bones)
    {
        const FTransform TargetTransform = Pose->GetBoneTransformByName(
            State.BoneName, EBoneSpaces::ComponentSpace);
        const FQuat TargetRotation = TargetTransform.GetRotation();
        const float Phase = TimeSeconds * (0.70f + State.Lag) + State.Phase;
        const FVector LocalAxis = FVector(
            GustX + FMath::Sin(Phase) * MotionGain,
            GustY + FMath::Cos(Phase * 0.83f) * MotionGain,
            GustZ + FMath::Sin(Phase * 0.61f) * MotionGain * 0.5f);
        const FQuat WindRotation = FQuat::MakeFromEuler(
            LocalAxis * State.AmplitudeDegrees);
        const FQuat DesiredRotation = (WindRotation * TargetRotation).GetNormalized();
        const float Follow = 1.0f - FMath::Exp(-SafeDelta / FMath::Max(State.Lag, 0.01f));
        State.SimulatedRotation = FQuat::Slerp(
            State.SimulatedRotation, DesiredRotation, Follow).GetNormalized();
        Pose->SetBoneRotationByName(State.BoneName,
            State.SimulatedRotation.Rotator(), EBoneSpaces::ComponentSpace);
    }
    // This solver runs after physics, later than PoseableMesh's default
    // component tick. Publish the corrected skinning transforms now so the
    // renderer sees this frame's action and secondary motion immediately.
    Pose->RefreshBoneTransforms();
}

void ULeonVariantDynamicsComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bDiscovered)
    {
        DiscoverStations();
    }
    const float TimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    for (FStationState& Station : Stations)
    {
        SimulateStation(Station, DeltaTime, TimeSeconds);
    }
}
