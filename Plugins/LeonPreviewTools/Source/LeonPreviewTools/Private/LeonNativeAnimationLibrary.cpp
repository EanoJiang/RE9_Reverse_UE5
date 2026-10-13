#include "LeonNativeAnimationLibrary.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Engine/SkeletalMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace LeonNative
{
struct FPose
{
    FVector T = FVector::ZeroVector;
    FQuat Q = FQuat::Identity;
    FVector S = FVector::OneVector;
};
static FPose Compose(const FPose& Parent, const FPose& Local)
{
    return {Parent.T + Parent.Q.RotateVector(Parent.S * Local.T),
        (Parent.Q * Local.Q).GetNormalized(), Parent.S * Local.S};
}
static FPose Relative(const FPose& Child, const FPose& Parent)
{
    return {Parent.Q.Inverse().RotateVector(Child.T - Parent.T) / Parent.S,
        (Parent.Q.Inverse() * Child.Q).GetNormalized(), Child.S / Parent.S};
}
static FVector ConvertVector(const FVector& V) { return FVector(V.X, V.Z, V.Y); }
static FQuat ConvertQuat(const FQuat& Q) { return FQuat(-Q.X, -Q.Z, -Q.Y, Q.W).GetNormalized(); }
static FString JsonText(const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
    return Text;
}
static bool SaveAsset(UObject* Asset)
{
    if (!Asset) return false;
    UPackage* Package = Asset->GetOutermost();
    const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Package, Asset, *Filename, Args);
}
struct FChannel
{
    int32 Components = 0;
    bool bStep = false;
    TArray<double> Times;
    TArray<FVector4> Values; // Raw glTF units; scalar curves never pass through C.
    FVector4 Sample(double Time, bool bRotation) const
    {
        int32 Low = 0, High = Times.Num();
        while (Low < High) { const int32 Mid = (Low + High) / 2; if (Times[Mid] <= Time) Low = Mid + 1; else High = Mid; }
        const int32 I = Low - 1;
        if (I < 0) return Values[0];
        if (I >= Times.Num()-1 || bStep) return Values[I];
        const double Alpha = FMath::Clamp((Time-Times[I])/(Times[I+1]-Times[I]), 0., 1.);
        if (bRotation)
        {
            const auto& A = Values[I]; const auto& B = Values[I+1];
            const FQuat Q = FQuat::Slerp(FQuat(A.X,A.Y,A.Z,A.W), FQuat(B.X,B.Y,B.Z,B.W), Alpha).GetNormalized();
            return FVector4(Q.X,Q.Y,Q.Z,Q.W);
        }
        return FMath::Lerp(Values[I], Values[I+1], Alpha);
    }
};
struct FNode
{
    FName Name;
    FString OriginalName;
    int32 Parent = INDEX_NONE;
    FPose Local, Rest;
    TOptional<FChannel> Translation, Rotation, Scale;
    bool bScalar = false;
    bool HasTracks() const { return Translation.IsSet() || Rotation.IsSet() || Scale.IsSet(); }
    FPose Sample(double Time) const
    {
        FPose Pose = Local;
        if (Translation.IsSet()) { const auto V = Translation->Sample(Time, false); Pose.T = ConvertVector(FVector(V.X,V.Y,V.Z))*100.; }
        if (Rotation.IsSet()) { const auto V = Rotation->Sample(Time, true); Pose.Q = ConvertQuat(FQuat(V.X,V.Y,V.Z,V.W)); }
        if (Scale.IsSet()) { const auto V = Scale->Sample(Time, false); Pose.S = ConvertVector(FVector(V.X,V.Y,V.Z)); }
        return Pose;
    }
};
struct FDocument
{
    TSharedPtr<FJsonObject> Json;
    TArray<uint8> Bytes;
    TArray<FNode> Nodes;
    TMap<FName,int32> ByName;
    int64 BinaryStart = 0, BinaryLength = 0;
    double Duration = 0;
    double KeyEnvelopeDuration = 0, SourceDurationSeconds = 0;
    int32 SourceFrameCount = -1;
    double SourceFrameRate = 0;
    int32 NativeTrackCount = 0;
    static uint32 U32(const uint8* P) { uint32 V; FMemory::Memcpy(&V,P,4); return V; }
    bool Accessor(int32 Index, TArray<FVector4>& Out, int32& Components, FString& Error) const
    {
        const auto& Accessors = Json->GetArrayField(TEXT("accessors"));
        if (!Accessors.IsValidIndex(Index)) { Error=TEXT("Accessor index out of range"); return false; }
        const auto A=Accessors[Index]->AsObject();
        if (A->GetIntegerField(TEXT("componentType"))!=5126) { Error=TEXT("Motion accessor is not float32"); return false; }
        const FString Type=A->GetStringField(TEXT("type"));
        Components=Type==TEXT("SCALAR")?1:Type==TEXT("VEC3")?3:Type==TEXT("VEC4")?4:0;
        if (!Components) { Error=TEXT("Unsupported motion accessor type: ")+Type; return false; }
        const auto& Views=Json->GetArrayField(TEXT("bufferViews"));
        const int32 ViewIndex=A->GetIntegerField(TEXT("bufferView"));
        if (!Views.IsValidIndex(ViewIndex)) { Error=TEXT("Invalid buffer view"); return false; }
        const auto V=Views[ViewIndex]->AsObject();
        if (V->GetIntegerField(TEXT("buffer"))!=0) { Error=TEXT("Only embedded GLB buffer 0 is supported"); return false; }
        double ViewOffset=0,AccessorOffset=0,Stride=Components*4;
        V->TryGetNumberField(TEXT("byteOffset"),ViewOffset); A->TryGetNumberField(TEXT("byteOffset"),AccessorOffset); V->TryGetNumberField(TEXT("byteStride"),Stride);
        const int32 Count=A->GetIntegerField(TEXT("count"));
        const int64 Offset=int64(ViewOffset+AccessorOffset);
        if (Count<1 || Count>10000000 || Stride<Components*4 || Offset<0 || Offset+int64(Count-1)*int64(Stride)+Components*4>BinaryLength)
        { Error=TEXT("Motion accessor exceeds GLB binary bounds"); return false; }
        Out.SetNumUninitialized(Count);
        for (int32 I=0;I<Count;++I)
        {
            float Value[4]={0,0,0,0}; FMemory::Memcpy(Value,Bytes.GetData()+BinaryStart+Offset+int64(I)*int64(Stride),Components*4);
            Out[I]=FVector4(Value[0],Value[1],Value[2],Value[3]);
        }
        return true;
    }
    bool Read(const FString& Filename,FString& Error)
    {
        if (!FFileHelper::LoadFileToArray(Bytes,*Filename)) { Error=TEXT("Cannot read source GLB"); return false; }
        if (Bytes.Num()<28 || U32(Bytes.GetData())!=0x46546C67 || U32(Bytes.GetData()+4)!=2 || U32(Bytes.GetData()+8)!=uint32(Bytes.Num()))
        { Error=TEXT("Invalid GLB 2 header"); return false; }
        const int64 JsonLength=U32(Bytes.GetData()+12);
        if (U32(Bytes.GetData()+16)!=0x4E4F534A || JsonLength<2 || 28+JsonLength>Bytes.Num()) { Error=TEXT("Invalid GLB JSON chunk"); return false; }
        FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+20),JsonLength);
        const FString Text(Converted.Length(),Converted.Get());
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json) || !Json.IsValid()) { Error=TEXT("Invalid GLB JSON"); return false; }
        const int64 BinHeader=20+JsonLength;
        BinaryLength=U32(Bytes.GetData()+BinHeader); BinaryStart=BinHeader+8;
        if (U32(Bytes.GetData()+BinHeader+4)!=0x004E4942 || BinaryStart+BinaryLength>Bytes.Num()) { Error=TEXT("Invalid GLB binary chunk"); return false; }
        const auto& JsonNodes=Json->GetArrayField(TEXT("nodes")); Nodes.SetNum(JsonNodes.Num());
        for (int32 I=0;I<Nodes.Num();++I)
        {
            const auto N=JsonNodes[I]->AsObject(); auto& Node=Nodes[I];
            N->TryGetStringField(TEXT("name"),Node.OriginalName); Node.Name=FName(*Node.OriginalName);
            Node.bScalar=Node.OriginalName.StartsWith(TEXT("HeadSkinned_blendShape.")) || Node.OriginalName.StartsWith(TEXT("Rig_Manager.Mask"));
            ByName.Add(Node.Name,I);
            const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
            if (N->TryGetArrayField(TEXT("translation"),A) && A->Num()==3) Node.Local.T=ConvertVector(FVector((*A)[0]->AsNumber(),(*A)[1]->AsNumber(),(*A)[2]->AsNumber()))*100.;
            if (N->TryGetArrayField(TEXT("rotation"),A) && A->Num()==4) Node.Local.Q=ConvertQuat(FQuat((*A)[0]->AsNumber(),(*A)[1]->AsNumber(),(*A)[2]->AsNumber(),(*A)[3]->AsNumber()));
            if (N->TryGetArrayField(TEXT("scale"),A) && A->Num()==3) Node.Local.S=ConvertVector(FVector((*A)[0]->AsNumber(),(*A)[1]->AsNumber(),(*A)[2]->AsNumber()));
            if (N->TryGetArrayField(TEXT("children"),A)) for (auto Child:*A)
            {
                const int32 C=Child->AsNumber(); if (!Nodes.IsValidIndex(C) || Nodes[C].Parent!=INDEX_NONE) { Error=TEXT("Invalid or duplicate parent"); return false; } Nodes[C].Parent=I;
            }
        }
        TArray<uint8> Seen; Seen.Init(0,Nodes.Num());
        TFunction<bool(int32)> Rest=[&](int32 I)
        {
            if (Seen[I]==2) return true; if (Seen[I]==1) { Error=TEXT("Cycle in native hierarchy"); return false; } Seen[I]=1;
            if (Nodes[I].Parent!=INDEX_NONE && !Rest(Nodes[I].Parent)) return false;
            Nodes[I].Rest=Compose(Nodes[I].Parent!=INDEX_NONE?Nodes[Nodes[I].Parent].Rest:FPose{},Nodes[I].Local); Seen[I]=2; return true;
        };
        for (int32 I=0;I<Nodes.Num();++I) if (!Rest(I)) return false;
        const TArray<TSharedPtr<FJsonValue>>* Animations=nullptr;
        if (!Json->TryGetArrayField(TEXT("animations"),Animations) || Animations->IsEmpty()) { Error=TEXT("GLB has no preserved motion animation"); return false; }
        const auto Anim=(*Animations)[0]->AsObject(); const auto& Samplers=Anim->GetArrayField(TEXT("samplers"));
        for (auto Entry:Anim->GetArrayField(TEXT("channels")))
        {
            const auto C=Entry->AsObject(),Target=C->GetObjectField(TEXT("target")); const int32 I=Target->GetIntegerField(TEXT("node"));
            const int32 SamplerIndex=C->GetIntegerField(TEXT("sampler"));
            if (!Nodes.IsValidIndex(I) || !Samplers.IsValidIndex(SamplerIndex)) { Error=TEXT("Animation channel index invalid"); return false; }
            const auto Sampler=Samplers[SamplerIndex]->AsObject(); FChannel Channel; TArray<FVector4> Times; int32 Count;
            if (!Accessor(Sampler->GetIntegerField(TEXT("input")),Times,Count,Error) || Count!=1) { if(Error.IsEmpty())Error=TEXT("Animation input is not SCALAR"); return false; }
            if (!Accessor(Sampler->GetIntegerField(TEXT("output")),Channel.Values,Channel.Components,Error)) return false;
            if (Times.Num()!=Channel.Values.Num()) { Error=TEXT("Animation time/value counts differ"); return false; }
            FString Interpolation=TEXT("LINEAR"); Sampler->TryGetStringField(TEXT("interpolation"),Interpolation);
            if (Interpolation!=TEXT("LINEAR") && Interpolation!=TEXT("STEP")) { Error=TEXT("Unsupported interpolation: ")+Interpolation; return false; } Channel.bStep=Interpolation==TEXT("STEP");
            for (auto Time:Times) { if(!Channel.Times.IsEmpty() && Time.X<Channel.Times.Last()){Error=TEXT("Unsorted motion key times");return false;}Channel.Times.Add(Time.X);Duration=FMath::Max(Duration,Time.X); }
            const FString Path=Target->GetStringField(TEXT("path"));
            if (Path==TEXT("translation") && Channel.Components==3) Nodes[I].Translation=MoveTemp(Channel);
            else if (Path==TEXT("rotation") && Channel.Components==4) Nodes[I].Rotation=MoveTemp(Channel);
            else if (Path==TEXT("scale") && Channel.Components==3) Nodes[I].Scale=MoveTemp(Channel);
            else { Error=TEXT("Unsupported native motion path: ")+Path; return false; }
        }
        KeyEnvelopeDuration=Duration;
        auto ReadDurationMetadata=[&](const TSharedPtr<FJsonObject>& Object)
        {
            const TSharedPtr<FJsonObject>* Extras=nullptr;
            if(!Object->TryGetObjectField(TEXT("extras"),Extras)||!Extras||!Extras->IsValid())return;
            double Seconds=0,Frames=-1,Rate=0;
            (*Extras)->TryGetNumberField(TEXT("sourceDurationSeconds"),Seconds);
            (*Extras)->TryGetNumberField(TEXT("sourceFrameCount"),Frames);
            (*Extras)->TryGetNumberField(TEXT("sourceFrameRate"),Rate);
            if(FMath::IsFinite(Seconds)&&Seconds>0)SourceDurationSeconds=FMath::Max(SourceDurationSeconds,Seconds);
            if(FMath::IsFinite(Frames)&&FMath::IsFinite(Rate)&&Frames>=0&&Frames<=100000000&&Rate>0)
            {
                SourceFrameCount=FMath::RoundToInt(Frames);SourceFrameRate=Rate;
                SourceDurationSeconds=FMath::Max(SourceDurationSeconds,Frames/Rate);
            }
        };
        ReadDurationMetadata(Json);ReadDurationMetadata(Anim);
        Duration=FMath::Max(KeyEnvelopeDuration,SourceDurationSeconds);
        for (auto& N:Nodes) if(N.HasTracks()) ++NativeTrackCount;
        return true;
    }
};
struct FOutputTrack
{
    int32 Source=INDEX_NONE,Target=INDEX_NONE,SourceTargetParent=INDEX_NONE;
    bool bReparented=false;
    TArray<FVector3f> Positions,Scales;
    TArray<FQuat4f> Rotations;
};
}

