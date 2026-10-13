#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "EngineUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "GroomComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Text/STextBlock.h"

static const FName LeonTabName(TEXT("LeonPreview"));
struct FLeonMotion { FAssetData Asset; FString Path; };

class SLeonPreviewPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLeonPreviewPanel) {} SLATE_END_ARGS()
    void Construct(const FArguments&)
    {
        CaptureLights(); RefreshMotions();
        TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).Text(FText::FromString(TEXT("LEON / Native character studio")))];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Status);})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).Text(FText::FromString(TEXT("Lighting")))];
        TSharedRef<SWrapBox> Presets=SNew(SWrapBox).UseAllottedSize(true);
        for (const FString Name : {TEXT("Split"),TEXT("Fireside"),TEXT("Moonlight"),TEXT("Tungsten"),TEXT("Portrait"),TEXT("RedLantern"),TEXT("TextureBooth")})
            Presets->AddSlot().Padding(2)[Button(Name,[this,Name]{ApplyPreset(Name);})];
        Panel->AddSlot().AutoHeight().Padding(4)[Presets];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Light rotation (degrees)"),-180,180,[this]{return LightYaw;},[this](float V){LightYaw=V;RotateLights();})];
        Panel->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Reset light angle"),[this]{LightYaw=0;RotateLights();})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).Text(FText::FromString(TEXT("Camera")))];
        TSharedRef<SWrapBox> CameraButtons=SNew(SWrapBox).UseAllottedSize(true);
        CameraButtons->AddSlot().Padding(2)[Button(TEXT("Portrait"),[this]{bPortraitCamera=true;Distance=150;TargetHeight=168;CameraYaw=90;CameraPitch=0;UpdateCamera();})];
        CameraButtons->AddSlot().Padding(2)[Button(TEXT("Full body"),[this]{FrameFullBody();})];
        CameraButtons->AddSlot().Padding(2)[Button(TEXT("Reset camera"),[this]{FrameFullBody();})];
        Panel->AddSlot().AutoHeight().Padding(4)[CameraButtons];
        Panel->AddSlot().AutoHeight().Padding(4)[Check(TEXT("Follow animation movement"),[this]{return bFollowMovement;},[this](bool V){bFollowMovement=V;UpdateCamera();})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Orbit yaw"),-180,180,[this]{return CameraYaw;},[this](float V){CameraYaw=V;UpdateCamera();})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Orbit elevation"),-65,65,[this]{return CameraPitch;},[this](float V){CameraPitch=V;UpdateCamera();})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Distance (cm)"),35,2000,[this]{return Distance;},[this](float V){Distance=V;UpdateCamera();})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).Text(FText::FromString(TEXT("Animations / actual imported assets")))];
        TSharedRef<SWrapBox> MotionButtons=SNew(SWrapBox).UseAllottedSize(true);
        MotionButtons->AddSlot().Padding(2)[Button(TEXT("idle_caution · generald / 0"),[this]{PlayNative(TEXT("generald"),0);})];
        MotionButtons->AddSlot().Padding(2)[Button(TEXT("walk_loop · generald / 2270"),[this]{PlayNative(TEXT("generald"),2270);})];
        MotionButtons->AddSlot().Padding(2)[Button(TEXT("comb hair · emote / 710"),[this]{PlayNative(TEXT("emote"),710);})];
        Panel->AddSlot().AutoHeight().Padding(4)[MotionButtons];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(SSearchBox).HintText(FText::FromString(TEXT("Search full asset path, motion bank, or native ID"))).OnTextChanged_Lambda([this](const FText& T){Search=T.ToString();FilterMotions();})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("%d matching / %d imported animation sequences"),Filtered.Num(),All.Num()));})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(SBox).HeightOverride(220)[SAssignNew(MotionList,SListView<TSharedPtr<FLeonMotion>>).ListItemsSource(&Filtered).SelectionMode(ESelectionMode::Single).OnGenerateRow_Lambda([](TSharedPtr<FLeonMotion> Item,const TSharedRef<STableViewBase>& Owner){return SNew(STableRow<TSharedPtr<FLeonMotion>>,Owner)[SNew(STextBlock).Text(FText::FromName(Item->Asset.AssetName)).ToolTipText(FText::FromString(Item->Path))];}).OnSelectionChanged_Lambda([this](TSharedPtr<FLeonMotion> Item,ESelectInfo::Type){Selected=Item;})]];
        TSharedRef<SWrapBox> Playback=SNew(SWrapBox).UseAllottedSize(true);
        Playback->AddSlot().Padding(2)[Button(TEXT("Play selected"),[this]{PlaySelected();})];
        Playback->AddSlot().Padding(2)[Button(TEXT("Play / pause"),[this]{TogglePlay();})];
        Playback->AddSlot().Padding(2)[Button(TEXT("Bind pose"),[this]{BindPose();})];
        Playback->AddSlot().Padding(2)[Button(TEXT("Refresh assets"),[this]{RefreshMotions();})];
        Playback->AddSlot().Padding(2)[Button(TEXT("Show selected asset"),[this]{BrowseSelected();})];
        Panel->AddSlot().AutoHeight().Padding(4)[Playback];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(SCheckBox).IsChecked_Lambda([this]{return bLoop?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState S){bLoop=S==ECheckBoxState::Checked;if(auto I=AnimInstance())I->SetLooping(bLoop);})[SNew(STextBlock).Text(FText::FromString(TEXT("Loop")))]];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Playback speed"),0.05,3,[this]{return Speed;},[this](float V){Speed=V;if(auto I=AnimInstance())I->SetPlayRate(Speed);})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(SSlider).MinValue(0).MaxValue(1).Value_Lambda([this]{auto I=AnimInstance();return I&&I->GetLength()>0?I->GetCurrentTime()/I->GetLength():0.f;}).OnValueChanged_Lambda([this](float V){Scrub(V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{auto I=AnimInstance();return FText::FromString(I?FString::Printf(TEXT("%.3f / %.3f seconds"),I->GetCurrentTime(),I->GetLength()):TEXT("No animation selected"));})];
        Panel->AddSlot().AutoHeight().Padding(4)[SNew(STextBlock).Text(FText::FromString(TEXT("Native Groom")))];
        Panel->AddSlot().AutoHeight().Padding(4)[Check(TEXT("Visible"),[this]{return bHairVisible;},[this](bool V){bHairVisible=V;if(auto H=Hair())H->SetVisibility(V,true);})];
        Panel->AddSlot().AutoHeight().Padding(4)[Check(TEXT("Simulation"),[this]{return bSimulation;},[this](bool V){bSimulation=V;if(auto H=Hair()){H->SetEnableSimulation(V);H->ResetSimulation();Status=V?TEXT("Groom dynamics enabled; inspect collisions before playback."):TEXT("Groom dynamics disabled.");}})];
        Panel->AddSlot().AutoHeight().Padding(4)[Check(TEXT("Show simulation guides"),[this]{return bGuides;},[this](bool V){bGuides=V;if(auto C=IConsoleManager::Get().FindConsoleVariable(TEXT("r.HairStrands.ViewMode")))C->Set(V?21:0,ECVF_SetByConsole);Redraw();})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Strand width (cm)"),0.001,0.06,[this]{return Width;},[this](float V){Width=V;if(auto H=Hair()){H->SetHairWidth(V);H->MarkRenderStateDirty();}Redraw();})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Dry roughness"),0.02,1,[this]{return Roughness;},[this](float V){Roughness=V;HairScalar(TEXT("DryRoughness"),V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Wet amount"),0,1,[this]{return Wet;},[this](float V){Wet=V;HairScalar(TEXT("WetAmount"),V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Wet roughness"),0.02f,1,[this]{return WetRoughness;},[this](float V){WetRoughness=V;HairScalar(TEXT("WetRoughness"),V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Specular"),0,1,[this]{return Specular;},[this](float V){Specular=V;HairScalar(TEXT("Specular"),V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Scatter"),0,1,[this]{return Scatter;},[this](float V){Scatter=V;HairScalar(TEXT("Scatter"),V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Backlit"),0,1,[this]{return Backlit;},[this](float V){Backlit=V;HairScalar(TEXT("Backlit"),V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[FloatControl(TEXT("Ambient occlusion"),0,1,[this]{return Occlusion;},[this](float V){Occlusion=V;HairScalar(TEXT("AmbientOcclusion"),V);})];
        Panel->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Reset Groom settings"),[this]{Width=.02f;Roughness=.42f;Wet=0;WetRoughness=.05f;Specular=.4f;Scatter=.35f;Backlit=1;Occlusion=.6f;HairScalar(TEXT("DryRoughness"),Roughness);HairScalar(TEXT("WetAmount"),Wet);HairScalar(TEXT("WetRoughness"),WetRoughness);HairScalar(TEXT("Specular"),Specular);HairScalar(TEXT("Scatter"),Scatter);HairScalar(TEXT("Backlit"),Backlit);HairScalar(TEXT("AmbientOcclusion"),Occlusion);if(auto H=Hair()){H->SetHairWidth(Width);H->SetVisibility(true,true);H->SetEnableSimulation(false);}bHairVisible=true;bSimulation=false;bGuides=false;if(auto C=IConsoleManager::Get().FindConsoleVariable(TEXT("r.HairStrands.ViewMode")))C->Set(0,ECVF_SetByConsole);Redraw();})];
        ChildSlot[SNew(SScrollBox)+SScrollBox::Slot()[Panel]];
        // The tab can be constructed before the level viewport has a real size.
        // Defer the first fit until the active timer sees valid viewport bounds.
        bInitialCameraFramePending=true;
        RegisterActiveTimer(1.f/30.f,FWidgetActiveTimerDelegate::CreateSP(this,&SLeonPreviewPanel::TrackCamera));
    }
private:
    TArray<TSharedPtr<FLeonMotion>> All,Filtered;
    TSharedPtr<FLeonMotion> Selected;
    TSharedPtr<SListView<TSharedPtr<FLeonMotion>>> MotionList;
    FString Search,Status=TEXT("Open LeonAssembledPreview. Controls affect the active editor scene.");
    TMap<FString,FRotator> BaseLightRotations;
    float LightYaw=0,CameraYaw=90,CameraPitch=0,Distance=310,TargetHeight=95,Speed=1,Width=.02f,Roughness=.42f,Wet=0;
    float WetRoughness=.05f,Specular=.4f,Scatter=.35f,Backlit=1,Occlusion=.6f;
    bool bLoop=true,bPlaying=false,bHairVisible=true,bSimulation=false,bGuides=false;
    bool bFollowMovement=true,bPortraitCamera=false,bInitialCameraFramePending=true;
    UWorld* World() const {return GEditor?GEditor->GetEditorWorldContext().World():nullptr;}
    AActor* Actor(const FString& Label) const {if(auto W=World())for(TActorIterator<AActor> I(W);I;++I)if(I->GetActorLabel()==Label)return *I;return nullptr;}
    USkeletalMeshComponent* Hero() const {auto A=Actor(TEXT("Leon_Assembled_Hero"));return A?A->FindComponentByClass<USkeletalMeshComponent>():nullptr;}
    UGroomComponent* Hair() const {if(auto W=World())for(TActorIterator<AActor> I(W);I;++I)if(auto H=I->FindComponentByClass<UGroomComponent>())return H;return nullptr;}
    UAnimSingleNodeInstance* AnimInstance() const {auto C=Hero();return C?C->GetSingleNodeInstance():nullptr;}
    void Redraw() {if(GEditor)GEditor->RedrawLevelEditingViewports();}
    TSharedRef<SWidget> Button(const FString& Label,TFunction<void()> Action) {return SNew(SButton).Text(FText::FromString(Label)).OnClicked_Lambda([Action]{Action();return FReply::Handled();});}
    TSharedRef<SWidget> Check(const FString& Label,TFunction<bool()> Get,TFunction<void(bool)> Set) {return SNew(SCheckBox).IsChecked_Lambda([Get]{return Get()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([Set](ECheckBoxState State){Set(State==ECheckBoxState::Checked);})[SNew(STextBlock).Text(FText::FromString(Label))];}
    TSharedRef<SWidget> FloatControl(const FString& Label,float Min,float Max,TFunction<float()> Get,TFunction<void(float)> Set) {return SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Label))]+SVerticalBox::Slot().AutoHeight()[SNew(SSpinBox<float>).MinValue(Min).MaxValue(Max).MinSliderValue(Min).MaxSliderValue(Max).Delta((Max-Min)/100.f).Value_Lambda([Get]{return Get();}).OnValueChanged_Lambda([Set](float Value){Set(Value);})];}
    void CaptureLights() {for(const FString N:{TEXT("Key_Split"),TEXT("Fill_Softbox"),TEXT("Rim_Hair")})if(auto A=Actor(N))BaseLightRotations.Add(N,A->GetActorRotation());}
    void RotateLights() {if(BaseLightRotations.IsEmpty())CaptureLights();for(const auto& P:BaseLightRotations)if(auto A=Actor(P.Key)){FRotator R=P.Value;R.Yaw+=LightYaw;A->SetActorRotation(R);}Redraw();}
    void ApplyPreset(const FString& Name)
    {
        FLinearColor K(1,.78,.62),F(.55,.68,1),R(.7,.82,1);float Key=7,Fill=900,Rim=1300,Sky=.6f;
        if(Name==TEXT("Fireside")){K=FLinearColor(1,.35,.07);F=FLinearColor(.18,.28,.55);R=FLinearColor(1,.62,.2);Key=6;Fill=420;Rim=1500;Sky=.18f;}
        else if(Name==TEXT("Moonlight")){K=FLinearColor(.32,.52,1);F=FLinearColor(.12,.18,.35);R=FLinearColor(.65,.82,1);Key=3;Fill=300;Rim=1100;Sky=.22f;}
        else if(Name==TEXT("Tungsten")){K=FLinearColor(1,.62,.28);F=FLinearColor(1,.76,.48);R=FLinearColor(.65,.78,1);Key=8;Fill=780;Rim=1200;Sky=.35f;}
        else if(Name==TEXT("Portrait")){K=FLinearColor(1,.9,.82);F=FLinearColor(.72,.82,1);R=FLinearColor(1,.86,.75);Key=6;Fill=1200;Rim=1500;Sky=.7f;}
        else if(Name==TEXT("RedLantern")){K=FLinearColor(1,.06,.025);F=FLinearColor(.18,.32,.65);R=FLinearColor(1,.28,.08);Key=5;Fill=600;Rim=1800;Sky=.15f;}
        else if(Name==TEXT("TextureBooth")){K=F=R=FLinearColor::White;Key=5;Fill=1500;Rim=1000;Sky=1;}
        // Measured against the final scene at manual EV 0.
        Key*=.15f;Fill*=.003f;Rim*=.004f;
        const TArray<FString> Names={TEXT("Key_Split"),TEXT("Fill_Softbox"),TEXT("Rim_Hair")};const TArray<FLinearColor> Colors={K,F,R};const TArray<float> Powers={Key,Fill,Rim};
        for(int32 i=0;i<Names.Num();++i)if(auto A=Actor(Names[i]))if(auto C=A->FindComponentByClass<ULightComponent>()){C->SetLightColor(Colors[i],false);C->SetIntensity(Powers[i]);}
        if(auto A=Actor(TEXT("Sky_Studio")))if(auto C=A->FindComponentByClass<USkyLightComponent>())C->SetIntensity(Sky);
        Status=TEXT("Lighting preset: ")+Name;RotateLights();
    }
    void UpdateCamera()
    {
        auto V=GCurrentLevelEditingViewportClient;if(!V){Status=TEXT("Activate the level viewport to adjust its camera.");return;}
        FVector Target(0,0,TargetHeight);
        if(auto H=Hero())
        {
            Target+=H->GetComponentLocation();
            if(bFollowMovement)
            {
                if(bPortraitCamera&&H->DoesSocketExist(TEXT("Head_0")))Target=H->GetSocketLocation(TEXT("Head_0"))+FVector(0,0,3.2f);
                else if(H->DoesSocketExist(TEXT("root")))Target=H->GetSocketLocation(TEXT("root"))+FVector(0,0,TargetHeight);
            }
        }
        const float Y=FMath::DegreesToRadians(CameraYaw),P=FMath::DegreesToRadians(CameraPitch);
        FVector Pos=Target+FVector(FMath::Cos(P)*FMath::Cos(Y),FMath::Cos(P)*FMath::Sin(Y),FMath::Sin(P))*Distance;
        V->SetViewLocation(Pos);V->SetViewRotation((Target-Pos).Rotation());V->ViewFOV=35;V->SetRealtime(true);V->Invalidate();
    }
    EActiveTimerReturnType TrackCamera(double,float)
    {
        if(bInitialCameraFramePending)
        {
            if(auto V=GCurrentLevelEditingViewportClient)
                if(V->Viewport && V->Viewport->GetSizeXY().X>0 && V->Viewport->GetSizeXY().Y>0)
                {
                    bInitialCameraFramePending=false;
                    FrameFullBody();
                }
        }
        if(bFollowMovement)UpdateCamera();
        return EActiveTimerReturnType::Continue;
    }
    void FrameFullBody()
    {
        bInitialCameraFramePending=false;
        float Aspect=16.f/9.f;
        if(auto V=GCurrentLevelEditingViewportClient)if(V->Viewport)
        {
            const FIntPoint Size=V->Viewport->GetSizeXY();
            if(Size.Y>0)Aspect=float(Size.X)/Size.Y;
        }
        bPortraitCamera=false;Distance=FMath::Clamp(110.f*Aspect/FMath::Tan(FMath::DegreesToRadians(17.5f)),250.f,1800.f);
        if(auto V=GCurrentLevelEditingViewportClient)V->SetGameView(true);
        TargetHeight=95;CameraYaw=90;CameraPitch=0;UpdateCamera();
    }
    void RefreshMotions()
    {
        All.Reset();FARFilter Filter;Filter.PackagePaths.Add(FName(TEXT("/Game/Leon/Animations/FullRig")));Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());Filter.bRecursivePaths=true;Filter.bRecursiveClasses=true;
        auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanPathsSynchronous({TEXT("/Game/Leon/Animations/FullRig")},true);
        TArray<FAssetData> Assets;Registry.GetAssets(Filter,Assets);
        for(const FAssetData& A:Assets){auto Entry=MakeShared<FLeonMotion>();Entry->Asset=A;Entry->Path=A.GetSoftObjectPath().ToString();All.Add(Entry);}
        All.Sort([](const TSharedPtr<FLeonMotion>& A,const TSharedPtr<FLeonMotion>& B){return A->Path<B->Path;});FilterMotions();
        Status=FString::Printf(TEXT("Animation registry refreshed: %d sequences."),All.Num());
    }
    void FilterMotions() {Filtered.Reset();for(auto E:All)if(Search.IsEmpty()||E->Path.Contains(Search))Filtered.Add(E);if(MotionList)MotionList->RequestListRefresh();}
    void PlaySelected()
    {
        if(!Selected){Status=TEXT("Select an actual animation asset first.");return;}auto C=Hero();if(!C){Status=TEXT("Leon_Assembled_Hero is missing from the current level.");return;}
        auto A=Cast<UAnimSequence>(Selected->Asset.GetAsset());if(!A){Status=TEXT("The selected asset is unavailable; refresh the registry.");return;}
        C->SetUpdateAnimationInEditor(true);C->SetComponentTickEnabled(true);C->bPauseAnims=false;C->PlayAnimation(A,bLoop);if(auto I=C->GetSingleNodeInstance())I->SetPlayRate(Speed);
        if(GCurrentLevelEditingViewportClient)GCurrentLevelEditingViewportClient->SetRealtime(true);bPlaying=true;Status=TEXT("Playing: ")+Selected->Path;Redraw();
    }
    void PlayNative(const FString& Bank,int32 Id)
    {
        RefreshMotions();const FString Native=FString::Printf(TEXT("编号%d"),Id);const FString Wrapper=FString::Printf(TEXT("__%d/"),Id);const FString FullId=FString::Printf(TEXT("_id%d"),Id);
        for(auto Entry:All)if(Entry->Path.Contains(Bank)&&(Entry->Path.Contains(Native+TEXT("_"))||Entry->Path.Contains(Native+TEXT("."))||Entry->Path.Contains(Wrapper)||Entry->Path.Contains(FullId+TEXT("_"))||Entry->Path.Contains(FullId+TEXT("."))||Entry->Path.Contains(FullId+TEXT("/")))){Selected=Entry;if(MotionList)MotionList->SetSelection(Selected);PlaySelected();return;}
        Status=FString::Printf(TEXT("Native %s / %d is not yet imported. Refresh after its batch completes."),*Bank,Id);
    }
    void TogglePlay() {if(auto I=AnimInstance()){bPlaying=!bPlaying;I->SetPlaying(bPlaying);if(auto C=Hero())C->bPauseAnims=!bPlaying;Redraw();}}
    void Scrub(float Fraction) {if(auto I=AnimInstance()){I->SetPlaying(false);I->SetPosition(Fraction*I->GetLength(),false);bPlaying=false;if(auto C=Hero()){C->bPauseAnims=false;C->TickAnimation(0,false);C->RefreshBoneTransforms();C->bPauseAnims=true;C->MarkRenderTransformDirty();}Redraw();}}
    void BindPose()
    {
        if(auto C=Hero())
        {
            auto Mesh=C->GetSkeletalMeshAsset();if(!Mesh)return;
            TArray<TPair<int32,int32>> HiddenSections;
            for(int32 Lod=0;Lod<Mesh->GetLODNum();++Lod)
                for(int32 Material=0;Material<Mesh->GetMaterials().Num();++Material)
                    if(!C->IsMaterialSectionShown(Material,Lod))HiddenSections.Emplace(Material,Lod);
            C->bPauseAnims=false;C->SetAnimation(nullptr);C->SetSkeletalMeshAsset(nullptr);C->SetSkeletalMeshAsset(Mesh);
            for(const auto& Section:HiddenSections)C->ShowMaterialSection(Section.Key,INDEX_NONE,false,Section.Value);
            C->SetUpdateAnimationInEditor(true);C->SetAnimationMode(EAnimationMode::AnimationSingleNode);C->SetAnimation(nullptr);
            C->TickAnimation(0,false);C->RefreshBoneTransforms();C->bPauseAnims=false;bPlaying=false;
            Status=TEXT("Native skeleton bind pose");Redraw();
        }
    }
    void BrowseSelected() {if(Selected)FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{Selected->Asset});}
    void HairScalar(const FName Parameter,float Value)
    {
        if(auto H=Hair()){for(int32 i=0;i<H->GetNumMaterials();++i){auto M=Cast<UMaterialInstanceDynamic>(H->GetMaterial(i));if(!M)M=H->CreateDynamicMaterialInstance(i);if(M)M->SetScalarParameterValue(Parameter,Value);}H->MarkRenderStateDirty();Status=Parameter.ToString()+FString::Printf(TEXT(" = %.3f"),Value);Redraw();}
        else Status=TEXT("No Groom component in the current level.");
    }
};

class FLeonPreviewToolsModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if(IsRunningCommandlet())return;
        bRegistered=true;
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LeonTabName,FOnSpawnTab::CreateRaw(this,&FLeonPreviewToolsModule::Spawn)).SetDisplayName(FText::FromString(TEXT("Leon Preview"))).SetMenuType(ETabSpawnerMenuType::Hidden);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,&FLeonPreviewToolsModule::RegisterMenu));
        OpenCommand=IConsoleManager::Get().RegisterConsoleCommand(TEXT("LeonPreview.Open"),TEXT("Open the Leon Preview control panel"),FConsoleCommandDelegate::CreateLambda([]{FGlobalTabmanager::Get()->TryInvokeTab(LeonTabName);}),ECVF_Default);
    }
    virtual void ShutdownModule() override {if(!bRegistered)return;if(OpenCommand)IConsoleManager::Get().UnregisterConsoleObject(OpenCommand);UToolMenus::UnRegisterStartupCallback(this);UToolMenus::UnregisterOwner(this);FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LeonTabName);}
    TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&) {return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SLeonPreviewPanel)];}
    void RegisterMenu()
    {
        FToolMenuOwnerScoped Owner(this);auto Menu=UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
        Menu->FindOrAddSection(TEXT("WindowLayout")).AddMenuEntry(TEXT("LeonPreviewTools"),FText::FromString(TEXT("Leon Preview")),FText::FromString(TEXT("Open live studio, animation and native Groom controls")),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([]{FGlobalTabmanager::Get()->TryInvokeTab(LeonTabName);}))); 
    }
private:
    bool bRegistered=false;
    IConsoleObject* OpenCommand=nullptr;
};
IMPLEMENT_MODULE(FLeonPreviewToolsModule,LeonPreviewTools)
