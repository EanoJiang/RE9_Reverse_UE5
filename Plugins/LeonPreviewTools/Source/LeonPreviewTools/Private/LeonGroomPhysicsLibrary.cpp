#include "LeonGroomPhysicsLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "GroomComponent.h"
#include "GroomAsset.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace LeonGroomPhysics
{
static FString JsonText(const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
    return Text;
}
static TArray<TSharedPtr<FJsonValue>> VectorJSON(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}
static bool SaveAsset(UObject* Asset)
{
    if (!Asset) return false;
    UPackage* Package = Asset->GetOutermost();
    const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Package, Asset, *Filename, Args);
}
}

bool ULeonGroomPhysicsLibrary::EnsureGroomHeadCollision(USkeletalMesh* Mesh, FName HeadBone,
    FVector CenterInMeshCM, float RadiusCM, bool bSave, FString& OutReport)
{
    using namespace LeonGroomPhysics;
    auto Report = MakeShared<FJsonObject>();
    auto Fail = [&](const FString& Reason)
    {
        Report->SetBoolField(TEXT("success"), false);
        Report->SetStringField(TEXT("error"), Reason);
        OutReport = JsonText(Report);
        return false;
    };
    if (!Mesh || !Mesh->GetPhysicsAsset()) return Fail(TEXT("Target mesh or PhysicsAsset missing"));
    if (!FMath::IsFinite(RadiusCM) || RadiusCM <= 0.0f || RadiusCM > 30.0f || CenterInMeshCM.ContainsNaN())
        return Fail(TEXT("Invalid head collision center or radius in centimeters"));
    UPhysicsAsset* Physics = Mesh->GetPhysicsAsset();
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    const int32 BoneIndex = Ref.FindBoneIndex(HeadBone);
    if (BoneIndex == INDEX_NONE) return Fail(TEXT("Head bone absent from mesh reference skeleton"));
    FTransform BoneToMesh = Ref.GetRefBonePose()[BoneIndex];
    for (int32 Parent = Ref.GetParentIndex(BoneIndex); Parent != INDEX_NONE; Parent = Ref.GetParentIndex(Parent))
        BoneToMesh *= Ref.GetRefBonePose()[Parent];
    if (!BoneToMesh.IsValid() || BoneToMesh.GetScale3D().GetAbsMin() < SMALL_NUMBER)
        return Fail(TEXT("Invalid head reference transform"));

    USkeletalBodySetup* HeadBody = nullptr;
    for (USkeletalBodySetup* Existing : Physics->SkeletalBodySetups)
        if (Existing && Existing->BoneName == HeadBone) { HeadBody = Existing; break; }
    const bool bHasShape = HeadBody && HeadBody->AggGeom.GetElementCount() > 0;
    const int32 PriorBodyCount = Physics->SkeletalBodySetups.Num();
    const FVector LocalCenter = BoneToMesh.InverseTransformPosition(CenterInMeshCM);
    // Native import preserves a 100x scale on the root. Niagara's physics DI
    // sends ToMatrixWithScale to the GPU and leaves SphereElem.Radius in bone
    // units, so both center AND radius must account for inherited bone scale.
    const FVector ReferenceScale = BoneToMesh.GetScale3D().GetAbs();
    const float RadiusBoneUnits = RadiusCM / ReferenceScale.GetMax();
    if (!bHasShape)
    {
        Physics->Modify();
        if (!HeadBody)
        {
            HeadBody = NewObject<USkeletalBodySetup>(Physics, NAME_None, RF_Transactional);
            HeadBody->BoneName = HeadBone;
            Physics->SkeletalBodySetups.Add(HeadBody);
        }
        HeadBody->Modify();
        HeadBody->PhysicsType = PhysType_Kinematic;
        HeadBody->bConsiderForBounds = true;
        FKSphereElem Sphere;
        Sphere.Center = LocalCenter;
        Sphere.Radius = RadiusBoneUnits;
        HeadBody->AggGeom.SphereElems.Add(Sphere);
        HeadBody->InvalidatePhysicsData();
        HeadBody->CreatePhysicsMeshes();
        Physics->UpdateBodySetupIndexMap();
        Physics->UpdateBoundsBodiesArray();
        Physics->PostEditChange();
        Physics->RefreshPhysicsAssetChange();
        Physics->MarkPackageDirty();
    }
    const bool bSaved = !bSave || SaveAsset(Physics);
    Report->SetBoolField(TEXT("success"), bSaved && HeadBody && HeadBody->AggGeom.GetElementCount() > 0);
    Report->SetStringField(TEXT("physics_asset"), Physics->GetPathName());
    Report->SetStringField(TEXT("head_bone"), HeadBone.ToString());
    Report->SetBoolField(TEXT("added_shape"), !bHasShape);
    Report->SetBoolField(TEXT("existing_shapes_preserved"), true);
    Report->SetNumberField(TEXT("prior_body_count"), PriorBodyCount);
    Report->SetNumberField(TEXT("body_count"), Physics->SkeletalBodySetups.Num());
    Report->SetNumberField(TEXT("head_shape_count"), HeadBody->AggGeom.GetElementCount());
    Report->SetArrayField(TEXT("center_mesh_cm"), VectorJSON(CenterInMeshCM));
    Report->SetArrayField(TEXT("center_bone_cm"), VectorJSON(LocalCenter));
    Report->SetNumberField(TEXT("requested_radius_cm"), RadiusCM);
    Report->SetNumberField(TEXT("new_radius_bone_units"), RadiusBoneUnits);
    Report->SetArrayField(TEXT("reference_bone_scale"), VectorJSON(ReferenceScale));
    Report->SetArrayField(TEXT("new_radius_mesh_axes_cm"), VectorJSON(ReferenceScale * RadiusBoneUnits));
    Report->SetArrayField(TEXT("reconstructed_center_mesh_cm"), VectorJSON(BoneToMesh.TransformPosition(LocalCenter)));
    Report->SetBoolField(TEXT("saved"), bSave && bSaved);
    OutReport = JsonText(Report);
    return bSaved;
}