UAnimSequence* ULeonNativeAnimationLibrary::ImportNativeGLB(const FString& SourceFilename,const FString& DestinationPath,
    const FString& AssetName,USkeleton* TargetSkeleton,USkeletalMesh* PreviewMesh,int32 SampleRate,bool bSave,FString& OutReport)
{
    using namespace LeonNative;
    const double Start=FPlatformTime::Seconds();
    auto Report=MakeShared<FJsonObject>(); Report->SetStringField(TEXT("source"),SourceFilename);
    auto Fail=[&](const FString& Error)->UAnimSequence* {Report->SetBoolField(TEXT("success"),false);Report->SetStringField(TEXT("error"),Error);OutReport=JsonText(Report);UE_LOG(LogTemp,Error,TEXT("Leon native import: %s"),*Error);return nullptr;};
    if (!TargetSkeleton || SampleRate<1 || SampleRate>480) return Fail(TEXT("Target skeleton or sample rate invalid"));
    const FString PackageName=DestinationPath/AssetName;
    if (!FPackageName::IsValidLongPackageName(PackageName) || AssetName.IsEmpty() || AssetName.Contains(TEXT("/")) || AssetName.Contains(TEXT("."))) return Fail(TEXT("Invalid destination asset path"));
    FDocument Doc; FString Error; if(!Doc.Read(SourceFilename,Error))return Fail(Error);
    const auto& Ref=TargetSkeleton->GetReferenceSkeleton(); const int32 BoneCount=Ref.GetNum();
    TArray<FPose> TargetRest; TargetRest.SetNum(BoneCount);
    for (int32 I=0;I<BoneCount;++I)
    {
        const auto& T=Ref.GetRefBonePose()[I]; const FPose Local{T.GetTranslation(),T.GetRotation(),T.GetScale3D()};
        const int32 P=Ref.GetParentIndex(I); TargetRest[I]=Compose(P>=0?TargetRest[P]:FPose{},Local);
    }
    TArray<FQuat> Offsets; Offsets.Init(FQuat::Identity,Doc.Nodes.Num());
    TArray<FVector> OffsetScales; OffsetScales.Init(FVector::OneVector,Doc.Nodes.Num());
    TArray<int32> TargetToSource; TargetToSource.Init(INDEX_NONE,BoneCount);
    for (int32 I=0;I<Doc.Nodes.Num();++I)
    {
        const int32 Target=Ref.FindBoneIndex(Doc.Nodes[I].Name);
        if(Target>=0){TargetToSource[Target]=I;Offsets[I]=(Doc.Nodes[I].Rest.Q.Inverse()*TargetRest[Target].Q).GetNormalized();OffsetScales[I]=TargetRest[Target].S/Doc.Nodes[I].Rest.S;}
    }
    TArray<FOutputTrack> Tracks; TArray<TSharedPtr<FJsonValue>> Skipped,Reparented,ScalarNodes;
    for (int32 I=0;I<Doc.Nodes.Num();++I)
    {
        const auto& N=Doc.Nodes[I]; const int32 Target=Ref.FindBoneIndex(N.Name);
        if(N.bScalar){if(N.HasTracks())ScalarNodes.Add(MakeShared<FJsonValueString>(N.OriginalName));continue;}
        if(Target<0){if(N.HasTracks())Skipped.Add(MakeShared<FJsonValueString>(N.OriginalName));continue;}
        const int32 P=Ref.GetParentIndex(Target),SourceTargetParent=P>=0?TargetToSource[P]:INDEX_NONE;
        const bool bReparented=SourceTargetParent!=N.Parent;
        if(!N.HasTracks()&&!bReparented)continue;
        auto& T=Tracks.AddDefaulted_GetRef();T.Source=I;T.Target=Target;T.SourceTargetParent=SourceTargetParent;T.bReparented=bReparented;
        if(bReparented)Reparented.Add(MakeShared<FJsonValueString>(N.OriginalName));
    }
    if(!Skipped.IsEmpty()){Report->SetArrayField(TEXT("missing_native_bones"),Skipped);return Fail(TEXT("Target skeleton is missing animated native helper bones"));}
    const int32 Frames=FMath::Max(1,FMath::RoundToInt(Doc.Duration*SampleRate));
    if(Frames>1000000)return Fail(TEXT("Native motion exceeds frame limit"));
    for(auto& T:Tracks){T.Positions.Reserve(Frames+1);T.Rotations.Reserve(Frames+1);T.Scales.Reserve(Frames+1);}
    TArray<FPose> Cache;Cache.SetNum(Doc.Nodes.Num());TArray<int32> Stamps;Stamps.Init(-1,Doc.Nodes.Num());
    for(int32 F=0;F<=Frames;++F)
    {
        const double Time=double(F)/SampleRate;
        TFunction<const FPose&(int32)> World=[&](int32 I)->const FPose&
        {
            if(Stamps[I]!=F){const int32 P=Doc.Nodes[I].Parent;Cache[I]=Compose(P>=0?World(P):FPose{},Doc.Nodes[I].Sample(Time));Stamps[I]=F;}return Cache[I];
        };
        for(auto& T:Tracks)
        {
            const auto& N=Doc.Nodes[T.Source];FPose Local;
            if(T.bReparented)
            {
                FPose Child=World(T.Source);Child.Q=(Child.Q*Offsets[T.Source]).GetNormalized();Child.S*=OffsetScales[T.Source];
                FPose Parent;
                if(T.SourceTargetParent>=0){Parent=World(T.SourceTargetParent);Parent.Q=(Parent.Q*Offsets[T.SourceTargetParent]).GetNormalized();Parent.S*=OffsetScales[T.SourceTargetParent];}
                Local=Relative(Child,Parent);
            }
            else
            {
                const FPose Native=N.Sample(Time);const FQuat PO=N.Parent>=0?Offsets[N.Parent]:FQuat::Identity;
                const FVector PS=N.Parent>=0?OffsetScales[N.Parent]:FVector::OneVector;
                FVector RotatedScale;
                for(int32 Axis=0;Axis<3;++Axis){FVector Unit=FVector::ZeroVector;Unit[Axis]=1;RotatedScale[Axis]=(Native.S*Offsets[T.Source].RotateVector(Unit)).Length();}
                Local.T=PO.Inverse().RotateVector(Native.T)/PS;Local.Q=(PO.Inverse()*Native.Q*Offsets[T.Source]).GetNormalized();Local.S=RotatedScale*OffsetScales[T.Source]/PS;
            }
            if(Local.T.ContainsNaN()||Local.Q.ContainsNaN()||Local.S.ContainsNaN())return Fail(TEXT("Nonfinite basis compensation result"));
            T.Positions.Add(FVector3f(Local.T));T.Rotations.Add(FQuat4f(Local.Q));T.Scales.Add(FVector3f(Local.S));
        }
    }
    UPackage* Package=CreatePackage(*PackageName);
    UAnimSequence* Anim=FindObject<UAnimSequence>(Package,*AssetName);
    if(!Anim&&FPackageName::DoesPackageExist(PackageName))Anim=LoadObject<UAnimSequence>(nullptr,*(PackageName+TEXT(".")+AssetName));
    const bool bNew=!Anim;
    if(!Anim)Anim=NewObject<UAnimSequence>(Package,*AssetName,RF_Public|RF_Standalone);
    Anim->SetSkeleton(TargetSkeleton);
    auto& Controller=Anim->GetController();if(bNew)Controller.InitializeModel();
    Controller.OpenBracket(FText::FromString(TEXT("Import preserved Leon native motion")),false);
    Controller.RemoveAllBoneTracks(false);Controller.RemoveAllCurvesOfType(ERawCurveTrackTypes::RCT_Float,false);
    Controller.SetFrameRate(FFrameRate(SampleRate,1),false);Controller.SetNumberOfFrames(FFrameNumber(Frames),false);
    bool bKeysOK=true;
    for(auto& T:Tracks){const FName Name=Ref.GetBoneName(T.Target);bKeysOK=Controller.AddBoneCurve(Name,false)&&Controller.SetBoneTrackKeys(Name,T.Positions,T.Rotations,T.Scales,false)&&bKeysOK;}
    TSet<FName> CurveNames;TArray<TSharedPtr<FJsonValue>> MorphAliases,MissingMorphs;bool bMetadataChanged=false;
    auto AddScalarCurve=[&](FName Name,const FChannel& Channel,bool bMorph)
    {
        if(CurveNames.Contains(Name))return;CurveNames.Add(Name);TArray<FRichCurveKey> Keys;Keys.Reserve(Channel.Times.Num());
        for(int32 K=0;K<Channel.Times.Num();++K){FRichCurveKey Key(float(Channel.Times[K]),float(Channel.Values[K].X));Key.InterpMode=Channel.bStep?RCIM_Constant:RCIM_Linear;Keys.Add(Key);}
        const FAnimationCurveIdentifier Id(Name,ERawCurveTrackTypes::RCT_Float);
        bKeysOK=Controller.AddCurve(Id,AACF_DefaultCurve,false)&&Controller.SetCurveKeys(Id,Keys,false)&&bKeysOK;
        if(bMorph&&!TargetSkeleton->GetCurveMetaDataMorphTarget(Name)){TargetSkeleton->AddCurveMetaData(Name,false);TargetSkeleton->SetCurveMetaDataMorphTarget(Name,true);TargetSkeleton->MarkPackageDirty();bMetadataChanged=true;}
    };
    for(const auto& N:Doc.Nodes)if(N.bScalar&&N.Translation.IsSet())
    {
        AddScalarCurve(N.Name,N.Translation.GetValue(),false);
        const FString Prefix=TEXT("HeadSkinned_blendShape.");
        if(N.OriginalName.StartsWith(Prefix))
        {
            FString Alias=N.OriginalName.Mid(Prefix.Len());
            if(PreviewMesh&&!PreviewMesh->FindMorphTarget(FName(*Alias))){FString Sanitized=Alias.Replace(TEXT("."),TEXT("_"));if(PreviewMesh->FindMorphTarget(FName(*Sanitized)))Alias=Sanitized;else MissingMorphs.Add(MakeShared<FJsonValueString>(Alias));}
            AddScalarCurve(FName(*Alias),N.Translation.GetValue(),true);MorphAliases.Add(MakeShared<FJsonValueString>(Alias));
        }
    }
    Controller.NotifyPopulated();Controller.CloseBracket(false);
    if(PreviewMesh)Anim->SetPreviewMesh(PreviewMesh,false);
    if(bNew)FAssetRegistryModule::AssetCreated(Anim);
    Anim->MarkPackageDirty();
    if(!bKeysOK)return Fail(TEXT("Animation controller rejected native track or curve keys"));
    // The preview editor may own an open loader for this shared Skeleton.
    // Save independent sequences here and let the owning editor prepare/save
    // metadata once, instead of rewriting a shared asset in every batch.
    if(bSave&&!SaveAsset(Anim))return Fail(TEXT("Could not save native animation"));
    Report->SetBoolField(TEXT("success"),true);Report->SetStringField(TEXT("asset"),Anim->GetPathName());Report->SetStringField(TEXT("skeleton"),TargetSkeleton->GetPathName());
    Report->SetNumberField(TEXT("skeleton_bones"),BoneCount);Report->SetNumberField(TEXT("duration"),Doc.Duration);Report->SetNumberField(TEXT("fps"),SampleRate);Report->SetNumberField(TEXT("frames"),Frames);
    Report->SetNumberField(TEXT("key_envelope_duration"),Doc.KeyEnvelopeDuration);Report->SetNumberField(TEXT("source_duration_seconds"),Doc.SourceDurationSeconds);
    Report->SetNumberField(TEXT("source_frame_count"),Doc.SourceFrameCount);Report->SetNumberField(TEXT("source_frame_rate"),Doc.SourceFrameRate);
    Report->SetNumberField(TEXT("native_tracks"),Doc.NativeTrackCount);Report->SetNumberField(TEXT("imported_bone_tracks"),Tracks.Num());Report->SetNumberField(TEXT("float_curves"),CurveNames.Num());
    Report->SetArrayField(TEXT("reparented_bones"),Reparented);Report->SetArrayField(TEXT("scalar_carriers"),ScalarNodes);Report->SetArrayField(TEXT("morph_aliases"),MorphAliases);Report->SetArrayField(TEXT("missing_preview_morph_targets"),MissingMorphs);
    Report->SetBoolField(TEXT("saved"),bSave);Report->SetBoolField(TEXT("morph_metadata_needs_save"),bMetadataChanged);Report->SetNumberField(TEXT("seconds"),FPlatformTime::Seconds()-Start);OutReport=JsonText(Report);return Anim;
}

