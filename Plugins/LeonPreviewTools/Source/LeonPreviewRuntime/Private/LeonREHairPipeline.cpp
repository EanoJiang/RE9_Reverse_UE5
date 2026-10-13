#include "LeonREHairPipeline.h"
#include "HairStrandsInterface.h"

#include "GlobalShader.h"
#include "GroomComponent.h"
#include "GroomInstance.h"
#include "GroomResources.h"
#include "HairStrandsDatas.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderResource.h"
#include "RHICommandList.h"
#include "ShaderParameterUtils.h"

namespace
{
    class FLeonREHairGuideWriteCS : public FGlobalShader
    {
        DECLARE_GLOBAL_SHADER(FLeonREHairGuideWriteCS);
        SHADER_USE_PARAMETER_STRUCT(FLeonREHairGuideWriteCS, FGlobalShader);

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER(uint32, PointCount)
            SHADER_PARAMETER(uint32, RegisteredIndex)
            SHADER_PARAMETER(FVector3f, RestPositionOffset)
            SHADER_PARAMETER(float, CurrentTime)
            SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, PointRestPositions)
            SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, PointToCurveBuffer)
            SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, CurveBuffer)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, CurrentControlPoints)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, RestControlPoints)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, DeformedPositionOffset)
            SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, PointDeformedPositions)
        END_SHADER_PARAMETER_STRUCT()

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
        }
    };

    IMPLEMENT_GLOBAL_SHADER(
        FLeonREHairGuideWriteCS,
        "/Project/LeonREHairGuideWrite.usf",
        "MainCS",
        SF_Compute);
}

namespace LeonREHairPipeline
{
    FVector InterpolateFourControlPoints(const FVector& P0, const FVector& P1,
        const FVector& P2, const FVector& P3, float Alpha)
    {
        // Uniform Catmull-Rom.  The game uses four samples per projected
        // segment; this keeps C1 continuity while preserving the end samples.
        const float T = FMath::Clamp(Alpha, 0.0f, 1.0f);
        const float T2 = T * T;
        const float T3 = T2 * T;
        return 0.5f * ((2.0f * P1) + (-P0 + P2) * T +
            (2.0f * P0 - 5.0f * P1 + 4.0f * P2 - P3) * T2 +
            (-P0 + 3.0f * P1 - 3.0f * P2 + P3) * T3);
    }

