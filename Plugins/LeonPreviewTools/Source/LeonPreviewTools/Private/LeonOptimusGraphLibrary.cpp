#include "LeonOptimusGraphLibrary.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/GarbageCollection.h"

#include "OptimusBindingTypes.h"
#include "OptimusComponentSource.h"
#include "OptimusDataDomain.h"
#include "IOptimusComputeKernelProvider.h"
#include "IOptimusNodeAdderPinProvider.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodePin.h"
#include "OptimusShaderText.h"
#include "ShaderCompiler.h"

#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace LeonOptimusGraph
{
    // Optimus exposes direct pin mutators as protected helpers. The graph
    // builder only has a base UOptimusNode pointer, so use a non-instantiated
    // access shim to invoke those helpers without depending on private kernel
    // node headers.
    class FOptimusNodeAccess : public UOptimusNode
    {
    public:
        bool RenamePin(UOptimusNodePin* Pin, FName Name) { return SetPinNameDirect(Pin, Name); }
        bool SetType(UOptimusNodePin* Pin, FOptimusDataTypeRef Type) { return SetPinDataTypeDirect(Pin, Type); }
        bool SetDomain(UOptimusNodePin* Pin, const FOptimusDataDomain& Domain) { return SetPinDataDomainDirect(Pin, Domain); }
    };

    static UClass* FindClass(const TCHAR* Path)
    {
        return FindObject<UClass>(nullptr, Path);
    }

    static UOptimusNodePin* FindPin(UOptimusNode* Node, FName Name)
    {
        if (!Node)
        {
            return nullptr;
        }
        if (UOptimusNodePin* Pin = Node->GetPinByName(Name))
        {
            return Pin;
        }
        // Kernel bindings live below the "Primary Group" pin.  GetPinByName
        // only checks top-level pins, so walk the complete pin tree and match
        // the leaf name used by the binding declaration.
        TFunction<UOptimusNodePin*(TArrayView<UOptimusNodePin* const>)> FindInPins;
        FindInPins = [&FindInPins, Name](TArrayView<UOptimusNodePin* const> Pins) -> UOptimusNodePin*
        {
            for (UOptimusNodePin* Pin : Pins)
            {
                if (Pin && Pin->GetFName() == Name)
                {
                    return Pin;
                }
                if (Pin)
                {
                    if (UOptimusNodePin* Match = FindInPins(Pin->GetSubPins()))
                    {
                        return Match;
                    }
                }
            }
            return nullptr;
        };
        return FindInPins(Node->GetPins());
    }

    static UOptimusNodePin* BindingPinByIndex(UOptimusNode* Node, bool bInput, int32 Index)
    {
        if (!Node || Index < 0)
        {
            return nullptr;
        }
        if (bInput)
        {
            const IOptimusComputeKernelProvider* Provider = Cast<const IOptimusComputeKernelProvider>(Node);
            const UOptimusNodePin* PrimaryConst = Provider ? Provider->GetPrimaryGroupPin() : nullptr;
            UOptimusNodePin* Primary = const_cast<UOptimusNodePin*>(PrimaryConst);
            if (Primary && Primary->GetSubPins().IsValidIndex(Index))
            {
                return Primary->GetSubPins()[Index];
            }
            return nullptr;
        }
        const TArray<UOptimusNodePin*> Outputs = Node->GetPinsByDirection(EOptimusNodePinDirection::Output, false);
        return Outputs.IsValidIndex(Index) ? Outputs[Index] : nullptr;
    }

    // Dynamic custom-kernel bindings must be created through the adder-pin
    // provider.  Mutating InputBindingArray/OutputBindingArray and emitting a
    // generic property event causes Optimus' ArrayAdd handler to replace the
    // authored name with the placeholder `Input`/`Output`.  The public
    // TryAddPinFromPin path executes the same code used by the editor and
    // preserves the requested stable HLSL binding name.
    static UOptimusNodePin* AddKernelBindingFromPin(
        UOptimusNode* Kernel,
        UOptimusNodePin* SourcePin,
        EOptimusNodePinDirection Direction,
        FName BindingName)
    {
        if (!Kernel || !SourcePin)
        {
            return nullptr;
        }
        IOptimusNodeAdderPinProvider* Provider = Cast<IOptimusNodeAdderPinProvider>(Kernel);
        if (!Provider)
        {
            return nullptr;
        }
        FString Reason;
        const TArray<IOptimusNodeAdderPinProvider::FAdderPinAction> Actions =
            Provider->GetAvailableAdderPinActions(SourcePin, Direction, &Reason);
        for (const IOptimusNodeAdderPinProvider::FAdderPinAction& Action : Actions)
        {
            if (Action.NewPinDirection != Direction)
            {
                continue;
            }
            const TArray<UOptimusNodePin*> Added =
                Provider->TryAddPinFromPin(Action, SourcePin, BindingName);
            for (UOptimusNodePin* Pin : Added)
            {
                if (Pin && Pin->GetDirection() == Direction && !Pin->IsGroupingPin())
                {
                    return Pin;
                }
            }
        }
        return nullptr;
    }
    static bool AddBinding(UOptimusNode* Node, FName ArrayPropertyName, FName BindingName, FName DomainName, FName TypeName)
    {
        if (!Node)
        {
            return false;
        }
        FStructProperty* ArrayStructProperty = FindFProperty<FStructProperty>(Node->GetClass(), ArrayPropertyName);
        if (!ArrayStructProperty)
        {
            return false;
        }
        void* ArrayStruct = ArrayStructProperty->ContainerPtrToValuePtr<void>(Node);
        FArrayProperty* InnerArrayProperty = FindFProperty<FArrayProperty>(ArrayStructProperty->Struct, TEXT("InnerArray"));
        if (!InnerArrayProperty)
        {
            return false;
        }
        void* InnerArray = InnerArrayProperty->ContainerPtrToValuePtr<void>(ArrayStruct);
        FScriptArrayHelper Helper(InnerArrayProperty, InnerArray);
        int32 Index = INDEX_NONE;
        for (int32 I = 0; I < Helper.Num(); ++I)
        {
            const FOptimusParameterBinding* Existing = reinterpret_cast<const FOptimusParameterBinding*>(Helper.GetRawPtr(I));
            if (Existing && Existing->Name.Name == BindingName)
            {
                Index = I;
                break;
            }
        }
        if (Index == INDEX_NONE)
        {
            Index = Helper.AddValue();
        }
        FOptimusParameterBinding* Binding = reinterpret_cast<FOptimusParameterBinding*>(Helper.GetRawPtr(Index));
        if (!Binding)
        {
            return false;
        }
        Binding->Name.Name = BindingName;
        Binding->DataType.TypeName = TypeName;
        Binding->DataType.TypeObject = nullptr;
        Binding->DataDomain.Type = EOptimusDataDomainType::Dimensional;
        Binding->DataDomain.DimensionNames.Reset();
        Binding->DataDomain.DimensionNames.Add(DomainName);
        Binding->DataDomain.Multiplier = 1;
        Binding->bSupportAtomicIfCompatibleDataType = false;
        Binding->bSupportRead = false;
        // The editor implementation expects the leaf array property here and
        // the containing FOptimusParameterBindingArray as the active member.
        // Passing the struct property directly leaves the dynamic pins out of
        // sync with the binding array, which makes every subsequent link fail.
        FPropertyChangedEvent Changed(InnerArrayProperty, EPropertyChangeType::ArrayAdd);
        Changed.SetActiveMemberProperty(ArrayStructProperty);
        Node->PostEditChangeProperty(Changed);
        // ArrayAdd creates a placeholder pin (Input/Output).  A ValueSet
        // event makes Optimus rebuild the dynamic pin from the binding's
        // actual name, type and domain, preserving the authored binding.
        FPropertyChangedEvent Refresh(InnerArrayProperty, EPropertyChangeType::ValueSet);
        Refresh.SetActiveMemberProperty(ArrayStructProperty);
        Node->PostEditChangeProperty(Refresh);
        return FindPin(Node, BindingName) != nullptr;
    }

    static bool AddSingletonBinding(UOptimusNode* Node, FName BindingName, FName TypeName)
    {
        if (!Node)
        {
            return false;
        }
        FStructProperty* ArrayStructProperty = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("InputBindingArray"));
        if (!ArrayStructProperty)
        {
            return false;
        }
        void* ArrayStruct = ArrayStructProperty->ContainerPtrToValuePtr<void>(Node);
        FArrayProperty* InnerArrayProperty = FindFProperty<FArrayProperty>(ArrayStructProperty->Struct, TEXT("InnerArray"));
        if (!InnerArrayProperty)
        {
            return false;
        }
        void* InnerArray = InnerArrayProperty->ContainerPtrToValuePtr<void>(ArrayStruct);
        FScriptArrayHelper Helper(InnerArrayProperty, InnerArray);
        int32 Index = INDEX_NONE;
        for (int32 I = 0; I < Helper.Num(); ++I)
        {
            const FOptimusParameterBinding* Existing = reinterpret_cast<const FOptimusParameterBinding*>(Helper.GetRawPtr(I));
            if (Existing && Existing->Name.Name == BindingName)
            {
                Index = I;
                break;
            }
        }
        if (Index == INDEX_NONE)
        {
            Index = Helper.AddValue();
        }
        FOptimusParameterBinding* Binding = reinterpret_cast<FOptimusParameterBinding*>(Helper.GetRawPtr(Index));
        if (!Binding)
        {
            return false;
        }
        Binding->Name.Name = BindingName;
        Binding->DataType.TypeName = TypeName;
        Binding->DataType.TypeObject = nullptr;
        Binding->DataDomain.Type = EOptimusDataDomainType::Dimensional;
        Binding->DataDomain.DimensionNames.Reset();
        Binding->DataDomain.Multiplier = 1;
        Binding->DataDomain.Expression.Reset();
        Binding->bSupportAtomicIfCompatibleDataType = false;
        Binding->bSupportRead = false;
        FPropertyChangedEvent Changed(InnerArrayProperty, EPropertyChangeType::ArrayAdd);
        Changed.SetActiveMemberProperty(ArrayStructProperty);
        Node->PostEditChangeProperty(Changed);
        FPropertyChangedEvent Refresh(InnerArrayProperty, EPropertyChangeType::ValueSet);
        Refresh.SetActiveMemberProperty(ArrayStructProperty);
        Node->PostEditChangeProperty(Refresh);
        return FindPin(Node, BindingName) != nullptr;
    }

    static FString Json(const TSharedRef<FJsonObject>& Object)
    {
        FString Text;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Object, Writer);
        return Text;
    }
}