bool ULeonNativeAnimationLibrary::PrepareMorphCurveMetadata(USkeletalMesh* Mesh,USkeleton* TargetSkeleton,bool bSave,FString& OutReport)
{
    using namespace LeonNative;
    auto Report=MakeShared<FJsonObject>();
    if(!Mesh||!TargetSkeleton){Report->SetBoolField(TEXT("success"),false);Report->SetStringField(TEXT("error"),TEXT("Mesh or skeleton missing"));OutReport=JsonText(Report);return false;}
    int32 Added=0;TArray<TSharedPtr<FJsonValue>> Names;
    for(UMorphTarget* Morph:Mesh->GetMorphTargets())if(Morph)
    {
        const FName Name=Morph->GetFName();Names.Add(MakeShared<FJsonValueString>(Name.ToString()));
        if(!TargetSkeleton->GetCurveMetaDataMorphTarget(Name)){TargetSkeleton->AddCurveMetaData(Name,false);TargetSkeleton->SetCurveMetaDataMorphTarget(Name,true);++Added;}
    }
    if(Added)TargetSkeleton->MarkPackageDirty();
    const bool OK=!bSave||SaveAsset(TargetSkeleton);
    Report->SetBoolField(TEXT("success"),OK);Report->SetStringField(TEXT("skeleton"),TargetSkeleton->GetPathName());
    Report->SetNumberField(TEXT("registered_morph_names"),Names.Num());Report->SetNumberField(TEXT("new_metadata"),Added);Report->SetArrayField(TEXT("names"),Names);Report->SetBoolField(TEXT("saved"),bSave&&OK);
    OutReport=JsonText(Report);return OK;
}

