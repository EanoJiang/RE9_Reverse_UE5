#pragma once

#include "OptimusComputeDataInterface.h"
#include "ComputeFramework/ComputeDataProvider.h"
#include "LeonREHairGuideDataInterface.generated.h"

class FRDGBufferSRV;
class FRDGBuffer;
class UMeshComponent;

/**
 * Optimus input interface for the compact RE chain.  It deliberately only
 * owns the read side; HairStrandsDeformer's built-in GroomGuidesWrite
 * interface remains the output side and writes the guide deformer UAV.
 */
UCLASS(Category=ComputeFramework)
class LEONPREVIEWRUNTIME_API ULeonREHairGuideDataInterface : public UOptimusComputeDataInterface
{
	GENERATED_BODY()

public:
	virtual FString GetDisplayName() const override;
	virtual TArray<FOptimusCDIPinDefinition> GetPinDefinitions() const override;
	virtual TSubclassOf<UActorComponent> GetRequiredComponentClass() const override;

	virtual TCHAR const* GetClassName() const override { return TEXT("LeonREHairGuide"); }
	virtual bool CanSupportUnifiedDispatch() const override { return true; }
	virtual void GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const override;
	virtual void GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const override;
	virtual TCHAR const* GetShaderVirtualPath() const override;
	virtual void GetShaderHash(FString& InOutKey) const override;
	virtual void GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const override;
	virtual UComputeDataProvider* CreateDataProvider() const override;

private:
	static TCHAR const* TemplateFilePath;
};

UCLASS(BlueprintType, editinlinenew, Category=ComputeFramework)
class LEONPREVIEWRUNTIME_API ULeonREHairGuideDataProvider : public UComputeDataProvider
{
	GENERATED_BODY()

public:
	virtual void Initialize(const UComputeDataInterface* InDataInterface, UObject* InBinding, uint64 InInputMask, uint64 InOutputMask) override;
	virtual FComputeDataProviderRenderProxy* GetRenderProxy() override;

private:
	UPROPERTY(Transient)
	TArray<FVector4f> GuidePoints;
};

class FLeonREHairGuideProviderProxy : public FComputeDataProviderRenderProxy
{
public:
	explicit FLeonREHairGuideProviderProxy(TArray<FVector4f>&& InPoints);
	virtual bool IsValid(FValidationData const& InValidationData) const override;
	virtual void AllocateResources(FRDGBuilder& GraphBuilder, FAllocationData const& InAllocationData) override;
	virtual void GatherDispatchData(FDispatchData const& InDispatchData) override;

private:
	TArray<FVector4f> Points;
	FRDGBufferSRVRef GuidePointsSRV = nullptr;
};