    void SimulateChain(TArray<FChainState>& Chain, const FVector& RootPosition,
        const FQuat& RootRotation, float DeltaSeconds, const FVector& Gravity,
        float GlobalStiffness, float GlobalDamping, int32 ConstraintIterations,
        float CollisionRadius, int32 Substeps, float ConstraintCompliance,
        const FVector& Wind, float CollisionFriction)
    {
        if (Chain.Num() == 0) return;
        const float Dt = FMath::Clamp(DeltaSeconds, 1.0e-4f, 1.0f / 20.0f);
        const float Stiffness = FMath::Clamp(GlobalStiffness, 0.0f, 1.0f);
        const float Damping = FMath::Clamp(GlobalDamping, 0.0f, 1.0f);
        const int32 Iterations = FMath::Clamp(ConstraintIterations, 1, 16);
        const int32 Steps = FMath::Clamp(Substeps, 1, 8);
        const float StepDt = Dt / float(Steps);
        // XPBD compliance is expressed in cm/N.  Clamp it to a small positive
        // value so zero-length links cannot produce an infinite correction.
        const float Alpha = FMath::Max(ConstraintCompliance, 1.0e-7f) /
            FMath::Max(FMath::Square(StepDt), 1.0e-8f);
        const float Friction = FMath::Clamp(CollisionFriction, 0.0f, 1.0f);

        // Keep the previous state for a velocity reconstruction after all
        // substeps.  This gives the Groom Niagara stage a smooth impulse even
        // when the editor only ticks the controller at 30 Hz.
        TArray<FVector> FrameStartPositions;
        FrameStartPositions.SetNum(Chain.Num());
        for (int32 Index = 0; Index < Chain.Num(); ++Index)
            FrameStartPositions[Index] = Chain[Index].Position;

        const TArray<FVector> RestTargets = [&]()
        {
            TArray<FVector> Targets;
            Targets.SetNum(Chain.Num());
            for (int32 Index = 0; Index < Chain.Num(); ++Index)
                Targets[Index] = RootPosition + RootRotation.RotateVector(Chain[Index].RestOffset);
            return Targets;
        }();

        // XPBD multipliers are warm-started across substeps of this frame.
        // Reinitialising them inside the loop makes every substep behave like
        // an independent hard projection and removes the phase lag that gives
        // the outer guides their follow-through.
        TArray<float> Lambdas;
        Lambdas.Init(0.0f, FMath::Max(0, Chain.Num() - 1));
        TArray<FVector> SubstepStartPositions;
        SubstepStartPositions.SetNum(Chain.Num());

        for (int32 Step = 0; Step < Steps; ++Step)
        {
            // A velocity reconstructed from the entire frame start position
            // (the old implementation) averages away motion when there are
            // multiple substeps.  Keep the actual state at the beginning of
            // this substep and reconstruct immediately after projection so
            // the next substep integrates the corrected velocity.
            for (int32 Index = 0; Index < Chain.Num(); ++Index)
                SubstepStartPositions[Index] = Chain[Index].Position;

            // Integrate predicted positions. Wind is deliberately applied as
            // an acceleration rather than a direct offset, matching the RE
            // chain2 motion-force path and keeping gusts frame-rate stable.
            Chain[0].Position = RootPosition;
            Chain[0].Velocity = FVector::ZeroVector;
            for (int32 Index = 1; Index < Chain.Num(); ++Index)
            {
                FChainState& Point = Chain[Index];
                const float EffectiveDamping = FMath::Clamp(Point.Damping * Damping, 0.0f, 1.0f);
                const FVector Spring = (RestTargets[Index] - Point.Position) *
                    (Point.Stiffness * Stiffness);
                const FVector Acceleration = Gravity + Wind + Spring;
                Point.Velocity += Acceleration * StepDt;
                // Exponential damping avoids the frame-rate dependent Pow()
                // behavior of the old single-step approximation.
                Point.Velocity *= FMath::Exp(-FMath::Max(0.01f, EffectiveDamping) * StepDt * 8.0f);
                Point.Position += Point.Velocity * StepDt;
            }

            // XPBD distance constraints. Lambda is retained for all iterations
            // and all substeps in this frame; unlike a hard projection this
            // permits a tiny, tunable stretch while remaining stable at high
            // angular velocity.
            for (int32 Iter = 0; Iter < Iterations; ++Iter)
            {
                Chain[0].Position = RootPosition;
                for (int32 Index = 1; Index < Chain.Num(); ++Index)
                {
                    FChainState& Parent = Chain[Index - 1];
                    FChainState& Point = Chain[Index];
                    FVector Delta = Point.Position - Parent.Position;
                    const float Distance = Delta.Size();
                    const FVector Direction = Distance > KINDA_SMALL_NUMBER
                        ? Delta / Distance : RootRotation.RotateVector(FVector::UpVector);
                    const float RestLength = FMath::Max(Point.Length, 0.01f);
                    const float Constraint = Distance - RestLength;
                    const float ParentW = (Index == 1) ? 0.0f : FMath::Max(0.0f, Parent.InverseMass);
                    const float PointW = FMath::Max(0.0f, Point.InverseMass);
                    const float Weight = ParentW + PointW;
                    if (Weight > KINDA_SMALL_NUMBER)
                    {
                        const float DeltaLambda = (-Constraint - Alpha * Lambdas[Index - 1]) /
                            (Weight + Alpha);
                        Lambdas[Index - 1] += DeltaLambda;
                        if (PointW > 0.0f) Point.Position += Direction * (DeltaLambda * PointW);
                        if (ParentW > 0.0f) Parent.Position -= Direction * (DeltaLambda * ParentW);
                    }

                    // Head_0 is represented by a soft spherical collision
                    // volume.  Scale the radius by chain depth so the root
                    // remains pinned while the outer hair clears the scalp.
                    if (CollisionRadius > 0.0f && Index > 0)
                    {
                        const FVector Local = Point.Position - RootPosition;
                        const float DepthAlpha = float(Index) /
                            float(FMath::Max(1, Chain.Num() - 1));
                        const float MinDistance = CollisionRadius * FMath::Lerp(0.20f, 1.0f, DepthAlpha);
                        const float DistSq = Local.SizeSquared();
                        if (DistSq < FMath::Square(MinDistance))
                        {
                            const float Dist = FMath::Sqrt(FMath::Max(DistSq, 1.0e-6f));
                            const FVector Normal = Dist > KINDA_SMALL_NUMBER
                                ? Local / Dist : RootRotation.RotateVector(FVector::UpVector);
                            Point.Position = RootPosition + Normal * MinDistance;
                            // Remove the inward velocity component and apply
                            // tangential friction, as the RE collision solver
                            // does after its XPBD positional correction.
                            const float Inward = FVector::DotProduct(Point.Velocity, Normal);
                            if (Inward < 0.0f) Point.Velocity -= Normal * Inward;
                            Point.Velocity *= FMath::Lerp(1.0f, 1.0f - Friction, 0.35f);
                        }
                    }
                }
            }

            // Reconstruct velocity at the substep boundary.  This preserves
            // the constraint correction and carries it into the next step.
            for (int32 Index = 1; Index < Chain.Num(); ++Index)
                Chain[Index].Velocity = (Chain[Index].Position - SubstepStartPositions[Index]) /
                    FMath::Max(StepDt, 1.0e-4f);
        }

        // RE's guide stage evaluates a four-sample spline after the physical
        // projection.  Apply the same C1 smoothing to the compact proxy chain
        // and then re-project lengths once.  The actual dense STRD curves are
        // still owned by Groom/Niagara, but this makes the guide energy that is
        // fed into Groom come from a genuinely interpolated four-point path.
        if (Chain.Num() >= 4)
        {
            TArray<FVector> Smoothed;
            Smoothed.SetNum(Chain.Num());
            for (int32 Index = 0; Index < Chain.Num(); ++Index)
                Smoothed[Index] = Chain[Index].Position;
            for (int32 Index = 1; Index < Chain.Num() - 1; ++Index)
            {
                const int32 P0 = FMath::Max(0, Index - 1);
                const int32 P1 = Index;
                const int32 P2 = FMath::Min(Chain.Num() - 1, Index + 1);
                const int32 P3 = FMath::Min(Chain.Num() - 1, Index + 2);
                const FVector Curve = InterpolateFourControlPoints(
                    Chain[P0].Position, Chain[P1].Position,
                    Chain[P2].Position, Chain[P3].Position, 0.5f);
                Smoothed[Index] = FMath::Lerp(Chain[Index].Position, Curve, 0.18f);
            }
            Chain[0].Position = RootPosition;
            for (int32 Index = 1; Index < Chain.Num(); ++Index)
            {
                const FVector Delta = Smoothed[Index] - Chain[Index - 1].Position;
                const float Distance = Delta.Size();
                const FVector Direction = Distance > KINDA_SMALL_NUMBER
                    ? Delta / Distance : RootRotation.RotateVector(FVector::UpVector);
                Chain[Index].Position = Chain[Index - 1].Position +
                    Direction * FMath::Max(Chain[Index].Length, 0.01f);
            }
        }

        Chain[0].Position = RootPosition;
        Chain[0].Velocity = FVector::ZeroVector;
        for (int32 Index = 1; Index < Chain.Num(); ++Index)
            Chain[Index].Velocity = (Chain[Index].Position - FrameStartPositions[Index]) / Dt;
    }

