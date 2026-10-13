#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LeonGroomPhysicsLibrary.generated.h"

class USkeletalMesh;
class USkeletalMeshComponent;
class UGroomComponent;

/** Editor helpers for native Groom collision and inspectable solver state. */
UCLASS()
class LEONPREVIEWTOOLS_API ULeonGroomPhysicsLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Add a kinematic sphere on HeadBone only when that bone has no collision.
     * CenterInMeshCM is in the mesh reference pose; it is converted to bone space.
     * Existing body shapes are preserved. This does not save or modify a level.
     */
    UFUNCTION(BlueprintCallable, Category="Leon|Groom")
    static bool EnsureGroomHeadCollision(USkeletalMesh* Mesh, FName HeadBone,
        FVector CenterInMeshCM, float RadiusCM, bool bSave, FString& OutReport);

    /** Enable solver capability while leaving the component simulation off.
     * The caller saves the preview level to persist component settings.
     */
    UFUNCTION(BlueprintCallable, Category="Leon|Groom")
    static bool SetupNativeGroomSimulation(UGroomComponent* Groom,
        USkeletalMeshComponent* Body, FName HeadBone, bool bSaveGroom, FString& OutReport);

    /** Return actual effective group/LOD flags and instantiated Niagara presence. */
    UFUNCTION(BlueprintCallable, Category="Leon|Groom")
    static FString InspectNativeGroomSimulation(UGroomComponent* Groom);
};
