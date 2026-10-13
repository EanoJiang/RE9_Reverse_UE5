#include "LeonGroomReadbackLibrary.h"
#include "GroomComponent.h"
#include "GroomBuilder.h"
#include "GroomInstance.h"
#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"
#include "CoreGlobals.h"

namespace LeonGroomReadback
{
static TSharedPtr<FStrandsPositionOutput> PendingOutput;
static TWeakObjectPtr<UGroomComponent> PendingComponent;
static bool bPendingGuides = false;
static FTransform PendingComponentToWorld;
static TSharedPtr<FJsonObject> PendingRequestDiagnostics;
static uint64 PendingRequestFrame = 0;
static FString Text(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Result));
    return Result;
}
static TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}
static TSharedRef<FJsonObject> RenderState(const UGroomComponent* Groom)
{
    auto State = MakeShared<FJsonObject>();
    State->SetBoolField(TEXT("component_valid"), Groom != nullptr);
    State->SetNumberField(TEXT("frame_counter"), double(GFrameCounter));
    if (!Groom) return State;
    State->SetStringField(TEXT("component"), Groom->GetPathName());
    State->SetBoolField(TEXT("component_registered"), Groom->IsRegistered());
    State->SetBoolField(TEXT("render_state_created"), Groom->IsRenderStateCreated());
    State->SetBoolField(TEXT("visible"), Groom->IsVisible());
    State->SetBoolField(TEXT("scene_proxy_present"), Groom->GetSceneProxy() != nullptr);
    State->SetBoolField(TEXT("world_scene_present"), Groom->GetWorld() && Groom->GetWorld()->Scene);
    State->SetStringField(TEXT("world"), Groom->GetWorld() ? Groom->GetWorld()->GetPathName() : TEXT(""));
    State->SetNumberField(TEXT("component_instance_count"), Groom->GetGroupCount());
    TArray<TSharedPtr<FJsonValue>> Groups;
    for (uint32 Index = 0; Index < Groom->GetGroupCount(); ++Index)
    {
        auto Group = MakeShared<FJsonObject>();
        const FHairGroupInstance* Instance = Groom->GetGroupInstance(Index);
        Group->SetNumberField(TEXT("group"), Index);
        Group->SetBoolField(TEXT("component_instance_present"), Instance != nullptr);
        if (Instance)
        {
            Group->SetBoolField(TEXT("instance_proxy_matches_component_proxy"), Instance->Debug.Proxy == Groom->GetSceneProxy());
            Group->SetNumberField(TEXT("geometry_type"), int32(Instance->GeometryType));
            Group->SetNumberField(TEXT("binding_type"), int32(Instance->BindingType));
            Group->SetBoolField(TEXT("public_data_present"), Instance->HairGroupPublicData != nullptr);
            Group->SetBoolField(TEXT("strands_rest_resource_present"), Instance->Strands.RestResource != nullptr);
            Group->SetBoolField(TEXT("strands_deformed_resource_present"), Instance->Strands.DeformedResource != nullptr);
            Group->SetBoolField(TEXT("strands_root_resources_present"), Instance->Strands.HasValidRootData());
            Group->SetBoolField(TEXT("guides_rest_resource_present"), Instance->Guides.RestResource != nullptr);
            Group->SetBoolField(TEXT("guides_deformed_resource_present"), Instance->Guides.DeformedResource != nullptr);
            if (Instance->Strands.IsValid())
                Group->SetNumberField(TEXT("rest_curve_count"), Instance->Strands.GetData().GetNumCurves());
            if (Instance->HairGroupPublicData)
            {
                Group->SetNumberField(TEXT("active_curve_count"), Instance->HairGroupPublicData->GetActiveStrandsCurveCount());
                Group->SetNumberField(TEXT("active_point_count"), Instance->HairGroupPublicData->GetActiveStrandsPointCount());
                Group->SetNumberField(TEXT("lod_index"), Instance->HairGroupPublicData->GetLODIndex());
                Group->SetBoolField(TEXT("lod_visible"), Instance->HairGroupPublicData->GetLODVisibility());
            }
        }
        Groups.Add(MakeShared<FJsonValueObject>(Group));
    }
    State->SetArrayField(TEXT("groups"), Groups);
    auto CVars = MakeShared<FJsonObject>();
    for (const TCHAR* Name : {TEXT("r.HairStrands.Enable"), TEXT("r.HairStrands.Strands"), TEXT("r.HairStrands.Binding"), TEXT("r.HairStrands.Simulation"), TEXT("r.HairStrands.ViewMode"), TEXT("r.HairStrands.AutoLOD.Bias")})
        if (const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
            CVars->SetStringField(Name, CVar->GetString());
    State->SetObjectField(TEXT("console_variables"), CVars);
    State->SetStringField(TEXT("measurement"), TEXT("Render resource metadata for diagnosis, not GPU position readback"));
    return State;
}
}