    float ComputeChainEnergy(const TArray<FChainState>& Chain)
    {
        float Energy = 0.0f;
        for (int32 Index = 1; Index < Chain.Num(); ++Index)
            Energy += Chain[Index].Velocity.SizeSquared() * (0.5f + 0.5f * float(Index) / FMath::Max(1, Chain.Num() - 1));
        return FMath::Sqrt(FMath::Max(0.0f, Energy));
    }

    FVector ComputeChainTransportVelocity(const TArray<FChainState>& Chain)
    {
        if (Chain.Num() <= 1)
            return FVector::ZeroVector;

        // Weight the outer guides more heavily: the root follows Head_0 and
        // should not cancel the useful phase lag that gives the tail its
        // follow-through.  Keep the result bounded before it reaches the
        // dense solver's air-drag path; this is a transport velocity, not an
        // unconstrained impulse.
        FVector WeightedVelocity = FVector::ZeroVector;
        float WeightSum = 0.0f;
        for (int32 Index = 1; Index < Chain.Num(); ++Index)
        {
            const float Alpha = float(Index) / float(FMath::Max(1, Chain.Num() - 1));
            const float Weight = FMath::Lerp(0.45f, 1.0f, Alpha * Alpha);
            if (Chain[Index].Velocity.ContainsNaN())
                continue;
            WeightedVelocity += Chain[Index].Velocity * Weight;
            WeightSum += Weight;
        }
        if (WeightSum <= KINDA_SMALL_NUMBER)
            return FVector::ZeroVector;

        FVector Result = WeightedVelocity / WeightSum;
        constexpr float MaxTransportSpeed = 240.0f; // cm/s
        const float Speed = Result.Size();
        if (Speed > MaxTransportSpeed)
            Result *= MaxTransportSpeed / Speed;
        return Result;
    }

