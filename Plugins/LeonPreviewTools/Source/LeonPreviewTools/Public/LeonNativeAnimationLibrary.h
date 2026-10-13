#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LeonNativeAnimationLibrary.generated.h"

class UAnimSequence;
class USkeleton;
class USkeletalMesh;

/** Editor automation for preserved RE native motion data. */
UCLASS()
class LEONPREVIEWTOOLS_API ULeonNativeAnimationLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Import one GLB as sparse, basis-corrected bone tracks and raw scalar curves.
     * Scalar facial carrier nodes remain in the reference pose; their unscaled
     * translation.X payload is stored as curves and morph-name aliases.
     */
    UFUNCTION(BlueprintCallable, Category="Leon|Native Animation")
    static UAnimSequence* ImportNativeGLB(const FString& SourceFilename, const FString& DestinationPath,
        const FString& AssetName, USkeleton* TargetSkeleton, USkeletalMesh* PreviewMesh,
        int32 SampleRate, bool bSave, FString& OutReport);

    /** Assign an existing union skeleton without changing mesh bone references. */
    UFUNCTION(BlueprintCallable, Category="Leon|Native Animation")
    static bool AssignMasterSkeleton(USkeletalMesh* Mesh, USkeleton* MasterSkeleton, bool bSave, FString& OutReport);

    /** Register restored mesh morph names as morph-driving curve metadata once.
     * Run this in the editor owning the shared skeleton before headless batches.
     */
    UFUNCTION(BlueprintCallable, Category="Leon|Native Animation")
    static bool PrepareMorphCurveMetadata(USkeletalMesh* Mesh, USkeleton* TargetSkeleton, bool bSave, FString& OutReport);
};
