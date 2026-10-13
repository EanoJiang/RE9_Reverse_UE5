#include "LeonREHairGuideDataInterface.h"

#include "LeonREHairGuideBufferComponent.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterMetadataBuilder.h"
#include "Components/MeshComponent.h"
#include "EngineUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LeonREHairGuideDataInterface)

BEGIN_SHADER_PARAMETER_STRUCT(FLeonREHairGuideParameterStruct, )
	SHADER_PARAMETER(uint32, GuidePointCount)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, GuidePoints)
END_SHADER_PARAMETER_STRUCT()

FString ULeonREHairGuideDataInterface::GetDisplayName() const
{
	return TEXT("RE Hair Guide Chain (GPU)");
}

TArray<FOptimusCDIPinDefinition> ULeonREHairGuideDataInterface::GetPinDefinitions() const
{
	// "Points" is the canonical Groom guides execution-domain name in UE 5.8.
	// The provider returns the compact chain count; shader code clamps indices so
	// it can also be sampled by a full Groom point-domain kernel.
	return {
		FOptimusCDIPinDefinition(TEXT("GuidePoint"), TEXT("ReadGuidePoint"), FName(TEXT("Points")), TEXT("ReadGuidePointCount"), false, TEXT("Guide Point"))
	};
}

TSubclassOf<UActorComponent> ULeonREHairGuideDataInterface::GetRequiredComponentClass() const
{
	return UMeshComponent::StaticClass();
}

void ULeonREHairGuideDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.AddDefaulted_GetRef().SetName(TEXT("ReadGuidePointCount")).AddReturnType(EShaderFundamentalType::Uint);
	OutFunctions.AddDefaulted_GetRef().SetName(TEXT("ReadGuidePoint"))
		.AddParam(EShaderFundamentalType::Uint)
		.AddReturnType(EShaderFundamentalType::Float, 3);
}

void ULeonREHairGuideDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	InOutBuilder.AddNestedStruct<FLeonREHairGuideParameterStruct>(UID);
}

TCHAR const* ULeonREHairGuideDataInterface::TemplateFilePath = TEXT("/Plugin/LeonPreviewTools/Private/LeonREHairGuide.ush");

TCHAR const* ULeonREHairGuideDataInterface::GetShaderVirtualPath() const
{
	return TemplateFilePath;
}

void ULeonREHairGuideDataInterface::GetShaderHash(FString& InOutKey) const
{
	GetShaderFileHash(TemplateFilePath, EShaderPlatform::SP_PCD3D_SM5).AppendString(InOutKey);
}

void ULeonREHairGuideDataInterface::GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const
{
	TMap<FString, FStringFormatArg> Args = {{ TEXT("DataInterfaceName"), InDataInterfaceName }};
	FString Source;
	LoadShaderSourceFile(TemplateFilePath, EShaderPlatform::SP_PCD3D_SM5, &Source, nullptr);
	OutHLSL += FString::Format(*Source, Args);
}

UComputeDataProvider* ULeonREHairGuideDataInterface::CreateDataProvider() const
{
	return NewObject<ULeonREHairGuideDataProvider>();
}

void ULeonREHairGuideDataProvider::Initialize(const UComputeDataInterface* InDataInterface, UObject* InBinding, uint64 InInputMask, uint64 InOutputMask)
{
	GuidePoints.Reset();
	if (const UMeshComponent* Mesh = Cast<UMeshComponent>(InBinding))
	{
		if (AActor* Owner = Mesh->GetOwner())
		{
			if (const ULeonREHairGuideBufferComponent* Source = Owner->FindComponentByClass<ULeonREHairGuideBufferComponent>())
			{
				Source->Snapshot(GuidePoints);
			}
			// The Groom component is commonly a separate actor whose attach
			// parent is Leon's skeletal mesh.  Optimus binds its Groom source
			// component, so the owner above can differ from the hero actor that
			// owns the game-thread chain buffer. Resolve that shared buffer in
			// the same world instead of invalidating the entire deformer dispatch.
			if (GuidePoints.Num() == 0 && Owner->GetWorld())
			{
				for (TActorIterator<AActor> It(Owner->GetWorld()); It; ++It)
				{
					if (const ULeonREHairGuideBufferComponent* Source =
						It->FindComponentByClass<ULeonREHairGuideBufferComponent>())
					{
						Source->Snapshot(GuidePoints);
						if (GuidePoints.Num() > 0)
							break;
					}
				}
			}
		}
	}
}

FComputeDataProviderRenderProxy* ULeonREHairGuideDataProvider::GetRenderProxy()
{
	return new FLeonREHairGuideProviderProxy(MoveTemp(GuidePoints));
}

FLeonREHairGuideProviderProxy::FLeonREHairGuideProviderProxy(TArray<FVector4f>&& InPoints)
	: Points(MoveTemp(InPoints))
{
}

bool FLeonREHairGuideProviderProxy::IsValid(FValidationData const& InValidationData) const
{
	return InValidationData.ParameterStructSize == sizeof(FLeonREHairGuideParameterStruct) && Points.Num() > 0;
}

void FLeonREHairGuideProviderProxy::AllocateResources(FRDGBuilder& GraphBuilder, FAllocationData const& InAllocationData)
{
	const uint32 Count = FMath::Max(Points.Num(), 1);
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Count),
		TEXT("Leon.REHairGuideChain"));
	GuidePointsSRV = GraphBuilder.CreateSRV(Buffer);
	if (Points.Num() > 0)
	{
		GraphBuilder.QueueBufferUpload(Buffer, Points.GetData(), Points.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
	}
	else
	{
		const FVector4f Zero(0, 0, 0, 1);
		GraphBuilder.QueueBufferUpload(Buffer, &Zero, sizeof(FVector4f), ERDGInitialDataFlags::None);
	}
}

void FLeonREHairGuideProviderProxy::GatherDispatchData(FDispatchData const& InDispatchData)
{
	const TStridedView<FLeonREHairGuideParameterStruct> Parameters = MakeStridedParameterView<FLeonREHairGuideParameterStruct>(InDispatchData);
	for (int32 InvocationIndex = 0; InvocationIndex < Parameters.Num(); ++InvocationIndex)
	{
		FLeonREHairGuideParameterStruct& P = Parameters[InvocationIndex];
		P.GuidePointCount = FMath::Max(Points.Num(), 1);
		P.GuidePoints = GuidePointsSRV;
	}
}