bool ULeonGroomPhysicsLibrary::SetupNativeGroomSimulation(UGroomComponent* Groom,
    USkeletalMeshComponent* Body, FName HeadBone, bool bSaveGroom, FString& OutReport)
{
    using namespace LeonGroomPhysics;
    if (!Groom || !Groom->GroomAsset || !Body || !Body->GetSkeletalMeshAsset() || !Body->GetPhysicsAsset())
    {
        OutReport = TEXT("{\"success\":false,\"error\":\"Groom, bound body or PhysicsAsset missing\"}");
        return false;
    }
    Groom->Modify();
    Groom->SimulationSettings.bOverrideSettings = true;
    Groom->SimulationSettings.SolverSettings.bEnableSimulation = false;
    Groom->SimulationSettings.SimulationSetup.bLocalSimulation = true;
    Groom->SimulationSettings.SimulationSetup.LocalBone = HeadBone.ToString();
    Groom->SimulationSettings.SimulationSetup.LinearVelocityScale = 0.05f;
    Groom->SimulationSettings.SimulationSetup.AngularVelocityScale = 0.08f;
    Groom->SimulationSettings.SimulationSetup.TeleportDistance = 500.0f;
    Groom->SimulationSettings.ExternalForces.AirDrag = 0.55f;
    Groom->SimulationSettings.MaterialConstraints.BendDamping = 0.20f;
    Groom->SimulationSettings.MaterialConstraints.BendStiffness = 0.020f;
    Groom->SimulationSettings.MaterialConstraints.StretchDamping = 0.10f;
    Groom->SimulationSettings.MaterialConstraints.StretchStiffness = 0.80f;
    Groom->SimulationSettings.MaterialConstraints.StaticFriction = 0.2f;
    Groom->SimulationSettings.MaterialConstraints.KineticFriction = 0.15f;
    Groom->SimulationSettings.MaterialConstraints.StrandsViscosity = 0.80f;
    Groom->SimulationSettings.MaterialConstraints.CollisionRadius = 0.010f;
    Groom->SetPhysicsAsset(Body->GetPhysicsAsset());
    // Niagara already finds Body by walking Groom's attachment hierarchy.
    // Adding that same body as an extra collision component duplicates every
    // shape in BuildAndCollect/ExtractSourceComponent.
    Groom->ResetCollisionComponents();
    UGroomAsset* Asset = Groom->GroomAsset;
    Asset->Modify();
    TArray<FHairGroupsPhysics> Groups = Asset->GetHairGroupsPhysics();
    for (FHairGroupsPhysics& Group : Groups)
    {
        Group.SolverSettings.EnableSimulation = true;
        Group.SolverSettings.SubSteps = 8;
        Group.SolverSettings.IterationCount = 12;
    }
    Asset->SetHairGroupsPhysics(Groups);
    Asset->PostEditChange();
    Asset->MarkPackageDirty();
    Groom->SetEnableSimulation(false);
    Groom->ResetSimulation();
    const bool bSaved = !bSaveGroom || SaveAsset(Asset);
    auto Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("success"), bSaved);
    Report->SetBoolField(TEXT("asset_solver_capability"), Asset->IsSimulationEnable());
    Report->SetBoolField(TEXT("component_override"), Groom->SimulationSettings.bOverrideSettings);
    Report->SetBoolField(TEXT("component_default_enabled"), Groom->SimulationSettings.SolverSettings.bEnableSimulation);
    Report->SetStringField(TEXT("local_bone"), Groom->SimulationSettings.SimulationSetup.LocalBone);
    Report->SetNumberField(TEXT("collision_components"), Groom->CollisionComponents.Num());
    Report->SetStringField(TEXT("collision_source"), TEXT("Attached skeletal mesh with Groom PhysicsAsset override"));
    Report->SetStringField(TEXT("physics_asset"), Groom->PhysicsAsset->GetPathName());
    Report->SetBoolField(TEXT("map_saved"), false);
    Report->SetBoolField(TEXT("groom_saved"), bSaveGroom && bSaved);
    OutReport = JsonText(Report);
    return bSaved;
}