bool ULeonNativeAnimationLibrary::AssignMasterSkeleton(USkeletalMesh* Mesh,USkeleton* MasterSkeleton,bool bSave,FString& OutReport)
{
    using namespace LeonNative;
    auto Report=MakeShared<FJsonObject>();
    if(!Mesh||!MasterSkeleton){Report->SetBoolField(TEXT("success"),false);Report->SetStringField(TEXT("error"),TEXT("Missing mesh or master skeleton"));OutReport=JsonText(Report);return false;}
    USkeleton* Previous=Mesh->GetSkeleton();
    const int32 MeshBones=Mesh->GetRefSkeleton().GetNum();
    TArray<TSharedPtr<FJsonValue>> Missing;
    for(int32 I=0;I<MeshBones;++I)if(MasterSkeleton->GetReferenceSkeleton().FindBoneIndex(Mesh->GetRefSkeleton().GetBoneName(I))<0)Missing.Add(MakeShared<FJsonValueString>(Mesh->GetRefSkeleton().GetBoneName(I).ToString()));
    if(!Missing.IsEmpty()){Report->SetBoolField(TEXT("success"),false);Report->SetArrayField(TEXT("missing_mesh_bones"),Missing);OutReport=JsonText(Report);return false;}
    Mesh->Modify();Mesh->SetSkeleton(MasterSkeleton);Mesh->MarkPackageDirty();
    if(Previous&&Previous!=MasterSkeleton){Previous->AddCompatibleSkeleton(MasterSkeleton);MasterSkeleton->AddCompatibleSkeleton(Previous);Previous->MarkPackageDirty();MasterSkeleton->MarkPackageDirty();}
    const bool OK=!bSave||(SaveAsset(Mesh)&&SaveAsset(MasterSkeleton)&&(!Previous||Previous==MasterSkeleton||SaveAsset(Previous)));
    Report->SetBoolField(TEXT("success"),OK);Report->SetStringField(TEXT("mesh"),Mesh->GetPathName());Report->SetStringField(TEXT("master_skeleton"),MasterSkeleton->GetPathName());
    Report->SetNumberField(TEXT("mesh_bones_unchanged"),MeshBones);Report->SetNumberField(TEXT("master_bones"),MasterSkeleton->GetReferenceSkeleton().GetNum());Report->SetBoolField(TEXT("saved"),bSave&&OK);
    OutReport=JsonText(Report);return OK;
}
