#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LeonGroomReadbackLibrary.generated.h"

class UGroomComponent;

/** GPU geometry inspection for native hair, independent of mesh bounds. */
UCLASS()
class LEONPREVIEWTOOLS_API ULeonGroomReadbackLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Request actual deformed render or guide positions; poll after frames render. */
    UFUNCTION(BlueprintCallable, Category="Leon|Groom|Validation")
    static bool RequestNativeGroomPositions(UGroomComponent* Groom, bool bReadGuides);

    /** JSON state: pending/ready, actual curve/point counts, bounds and root samples. */
    UFUNCTION(BlueprintCallable, Category="Leon|Groom|Validation")
    static FString ReadNativeGroomPositionSummary(UGroomComponent* Groom);
};
