#include "LeonREHairGuideBufferComponent.h"

void ULeonREHairGuideBufferComponent::SetGuidePoints(TConstArrayView<FVector> InPoints)
{
	GuidePoints.Reset(InPoints.Num());
	for (const FVector& P : InPoints)
	{
		GuidePoints.Add(FVector4f((float)P.X, (float)P.Y, (float)P.Z, 1.0f));
	}
}

void ULeonREHairGuideBufferComponent::ResetGuidePoints()
{
	GuidePoints.Reset();
}

void ULeonREHairGuideBufferComponent::Snapshot(TArray<FVector4f>& OutPoints) const
{
	OutPoints = GuidePoints;
}