bool ULeonGroomReadbackLibrary::RequestNativeGroomPositions(UGroomComponent* Groom, bool bReadGuides)
{
    using namespace LeonGroomReadback;
    PendingRequestDiagnostics = RenderState(Groom);
    PendingRequestFrame = GFrameCounter;
    if (!Groom || !Groom->GroomAsset || !Groom->GetWorld() || !Groom->GetWorld()->Scene || Groom->GetGroupCount() == 0) return false;
    // Engine RequestStrandsPosition obtains its ref-counted group from the
    // scene proxy. Without a proxy its query is requeued forever.
    if (!Groom->GetSceneProxy())
    {
        PendingRequestDiagnostics->SetStringField(TEXT("request_rejection"), TEXT("Native Groom has no scene proxy"));
        return false;
    }
    // A previous request retains its shared output in the engine's readback queue.
    PendingOutput = MakeShared<FStrandsPositionOutput>();
    PendingComponent = Groom;
    PendingComponentToWorld = Groom->GetComponentTransform();
    bPendingGuides = bReadGuides;
    return RequestStrandsPosition(Groom, PendingOutput, bReadGuides);
}

FString ULeonGroomReadbackLibrary::ReadNativeGroomPositionSummary(UGroomComponent* Groom)
{
    using namespace LeonGroomReadback;
    auto Report = MakeShared<FJsonObject>();
    Report->SetObjectField(TEXT("render_state_now"), RenderState(Groom));
    if (PendingRequestDiagnostics.IsValid())
    {
        Report->SetObjectField(TEXT("render_state_at_request"), PendingRequestDiagnostics.ToSharedRef());
        Report->SetNumberField(TEXT("frames_since_request"), double(GFrameCounter - PendingRequestFrame));
    }
    const bool bRequested = Groom && PendingComponent.Get() == Groom && PendingOutput.IsValid();
    Report->SetBoolField(TEXT("requested"), bRequested);
    if (!bRequested) return Text(Report);
    Report->SetStringField(TEXT("representation"), bPendingGuides ? TEXT("guides") : TEXT("render_strands"));
    Report->SetNumberField(TEXT("remaining_groups"), PendingOutput->Status);
    Report->SetBoolField(TEXT("ready"), PendingOutput->IsValid());
    if (!PendingOutput->IsValid()) return Text(Report);
    const USkeletalMeshComponent* Body = Cast<USkeletalMeshComponent>(Groom->GetAttachParent());
    FTransform HeadWorld = Body ? Body->GetSocketTransform(TEXT("Head_0"), RTS_World) : FTransform::Identity;
    HeadWorld.SetScale3D(FVector::OneVector); // Distances in centimeters, independent of native root scale.
    Report->SetStringField(TEXT("coordinates"), TEXT("GPU deformed Groom local positions transformed by captured component transform, centimeters"));
    Report->SetBoolField(TEXT("head_space_available"), Body != nullptr);
    Report->SetArrayField(TEXT("head_origin_world_cm"), Vector(HeadWorld.GetTranslation()));

    TArray<TSharedPtr<FJsonValue>> Groups;
    int64 TotalCurves = 0, TotalPoints = 0, InvalidPoints = 0;
    for (int32 GroupIndex = 0; GroupIndex < PendingOutput->Groups.Num(); ++GroupIndex)
    {
        const FStrandsPositionOutput::FGroup& Data = PendingOutput->Groups[GroupIndex];
        auto Group = MakeShared<FJsonObject>();
        Group->SetNumberField(TEXT("group"), GroupIndex);
        Group->SetNumberField(TEXT("curves"), Data.Num());
        FBox Bounds(ForceInit), RootBounds(ForceInit);
        FVector RootSum = FVector::ZeroVector;
        int64 PointCount = 0, RootCount = 0;
        TArray<TSharedPtr<FJsonValue>> Samples;
        for (int32 CurveIndex = 0; CurveIndex < Data.Num(); ++CurveIndex)
        {
            const FStrandsPositionOutput::FStrand& Curve = Data[CurveIndex];
            for (const FVector3f& P : Curve)
            {
                const FVector World = PendingComponentToWorld.TransformPosition(FVector(P));
                if (World.ContainsNaN()) { ++InvalidPoints; continue; }
                Bounds += World;
                ++PointCount;
            }
            if (Curve.Num() == 0) continue;
            const FVector Root = PendingComponentToWorld.TransformPosition(FVector(Curve[0]));
            if (Root.ContainsNaN()) continue;
            RootBounds += Root;
            RootSum += Root;
            ++RootCount;
            if (CurveIndex % FMath::Max(1, Data.Num()/8) == 0 && Samples.Num() < 8)
            {
                auto Sample = MakeShared<FJsonObject>();
                Sample->SetNumberField(TEXT("curve"), CurveIndex);
                Sample->SetArrayField(TEXT("root_world_cm"), Vector(Root));
                Sample->SetArrayField(TEXT("root_head_space_cm"), Vector(HeadWorld.InverseTransformPosition(Root)));
                Sample->SetArrayField(TEXT("tip_world_cm"), Vector(PendingComponentToWorld.TransformPosition(FVector(Curve.Last()))));
                Samples.Add(MakeShared<FJsonValueObject>(Sample));
            }
        }
        Group->SetNumberField(TEXT("points"), PointCount);
        Group->SetNumberField(TEXT("root_count"), RootCount);
        if (Bounds.IsValid)
        {
            Group->SetArrayField(TEXT("bounds_min_world_cm"), Vector(Bounds.Min));
            Group->SetArrayField(TEXT("bounds_max_world_cm"), Vector(Bounds.Max));
        }
        if (RootCount > 0)
        {
            const FVector Center = RootSum / double(RootCount);
            Group->SetArrayField(TEXT("root_centroid_world_cm"), Vector(Center));
            Group->SetArrayField(TEXT("root_centroid_head_space_cm"), Vector(HeadWorld.InverseTransformPosition(Center)));
            Group->SetArrayField(TEXT("root_bounds_min_world_cm"), Vector(RootBounds.Min));
            Group->SetArrayField(TEXT("root_bounds_max_world_cm"), Vector(RootBounds.Max));
        }
        Group->SetArrayField(TEXT("curve_samples"), Samples);
        Groups.Add(MakeShared<FJsonValueObject>(Group));
        TotalCurves += Data.Num();
        TotalPoints += PointCount;
    }
    Report->SetArrayField(TEXT("groups"), Groups);
    Report->SetNumberField(TEXT("curves"), TotalCurves);
    Report->SetNumberField(TEXT("points"), TotalPoints);
    Report->SetNumberField(TEXT("invalid_points"), InvalidPoints);
    Report->SetBoolField(TEXT("success"), TotalCurves > 0 && TotalPoints > TotalCurves && InvalidPoints == 0);
    return Text(Report);
}
