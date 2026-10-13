#pragma once

#include "CoreMinimal.h"

class UGroomComponent;

/**
 * Small, engine independent pieces of the reconstructed RE9 hair path.
 *
 * The shipped game stores the dense strand curves in STRD/SBD resources and
 * drives them from a compact chain2 pose solver.  UE's Groom solver remains
 * responsible for rendering the imported curves; this helper supplies the
 * same two high level stages that are missing from the stock UE preview:
 * four point cubic interpolation and a damped, collision constrained chain.
 */
namespace LeonREHairPipeline
{
    struct FChainState
    {
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        FVector RestOffset = FVector::ZeroVector;
        float Length = 0.0f;
        float Stiffness = 0.82f;
        float Damping = 0.72f;
        // Hair strands are effectively unit-mass particles.  Keeping the
        // inverse mass explicit lets the XPBD projection pin the scalp while
        // still allowing the tail to yield under wind and inertia.
        float InverseMass = 1.0f;
    };

    /** Catmull-Rom interpolation used as the RE four-control-point fallback. */
    LEONPREVIEWRUNTIME_API FVector InterpolateFourControlPoints(
        const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3,
        float Alpha);

    /**
     * Advance a short hair/cloth style chain.  Pinned[0] is the scalp/root;
     * the remaining points are integrated with gravity, spring and damping,
     * then projected back to their rest lengths.  The function is deliberately
     * deterministic so the same pose is reproducible in editor and game.
     */
    LEONPREVIEWRUNTIME_API void SimulateChain(
        TArray<FChainState>& Chain,
        const FVector& RootPosition,
        const FQuat& RootRotation,
        float DeltaSeconds,
        const FVector& Gravity,
        float GlobalStiffness,
        float GlobalDamping,
        int32 ConstraintIterations,
        float CollisionRadius,
        int32 Substeps = 3,
        float ConstraintCompliance = 0.00015f,
        const FVector& Wind = FVector::ZeroVector,
        float CollisionFriction = 0.8f);

    /** A scalar describing chain motion, useful for feeding Groom velocity. */
    LEONPREVIEWRUNTIME_API float ComputeChainEnergy(const TArray<FChainState>& Chain);

    /**
     * Return the depth-weighted transport velocity of the proxy guides.
     *
     * The compact chain is not itself the 20k-curve render resource.  This
     * velocity is the bridge into UE HairStrands' dense solver: callers feed
     * it as AirVelocity in the simulation's local frame so drag advects every
     * dense strand while preserving the guide phase lag.
     */
    LEONPREVIEWRUNTIME_API FVector ComputeChainTransportVelocity(
        const TArray<FChainState>& Chain);

    /** Return a normalized root-to-tip direction for diagnostics and shaping. */
    LEONPREVIEWRUNTIME_API FVector ComputeChainTipDirection(
        const TArray<FChainState>& Chain);

    /**
     * Queue a render-thread write into UE 5.8's Groom guide DeformerBuffer.
     *
     * The five compact controls are evaluated as four overlapping
     * four-control-point Catmull-Rom segments for every imported guide point.
     * HairStrands/GroomManager then consumes that UAV and performs its normal
     * guide-to-dense interpolation.  The call is non-blocking and returns
     * false when the selected Groom group has no allocated guide resources.
     */
    LEONPREVIEWRUNTIME_API bool EnqueueGuidePositionWrite(
        UGroomComponent* GroomComponent,
        uint32 GroupIndex,
        TConstArrayView<FVector4f> CurrentControlPoints,
        TConstArrayView<FVector4f> RestControlPoints,
        float CurrentTimeSeconds);
}
