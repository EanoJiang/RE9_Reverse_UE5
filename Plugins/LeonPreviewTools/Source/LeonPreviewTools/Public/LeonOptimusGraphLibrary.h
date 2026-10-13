#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "LeonOptimusGraphLibrary.generated.h"

/** Editor-only helpers for creating the RE hair Optimus graph. */
UCLASS()
class LEONPREVIEWTOOLS_API ULeonOptimusGraphLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Create or inspect a GroomGuidesRead/Write + Scene + custom kernel graph. */
    UFUNCTION(BlueprintCallable, Category="Leon|Optimus")
    static FString BuildLeonREHairOptimusGraph(const FString& InAssetPath, bool bForceRebuild = false);
};
