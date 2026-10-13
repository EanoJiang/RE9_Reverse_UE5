#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LeonREHairGuideBufferComponent.generated.h"

/**
 * Game-thread staging buffer for the RE guide chain.  Optimus creates its
 * render proxy on the game thread, so this component is intentionally a
 * small POD-like bridge: the render proxy copies the chain once per frame
 * and uploads it to a structured SRV before the groom guide write pass.
 */
UCLASS(ClassGroup=(Leon), meta=(BlueprintSpawnableComponent))
class LEONPREVIEWRUNTIME_API ULeonREHairGuideBufferComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	void SetGuidePoints(TConstArrayView<FVector> InPoints);
	void ResetGuidePoints();
	void Snapshot(TArray<FVector4f>& OutPoints) const;
	int32 NumGuidePoints() const { return GuidePoints.Num(); }

private:
	UPROPERTY(Transient)
	TArray<FVector4f> GuidePoints;
};