FString ULeonOptimusGraphLibrary::BuildLeonREHairOptimusGraph(const FString& InAssetPath, bool bForceRebuild)
{
    using namespace LeonOptimusGraph;
    const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("asset_path"), InAssetPath);

    FString PackageName;
    FString PackageRoot;
    FString PackagePath;
    FString AssetName;
    if (!FPackageName::SplitLongPackageName(InAssetPath, PackageRoot, PackagePath, AssetName) ||
        PackageRoot.IsEmpty() || AssetName.IsEmpty())
    {
        Report->SetBoolField(TEXT("success"), false);
        Report->SetStringField(TEXT("error"), TEXT("Expected a long package path such as /Game/Leon/Groom/DF_Leon_REHair"));
        return Json(Report);
    }
    PackageName = PackageRoot + PackagePath + AssetName;

    UOptimusDeformer* Existing = FindObject<UOptimusDeformer>(nullptr, *InAssetPath);
    if (Existing && !bForceRebuild)
    {
        Report->SetBoolField(TEXT("success"), true);
        Report->SetBoolField(TEXT("created"), false);
        Report->SetStringField(TEXT("status"), TEXT("already_exists"));
        Report->SetNumberField(TEXT("nodes"), Existing->GetUpdateGraph() ? Existing->GetUpdateGraph()->GetAllNodes().Num() : 0);
        return Json(Report);
    }
    if (Existing && bForceRebuild)
    {
        Existing->ClearFlags(RF_Public | RF_Standalone);
        Existing->MarkAsGarbage();
        // A force rebuild can run in an editor process that already loaded the
        // deformer.  Leaving the old object in the package makes the next
        // object with the same name appear as a duplicate and SavePackage()
        // quietly returns false.  Collect it before creating the replacement.
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    UPackage* Package = CreatePackage(*PackageName);
    UOptimusDeformer* Deformer = Package ? NewObject<UOptimusDeformer>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional) : nullptr;
    if (!Deformer)
    {
        Report->SetBoolField(TEXT("success"), false);
        Report->SetStringField(TEXT("error"), TEXT("Could not create UOptimusDeformer package"));
        return Json(Report);
    }
    FAssetRegistryModule::AssetCreated(Deformer);
    Deformer->MarkPackageDirty();

    UClass* SourceClass = FindClass(TEXT("/Script/HairStrandsDeformer.OptimusGroomAssetComponentSource"));
    UClass* GuidesReadClass = FindClass(TEXT("/Script/HairStrandsDeformer.OptimusGroomGuidesReadDataInterface"));
    UClass* GuidesWriteClass = FindClass(TEXT("/Script/HairStrandsDeformer.OptimusGroomGuidesWriteDataInterface"));
    UClass* SceneClass = FindClass(TEXT("/Script/OptimusCore.OptimusSceneDataInterface"));
    UClass* RuntimeGuideClass = FindClass(TEXT("/Script/LeonPreviewRuntime.LeonREHairGuideDataInterface"));
    UClass* KernelClass = FindClass(TEXT("/Script/OptimusCore.OptimusNode_CustomComputeKernel"));
    if (!SourceClass || !GuidesReadClass || !GuidesWriteClass || !SceneClass || !RuntimeGuideClass || !KernelClass)
    {
        Report->SetBoolField(TEXT("success"), false);
        Report->SetStringField(TEXT("error"), TEXT("Missing Optimus/HairStrands classes; enable DeformerGraph and HairStrands plugins"));
        return Json(Report);
    }

    UOptimusComponentSource* Source = NewObject<UOptimusComponentSource>(Deformer, SourceClass);
    UOptimusComponentSourceBinding* Binding = Deformer->AddComponentBinding(Source, TEXT("LeonGroom"));
    UOptimusNodeGraph* Graph = Deformer->GetUpdateGraph();
    UOptimusNode* SourceNode = Graph ? Graph->AddComponentBindingGetNode(Binding, FVector2D(-1000, 0)) : nullptr;
    UOptimusNode* GuidesRead = Graph ? Graph->AddDataInterfaceNode(GuidesReadClass, FVector2D(-720, -240)) : nullptr;
    UOptimusNode* RuntimeGuide = Graph ? Graph->AddDataInterfaceNode(RuntimeGuideClass, FVector2D(-720, 0)) : nullptr;
    UOptimusNode* Scene = Graph ? Graph->AddDataInterfaceNode(SceneClass, FVector2D(-720, 240)) : nullptr;
    UOptimusNode* Kernel = Graph ? Graph->AddNode(KernelClass, FVector2D(-220, 0)) : nullptr;
    UOptimusNode* GuidesWrite = Graph ? Graph->AddDataInterfaceNode(GuidesWriteClass, FVector2D(320, 0)) : nullptr;
    if (!Graph || !SourceNode || !GuidesRead || !RuntimeGuide || !Scene || !Kernel || !GuidesWrite)
    {
        Report->SetBoolField(TEXT("success"), false);
        Report->SetStringField(TEXT("error"), TEXT("Failed to create Optimus graph nodes"));
        return Json(Report);
    }

    UOptimusNodePin* SourceOutput = SourceNode->GetPins().Num() > 0 ? SourceNode->GetPins()[0] : nullptr;
    const auto LinkPins = [Graph](UOptimusNodePin* OutputPin, UOptimusNodePin* InputPin)
    {
        return OutputPin && InputPin && Graph->AddLink(OutputPin, InputPin);
    };
    UOptimusNodePin* GuidesReadComponent = GuidesRead->GetPins().Num() > 0 ? GuidesRead->GetPins()[0] : nullptr;
    UOptimusNodePin* RuntimeGuideComponent = RuntimeGuide->GetPins().Num() > 0 ? RuntimeGuide->GetPins()[0] : nullptr;
    UOptimusNodePin* SceneComponent = Scene->GetPins().Num() > 0 ? Scene->GetPins()[0] : nullptr;
    UOptimusNodePin* GuidesWriteComponent = GuidesWrite->GetPins().Num() > 0 ? GuidesWrite->GetPins()[0] : nullptr;
    const bool bSourceToGuidesRead = LinkPins(SourceOutput, GuidesReadComponent);
    const bool bSourceToRuntimeGuide = LinkPins(SourceOutput, RuntimeGuideComponent);
    const bool bSourceToScene = LinkPins(SourceOutput, SceneComponent);
    const bool bSourceToGuidesWrite = LinkPins(SourceOutput, GuidesWriteComponent);
    const IOptimusComputeKernelProvider* KernelProvider = Cast<const IOptimusComputeKernelProvider>(Kernel);
    UOptimusNodePin* PrimaryGroup = KernelProvider
        ? const_cast<UOptimusNodePin*>(KernelProvider->GetPrimaryGroupPin()) : nullptr;
    const bool bSourceToKernel = LinkPins(SourceOutput, PrimaryGroup);
    bool bLinks = bSourceToGuidesRead && bSourceToRuntimeGuide && bSourceToScene &&
        bSourceToGuidesWrite && bSourceToKernel;

    const FName GuidesPoints(TEXT("GuidesPoints"));
    // Create bindings through the same public adder-pin API used by the
    // Optimus editor. Mutating the binding arrays directly causes the
    // ArrayAdd handler to replace authored names with Input/Output.
    UOptimusNodePin* GuidesReadPosition = FindPin(GuidesRead, TEXT("Position"));
    UOptimusNodePin* GuidesReadPointCurveIndex = FindPin(GuidesRead, TEXT("PointCurveIndex"));
    UOptimusNodePin* GuidesReadCurveOffset = FindPin(GuidesRead, TEXT("CurveOffsetPoint"));
    UOptimusNodePin* GuidesReadCurveNumPoint = FindPin(GuidesRead, TEXT("CurveNumPoint"));
    UOptimusNodePin* RuntimeGuidePoint = FindPin(RuntimeGuide, TEXT("GuidePoint"));
    UOptimusNodePin* SceneGameTime = FindPin(Scene, TEXT("GameTime"));
    UOptimusNodePin* GuidesWritePosition = FindPin(GuidesWrite, TEXT("PointDeformedPosition"));
    UOptimusNodePin* RestKernelPin = AddKernelBindingFromPin(Kernel, GuidesReadPosition,
        EOptimusNodePinDirection::Input, TEXT("RestPosition"));
    UOptimusNodePin* PointCurveIndexKernelPin = AddKernelBindingFromPin(Kernel, GuidesReadPointCurveIndex,
        EOptimusNodePinDirection::Input, TEXT("PointCurveIndex"));
    UOptimusNodePin* CurveOffsetKernelPin = AddKernelBindingFromPin(Kernel, GuidesReadCurveOffset,
        EOptimusNodePinDirection::Input, TEXT("CurveOffsetPoint"));
    UOptimusNodePin* CurveNumKernelPin = AddKernelBindingFromPin(Kernel, GuidesReadCurveNumPoint,
        EOptimusNodePinDirection::Input, TEXT("CurveNumPoint"));
    UOptimusNodePin* ProxyKernelPin = AddKernelBindingFromPin(Kernel, RuntimeGuidePoint,
        EOptimusNodePinDirection::Input, TEXT("ProxyPosition"));
    UOptimusNodePin* TimeKernelPin = AddKernelBindingFromPin(Kernel, SceneGameTime,
        EOptimusNodePinDirection::Input, TEXT("Time"));
    UOptimusNodePin* OutputKernelPin = AddKernelBindingFromPin(Kernel, GuidesWritePosition,
        EOptimusNodePinDirection::Output, TEXT("PointDeformedPosition"));
    bool bRest = RestKernelPin != nullptr;
    bool bPointCurveIndex = PointCurveIndexKernelPin != nullptr;
    bool bCurveOffset = CurveOffsetKernelPin != nullptr;
    bool bCurveNum = CurveNumKernelPin != nullptr;
    bool bProxy = ProxyKernelPin != nullptr;
    bool bTime = TimeKernelPin != nullptr;
    bool bOutput = OutputKernelPin != nullptr;

    if (FStructProperty* Domain = FindFProperty<FStructProperty>(Kernel->GetClass(), TEXT("ExecutionDomain")))
    {
        // FOptimusExecutionDomain stores the selected name as a private UPROPERTY.
        if (FNameProperty* NameProperty = FindFProperty<FNameProperty>(Domain->Struct, TEXT("Name")))
        {
            void* DomainValue = Domain->ContainerPtrToValuePtr<void>(Kernel);
            *NameProperty->ContainerPtrToValuePtr<FName>(DomainValue) = GuidesPoints;
        }
    }
    if (FStructProperty* GroupSize = FindFProperty<FStructProperty>(Kernel->GetClass(), TEXT("GroupSize")))
    {
        *GroupSize->ContainerPtrToValuePtr<FIntVector>(Kernel) = FIntVector(64, 1, 1);
    }
    if (FStructProperty* KernelName = FindFProperty<FStructProperty>(Kernel->GetClass(), TEXT("KernelName")))
    {
        if (FNameProperty* NameProperty = FindFProperty<FNameProperty>(KernelName->Struct, TEXT("Name")))
        {
            void* Value = KernelName->ContainerPtrToValuePtr<void>(Kernel);
            *NameProperty->ContainerPtrToValuePtr<FName>(Value) = TEXT("LeonREHairGuideDeform");
        }
    }
    if (FStructProperty* Shader = FindFProperty<FStructProperty>(Kernel->GetClass(), TEXT("ShaderSource")))
    {
        FOptimusShaderText* SourceText = Shader->ContainerPtrToValuePtr<FOptimusShaderText>(Kernel);
        // The runtime guide DI contains only the compact five-point proxy, but
        // GuidesPoints executes once for every authored guide point.  The old
        // kernel therefore read ProxyPosition(Index), which clamped every
        // invocation after point four to the tip and collapsed the groom into
        // a synchronized sheet.  Resolve each point's curve-local U and
        // evaluate the same four-overlapping Catmull-Rom segments used by the
        // render-thread writer. Explicit indices 0..4 are safe because the
        // DI's ReadGuidePoint implementation clamps to its five controls.
        SourceText->ShaderText = TEXT("KERNEL\n{\n    const float3 Rest = ReadRestPosition(Index);\n    const uint CurveIndex = ReadPointCurveIndex(Index);\n    const uint CurveOffset = ReadCurveOffsetPoint(CurveIndex);\n    const uint CurveCount = max(ReadCurveNumPoint(CurveIndex), 1u);\n    const uint LocalPoint = (Index >= CurveOffset) ? Index - CurveOffset : 0u;\n    const float U = saturate((float)LocalPoint / max((float)CurveCount - 1.0, 1.0));\n    const float SegmentCoord = U * 4.0;\n    const uint Segment = min((uint)floor(SegmentCoord), 3u);\n    const float T = SegmentCoord - (float)Segment;\n    const uint P0 = (Segment > 0u) ? Segment - 1u : 0u;\n    const uint P1 = Segment;\n    const uint P2 = Segment + 1u;\n    const uint P3 = min(Segment + 2u, 4u);\n    const float3 C0 = ReadProxyPosition(P0);\n    const float3 C1 = ReadProxyPosition(P1);\n    const float3 C2 = ReadProxyPosition(P2);\n    const float3 C3 = ReadProxyPosition(P3);\n    const float T2 = T * T;\n    const float T3 = T2 * T;\n    const float3 Proxy = 0.5 * ((2.0 * C1) + (-C0 + C2) * T +\n        (2.0 * C0 - 5.0 * C1 + 4.0 * C2 - C3) * T2 +\n        (-C0 + 3.0 * C1 - 3.0 * C2 + C3) * T3);\n    const float TimeValue = ReadTime();\n    // Preserve each authored strand's curl. Proxy is a compact absolute curve in component space, so only its displacement from the compact root is applied to the dense rest point; this pins every scalp root.\n    const float3 ProxyDelta = Proxy - C0;\n    const float Envelope = saturate(U * 1.15);\n    const float Flutter = 0.012 * Envelope * Envelope * sin(TimeValue * 1.17 + (float)Index * 0.071);\n    WritePointDeformedPosition(Index, Rest + ProxyDelta * (0.72 + 0.28 * Envelope) + float3(Flutter, 0.0, 0.0));\n}");
    }
    Kernel->PostEditChange();

    const bool bRestLink = LinkPins(GuidesReadPosition, RestKernelPin);
    const bool bPointCurveIndexLink = LinkPins(GuidesReadPointCurveIndex, PointCurveIndexKernelPin);
    const bool bCurveOffsetLink = LinkPins(GuidesReadCurveOffset, CurveOffsetKernelPin);
    const bool bCurveNumLink = LinkPins(GuidesReadCurveNumPoint, CurveNumKernelPin);
    const bool bProxyLink = LinkPins(RuntimeGuidePoint, ProxyKernelPin);
    const bool bTimeLink = LinkPins(SceneGameTime, TimeKernelPin);
    const bool bOutputLink = LinkPins(OutputKernelPin, GuidesWritePosition);
    bLinks = bLinks && bRestLink && bPointCurveIndexLink && bCurveOffsetLink && bCurveNumLink && bProxyLink && bTimeLink && bOutputLink;

    auto PinNames = [](UOptimusNode* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        if (!Node)
        {
            return Values;
        }
        TFunction<void(TArrayView<UOptimusNodePin* const>, const FString&)> Visit;
        Visit = [&Values, &Visit](TArrayView<UOptimusNodePin* const> Pins, const FString& Prefix)
        {
            for (UOptimusNodePin* Pin : Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                const FString Path = Prefix.IsEmpty() ? Pin->GetFName().ToString()
                    : Prefix + TEXT(".") + Pin->GetFName().ToString();
                Values.Add(MakeShared<FJsonValueString>(Path));
                Visit(Pin->GetSubPins(), Path);
            }
        };
        Visit(Node->GetPins(), FString());
        return Values;
    };

    const bool bCompiled = Deformer->Compile();
    // Optimus Compile() can queue a shader map and return before the package
    // is serializable. Finish those jobs before SavePackage so commandlet
    // invocations do not cancel the newly generated kernel on exit.
    if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
    {
        GShaderCompilingManager->FinishAllCompilation();
    }
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Package, Deformer, *Filename, SaveArgs);

    Report->SetBoolField(TEXT("success"), bLinks && bRest && bPointCurveIndex && bCurveOffset && bCurveNum && bProxy && bTime && bOutput && bSaved);
    Report->SetBoolField(TEXT("created"), true);
    Report->SetBoolField(TEXT("links_valid"), bLinks);
    Report->SetBoolField(TEXT("link_source_to_guides_read"), bSourceToGuidesRead);
    Report->SetBoolField(TEXT("link_source_to_runtime_guide"), bSourceToRuntimeGuide);
    Report->SetBoolField(TEXT("link_source_to_scene"), bSourceToScene);
    Report->SetBoolField(TEXT("link_source_to_guides_write"), bSourceToGuidesWrite);
    Report->SetBoolField(TEXT("link_source_to_kernel"), bSourceToKernel);
    Report->SetBoolField(TEXT("link_rest_position"), bRestLink);
    Report->SetBoolField(TEXT("link_point_curve_index"), bPointCurveIndexLink);
    Report->SetBoolField(TEXT("link_curve_offset"), bCurveOffsetLink);
    Report->SetBoolField(TEXT("link_curve_num_point"), bCurveNumLink);
    Report->SetBoolField(TEXT("link_proxy_position"), bProxyLink);
    Report->SetBoolField(TEXT("link_time"), bTimeLink);
    Report->SetBoolField(TEXT("link_output_position"), bOutputLink);
    Report->SetBoolField(TEXT("binding_rest_position"), bRest);
    Report->SetBoolField(TEXT("binding_point_curve_index"), bPointCurveIndex);
    Report->SetBoolField(TEXT("binding_curve_offset"), bCurveOffset);
    Report->SetBoolField(TEXT("binding_curve_num_point"), bCurveNum);
    Report->SetBoolField(TEXT("binding_proxy_position"), bProxy);
    Report->SetBoolField(TEXT("binding_time"), bTime);
    Report->SetBoolField(TEXT("binding_output_position"), bOutput);
    Report->SetBoolField(TEXT("compiled"), bCompiled);
    Report->SetBoolField(TEXT("saved"), bSaved);
    Report->SetNumberField(TEXT("nodes"), Graph->GetAllNodes().Num());
    Report->SetNumberField(TEXT("links"), Graph->GetAllLinks().Num());
    Report->SetArrayField(TEXT("guides_read_pins"), PinNames(GuidesRead));
    Report->SetArrayField(TEXT("runtime_guide_pins"), PinNames(RuntimeGuide));
    Report->SetArrayField(TEXT("scene_pins"), PinNames(Scene));
    Report->SetArrayField(TEXT("kernel_pins"), PinNames(Kernel));
    Report->SetArrayField(TEXT("guides_write_pins"), PinNames(GuidesWrite));
    Report->SetStringField(TEXT("guide_domain"), GuidesPoints.ToString());
    Report->SetStringField(TEXT("runtime_interface"), TEXT("LeonREHairGuideDataInterface -> structured SRV"));
    Report->SetStringField(TEXT("write_interface"), TEXT("OptimusGroomGuidesWriteDataInterface -> DeformerBuffer UAV"));
    Report->SetStringField(TEXT("filename"), Filename);
    return Json(Report);
}

#else

FString ULeonOptimusGraphLibrary::BuildLeonREHairOptimusGraph(const FString& InAssetPath, bool bForceRebuild)
{
    return TEXT("{\"success\":false,\"error\":\"Editor-only Optimus graph builder\"}");
}

#endif