FString ULeonGroomPhysicsLibrary::InspectNativeGroomSimulation(UGroomComponent* Groom)
{
    using namespace LeonGroomPhysics;
    auto Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("valid"), Groom && Groom->GroomAsset);
    if (!Groom || !Groom->GroomAsset) return JsonText(Report);
    Report->SetBoolField(TEXT("asset_solver_capability"), Groom->GroomAsset->IsSimulationEnable());
    Report->SetBoolField(TEXT("component_override"), Groom->SimulationSettings.bOverrideSettings);
    Report->SetBoolField(TEXT("component_solver_flag"), Groom->SimulationSettings.SolverSettings.bEnableSimulation);
    Report->SetStringField(TEXT("local_bone"), Groom->SimulationSettings.SimulationSetup.LocalBone);
    Report->SetNumberField(TEXT("collision_components"), Groom->CollisionComponents.Num());
    Report->SetStringField(TEXT("physics_asset"), Groom->PhysicsAsset ? Groom->PhysicsAsset->GetPathName() : TEXT(""));
    TArray<TSharedPtr<FJsonValue>> Groups;
    for (int32 GroupIndex = 0; GroupIndex < Groom->GroomAsset->GetHairGroupsPhysics().Num(); ++GroupIndex)
    {
        auto Group = MakeShared<FJsonObject>();
        Group->SetNumberField(TEXT("group"), GroupIndex);
        Group->SetBoolField(TEXT("effective_lod0_simulation"), Groom->IsSimulationEnable(GroupIndex, 0));
        Group->SetBoolField(TEXT("niagara_component_present"), Groom->GetNiagaraComponent(GroupIndex) != nullptr);
        Groups.Add(MakeShared<FJsonValueObject>(Group));
    }
    Report->SetArrayField(TEXT("groups"), Groups);
    USkeletalMeshComponent* Body = Cast<USkeletalMeshComponent>(Groom->GetAttachParent());
    if (!Body && Groom->CollisionComponents.Num() > 0) Body = Groom->CollisionComponents[0].Get();
    TArray<TSharedPtr<FJsonValue>> Shapes;
    if (Body && Body->GetSkeletalMeshAsset() && Groom->PhysicsAsset)
    {
        const FReferenceSkeleton& Ref = Body->GetSkeletalMeshAsset()->GetRefSkeleton();
        for (const USkeletalBodySetup* Setup : Groom->PhysicsAsset->SkeletalBodySetups)
        {
            if (!Setup) continue;
            const int32 BoneIndex = Ref.FindBoneIndex(Setup->BoneName);
            if (BoneIndex == INDEX_NONE) continue;
            const FTransform BoneToWorld = Body->GetBoneTransform(BoneIndex);
            const FVector Scale = BoneToWorld.GetScale3D().GetAbs();
            for (const FKSphereElem& Sphere : Setup->AggGeom.SphereElems)
            {
                auto Shape = MakeShared<FJsonObject>();
                Shape->SetStringField(TEXT("bone"), Setup->BoneName.ToString());
                Shape->SetStringField(TEXT("type"), TEXT("sphere"));
                Shape->SetArrayField(TEXT("center_world_cm"), VectorJSON(BoneToWorld.TransformPosition(Sphere.Center)));
                Shape->SetArrayField(TEXT("radius_world_axes_cm"), VectorJSON(Scale * Sphere.Radius));
                Shape->SetNumberField(TEXT("radius_bone_units"), Sphere.Radius);
                Shapes.Add(MakeShared<FJsonValueObject>(Shape));
            }
            for (const FKSphylElem& Capsule : Setup->AggGeom.SphylElems)
            {
                auto Shape = MakeShared<FJsonObject>();
                Shape->SetStringField(TEXT("bone"), Setup->BoneName.ToString());
                Shape->SetStringField(TEXT("type"), TEXT("capsule"));
                Shape->SetArrayField(TEXT("center_world_cm"), VectorJSON(BoneToWorld.TransformPosition(Capsule.Center)));
                Shape->SetArrayField(TEXT("radius_world_axes_cm"), VectorJSON(Scale * Capsule.Radius));
                Shape->SetNumberField(TEXT("length_bone_units"), Capsule.Length);
                Shapes.Add(MakeShared<FJsonValueObject>(Shape));
            }
        }
    }
    Report->SetArrayField(TEXT("collision_shapes_world"), Shapes);
    return JsonText(Report);
}