    FVector ComputeChainTipDirection(const TArray<FChainState>& Chain)
    {
        if (Chain.Num() <= 1)
            return FVector::UpVector;
        const FVector Delta = Chain.Last().Position - Chain[0].Position;
        return Delta.IsNearlyZero() ? FVector::UpVector : Delta.GetSafeNormal();
    }

    bool EnqueueGuidePositionWrite(
        UGroomComponent* GroomComponent,
        uint32 GroupIndex,
        TConstArrayView<FVector4f> CurrentControlPoints,
        TConstArrayView<FVector4f> RestControlPoints,
        float CurrentTimeSeconds)
    {
        static bool bReportedFailure = false;
        auto ReportFailure = [&](const TCHAR* Reason)
        {
            if (!bReportedFailure)
            {
                bReportedFailure = true;
                UE_LOG(LogTemp, Warning, TEXT("[LeonREHair] guide direct write unavailable: %s"), Reason);
            }
        };
        if (!GroomComponent)
        {
            ReportFailure(TEXT("GroomComponent=null"));
            return false;
        }
        if (CurrentControlPoints.Num() < 5 || RestControlPoints.Num() < 5)
        {
            ReportFailure(TEXT("compact controls < 5"));
            return false;
        }

        FHairStrandsRestResource* RestResource =
            GroomComponent->GetGuideStrandsRestResource(GroupIndex);
        FHairStrandsDeformedResource* DeformedResource =
            GroomComponent->GetGuideStrandsDeformedResource(GroupIndex);
        const FHairGroupInstance* GroupInstance = GroomComponent->GetGroupInstance(static_cast<int32>(GroupIndex));
        if (!RestResource)
        {
            ReportFailure(TEXT("guide rest resource=null"));
            return false;
        }
        if (!DeformedResource)
        {
            ReportFailure(TEXT("guide deformed resource=null"));
            return false;
        }
        if (!GroupInstance)
        {
            ReportFailure(TEXT("group instance=null"));
            return false;
        }
        // Groom allocates the RHI buffers on the render thread.  Do not claim
        // ownership of simulation until the guide position buffer is already
        // initialized; otherwise the first frame can disable Niagara while
        // the queued UAV pass has nothing to consume.
        if (!RestResource->PositionBuffer.Buffer)
        {
            ReportFailure(TEXT("guide position buffer=null"));
            return false;
        }

        const uint32 PointCount = RestResource->GetPointCount();
        if (PointCount == 0)
        {
            ReportFailure(TEXT("guide point count=0"));
            return false;
        }

        // Copy the compact controls into the render command.  The game-thread
        // chain is reused on the next tick, so retaining a view here would
        // race with the simulation and make the GPU read stale positions.
        TArray<FVector4f> CurrentControls;
        TArray<FVector4f> RestControls;
        CurrentControls.Append(CurrentControlPoints.GetData(), 5);
        RestControls.Append(RestControlPoints.GetData(), 5);
        const uint32 RegisteredIndex = GroupInstance->RegisteredIndex;

        ENQUEUE_RENDER_COMMAND(LeonREHairGuidePositionWrite)(
            [RestResource, DeformedResource, PointCount, RegisteredIndex, CurrentTimeSeconds,
                CurrentControls = MoveTemp(CurrentControls),
                RestControls = MoveTemp(RestControls)](FRHICommandListImmediate& RHICmdList) mutable
            {
                if (!RestResource || !DeformedResource || !RestResource->PositionBuffer.Buffer)
                    return;

                FRDGBuilder GraphBuilder(RHICmdList);

                FRDGBufferRef CurrentControlBuffer = GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 5),
                    TEXT("Leon.REHair.CurrentCompactControls"));
                FRDGBufferRef RestControlBuffer = GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 5),
                    TEXT("Leon.REHair.RestCompactControls"));
                GraphBuilder.QueueBufferUpload(
                    CurrentControlBuffer, CurrentControls.GetData(),
                    sizeof(FVector4f) * CurrentControls.Num(), ERDGInitialDataFlags::None);
                GraphBuilder.QueueBufferUpload(
                    RestControlBuffer, RestControls.GetData(),
                    sizeof(FVector4f) * RestControls.Num(), ERDGInitialDataFlags::None);

                const FRDGBufferSRVRef RestPositions =
                    RegisterAsSRV(GraphBuilder, RestResource->PositionBuffer);
                const FRDGBufferSRVRef PointToCurve =
                    RegisterAsSRV(GraphBuilder, RestResource->PointToCurveBuffer);
                const FRDGBufferSRVRef Curves =
                    RegisterAsSRV(GraphBuilder, RestResource->CurveBuffer);
                const FRDGBufferSRVRef CurrentControlsSRV = GraphBuilder.CreateSRV(CurrentControlBuffer);
                const FRDGBufferSRVRef RestControlsSRV = GraphBuilder.CreateSRV(RestControlBuffer);
                const FRDGBufferSRVRef DeformedOffset = RegisterAsSRV(
                    GraphBuilder,
                    DeformedResource->GetPositionOffsetBuffer(FHairStrandsDeformedResource::EFrameType::Current));
                const FRDGBufferUAVRef DeformedPositions = Register(
                    GraphBuilder,
                    DeformedResource->GetDeformerBuffer(GraphBuilder),
                    ERDGImportedBufferFlags::CreateUAV).UAV;

                if (!RestPositions || !PointToCurve || !Curves || !CurrentControlsSRV ||
                    !RestControlsSRV || !DeformedOffset || !DeformedPositions)
                {
                    GraphBuilder.Execute();
                    return;
                }

                FLeonREHairGuideWriteCS::FParameters* Parameters =
                    GraphBuilder.AllocParameters<FLeonREHairGuideWriteCS::FParameters>();
                Parameters->PointCount = PointCount;
                Parameters->RegisteredIndex = RegisteredIndex;
                Parameters->RestPositionOffset = (FVector3f)RestResource->GetPositionOffset();
                Parameters->CurrentTime = CurrentTimeSeconds;
                Parameters->PointRestPositions = RestPositions;
                Parameters->PointToCurveBuffer = PointToCurve;
                Parameters->CurveBuffer = Curves;
                Parameters->CurrentControlPoints = CurrentControlsSRV;
                Parameters->RestControlPoints = RestControlsSRV;
                Parameters->DeformedPositionOffset = DeformedOffset;
                Parameters->PointDeformedPositions = DeformedPositions;

                TShaderMapRef<FLeonREHairGuideWriteCS> ComputeShader(
                    GetGlobalShaderMap(GMaxRHIFeatureLevel));
                const uint32 GroupSize = 64;
                const uint32 DispatchCount = FMath::DivideAndRoundUp(PointCount, GroupSize);
                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("LeonREHair::WriteGuideDeformerBuffer"),
                    ERDGPassFlags::Compute,
                    ComputeShader,
                    Parameters,
                    FIntVector(DispatchCount, 1, 1));
                GraphBuilder.Execute();
            });

        return true;
    }
}
