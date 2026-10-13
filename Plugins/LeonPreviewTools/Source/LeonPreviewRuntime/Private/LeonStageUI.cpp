#include "LeonStageUI.h"
#include "LeonPreviewController.h"
#include "Styling/CoreStyle.h"
#include "Rendering/DrawElements.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Text/STextBlock.h"

namespace LeonStageStyle
{
static const FSlateBrush* White(){return FCoreStyle::Get().GetBrush("WhiteBrush");}
static FSlateFontInfo Font(int32 Size){return FSlateFontInfo(FPaths::EngineContentDir()/TEXT("Slate/Fonts/DroidSansFallback.ttf"),Size);}
static FLinearColor TextColor(.64f,.64f,.64f,1);
static FLinearColor PanelColor(.003f,.003f,.003f,.93f);
static const FButtonStyle& ButtonStyle()
{
    static FButtonStyle Style=FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder");
    return Style;
}

class SLeonAvailability : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SLeonAvailability){} SLATE_END_ARGS()
    void Construct(const FArguments&){}
    FVector2D ComputeDesiredSize(float) const override{return FVector2D(30,30);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& E,int32 L,const FWidgetStyle&,bool) const override
    {
        const float Size=26.f;
        for(const FVector4 Edge : {FVector4(1,2,Size,1),FVector4(1,2+Size,Size,1),FVector4(1,2,1,Size),FVector4(Size,2,1,Size)})
            FSlateDrawElement::MakeBox(E,L,G.ToPaintGeometry(FVector2D(Edge.Z,Edge.W),FSlateLayoutTransform(FVector2D(Edge.X,Edge.Y))),LeonStageStyle::White(),ESlateDrawEffect::None,FLinearColor(.32f,.32f,.32f,1));
        const TArray<FVector2D> Points={FVector2D(4,16),FVector2D(12,24),FVector2D(25,5)};
        FSlateDrawElement::MakeLines(E,L+1,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,FLinearColor(.55f,.18f,.27f,1),true,1.5f);
        return L+1;
    }
};
}

// Reference-selected rows have square silver edges and horizontal scan lines.
// They are painted in Slate, so their appearance remains sharp at every size.
class SLeonReferenceRow : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLeonReferenceRow):_Available(false){}
        SLATE_ARGUMENT(FString,Label)
        SLATE_ARGUMENT(bool,Available)
        SLATE_ARGUMENT(TFunction<bool()>,Selected)
        SLATE_ARGUMENT(TFunction<void()>,Action)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        IsSelected=Args._Selected;
        ChildSlot[SNew(SButton).IsFocusable(false).ButtonStyle(&LeonStageStyle::ButtonStyle()).ContentPadding(FMargin(15,0))
            .HAlign(HAlign_Left).VAlign(VAlign_Center)
            .OnClicked_Lambda([Action=Args._Action]{if(Action)Action();return FReply::Handled();})
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,9,0)[SNew(SBox).Visibility(Args._Available?EVisibility::Visible:EVisibility::Collapsed)[SNew(LeonStageStyle::SLeonAvailability)]]
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(Args._Label)).Font(LeonStageStyle::Font(18)).ColorAndOpacity(LeonStageStyle::TextColor)]]];
    }
    int32 OnPaint(const FPaintArgs& Args,const FGeometry& Geometry,const FSlateRect& Culling,
        FSlateWindowElementList& Elements,int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
    {
        const FVector2D Size=Geometry.GetLocalSize();
        if((IsSelected&&IsSelected())||IsHovered())
        {
            const int32 Bands=FMath::Max(1,FMath::CeilToInt(Size.Y));
            for(int32 Y=0;Y<Bands;++Y)
            {
                const float Edge=FMath::Abs((float(Y)/Bands)-.5f)*2.f;
                const float Grey=.15f+.15f*Edge+(Y%3==0?.045f:0.f);
                FSlateDrawElement::MakeBox(Elements,Layer,Geometry.ToPaintGeometry(FVector2D(Size.X,1),FSlateLayoutTransform(FVector2D(0,Y))),LeonStageStyle::White(),ESlateDrawEffect::None,FLinearColor(Grey,Grey,Grey,1));
            }
            for(const FVector4 Edge : {FVector4(0,0,Size.X,1),FVector4(0,Size.Y-1,Size.X,1),FVector4(0,0,1,Size.Y),FVector4(Size.X-1,0,1,Size.Y)})
                FSlateDrawElement::MakeBox(Elements,Layer+1,Geometry.ToPaintGeometry(FVector2D(Edge.Z,Edge.W),FSlateLayoutTransform(FVector2D(Edge.X,Edge.Y))),LeonStageStyle::White(),ESlateDrawEffect::None,FLinearColor(.55f,.55f,.55f,1));
        }
        else FSlateDrawElement::MakeBox(Elements,Layer,Geometry.ToPaintGeometry(FVector2D(Size.X,1),FSlateLayoutTransform(FVector2D(0,Size.Y-1))),LeonStageStyle::White(),ESlateDrawEffect::None,FLinearColor(.12f,.12f,.12f,1));
        return SCompoundWidget::OnPaint(Args,Geometry,Culling,Elements,Layer+2,Style,Enabled);
    }
private:
    TFunction<bool()> IsSelected;
};

class SLeonStageUI : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLeonStageUI){} SLATE_ARGUMENT(ALeonPreviewController*,Controller) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Controller=Args._Controller;
        const auto Left=SNew(SVerticalBox);
        Left->AddSlot().AutoHeight()[Label(TEXT("灯光预设"),22)];
        Left->AddSlot().AutoHeight().Padding(0,5,0,12)[Label(TEXT("MetaHuman Creator"),16)];
        Left->AddSlot().AutoHeight()[SNew(STextBlock).Font(LeonStageStyle::Font(17)).ColorAndOpacity(LeonStageStyle::TextColor)
            .Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("灯光旋转  %d°"),FMath::RoundToInt(C()->GetLightRotation())));})];
        Left->AddSlot().AutoHeight().Padding(0,0,0,9)[Slider(-180,180,[this]{return C()->GetLightRotation();},[this](float V){C()->SetLightRotation(V);})];
        Left->AddSlot().AutoHeight()[Row(TEXT("重置灯光角度"),[]{return false;},[this]{C()->SetLightRotation(0);},52)];
        Left->AddSlot().AutoHeight().Padding(0,15,0,0)[Row(TEXT("原图鉴（RE9）"),[this]{return C()->GetLightingPreset()==TEXT("RE9");},[this]{C()->SetLightingPreset(TEXT("RE9"));},50)];
        Left->AddSlot().AutoHeight().Padding(15,8,0,8)[Label(TEXT("Studio"),18)];
        for(const FString Name : {TEXT("Split"),TEXT("Fireside"),TEXT("Moonlight"),TEXT("Tungsten"),TEXT("Portrait"),TEXT("RedLantern"),TEXT("TextureBooth")})
            Left->AddSlot().AutoHeight()[Row(Name,[this,Name]{return C()->GetLightingPreset()==Name;},[this,Name]{C()->SetLightingPreset(Name);},50)];
        Left->AddSlot().AutoHeight().Padding(0,20,0,0)[Label(TEXT("拖动滑条旋转灯光。\n切换预设保留角度，重置恢复\n默认方向。"),12)];

        const auto Right=SNew(SVerticalBox);
        Right->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(50)[SNew(SLeonReferenceRow).Label(TEXT("里昂（夹克）")).Available(true).Selected([]{return true;}).Action([this]{C()->SavePreviewState(TEXT("single_leon_selected"));})]];
        Right->AddSlot().AutoHeight().Padding(0,26,0,10)[Label(TEXT("动画选择"),22)];
        for(const TPair<FString,FString>& Item : TArray<TPair<FString,FString>>{{TEXT("原生展示姿势"),TEXT("native")},{TEXT("绑定姿势"),TEXT("bind")},{TEXT("待机"),TEXT("idle")},{TEXT("行走"),TEXT("walk")},{TEXT("整理头发"),TEXT("comb")}})
            Right->AddSlot().AutoHeight()[Row(Item.Key,[this,Id=Item.Value]{return C()->GetAnimationSelection()==Id;},[this,Id=Item.Value]{C()->SetPreviewAnimation(Id);},48)];
        Right->AddSlot().AutoHeight().Padding(0,15,0,0)[Check(TEXT("骨链物理（RE9 近似）"),[this]{return C()->GetBodyPhysics();},[this](bool V){C()->SetBodyPhysics(V);},false,TEXT("Head_0 驱动的阻尼链，读取原始 chain2 约束并保持角色主体动画。"))];
        Right->AddSlot().AutoHeight()[Check(TEXT("毛发模拟"),[this]{return C()->GetHairSimulation();},[this](bool V){C()->SetHairSimulation(V);})];
        Right->AddSlot().AutoHeight()[Check(TEXT("四控制点曲线插值（RE9 近似）"),[this]{return C()->GetProjectionInterpolation();},[this](bool V){C()->SetProjectionInterpolation(V);},false,TEXT("使用四采样 Catmull-Rom 投影，并将连续性与粘滞参数传给 Groom 求解器。"))];
        Right->AddSlot().AutoHeight()[Check(TEXT("原生 RE9 显示（关闭后使用 UE / 相机）"),[]{return false;},[](bool){},false,TEXT("本关卡使用 Unreal Engine 渲染原始里昂资产。"))];
        Right->AddSlot().AutoHeight().Padding(0,5,0,0)[SNew(STextBlock).Font(LeonStageStyle::Font(14)).ColorAndOpacity(LeonStageStyle::TextColor)
            .Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("HDR 纸白 %d nits"),FMath::RoundToInt(C()->GetHDRPaperWhite())));})];
        Right->AddSlot().AutoHeight().Padding(0,2,0,0)[SNew(STextBlock).Font(LeonStageStyle::Font(11)).ColorAndOpacity(LeonStageStyle::TextColor)
            .Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("RE 链能量 %.2f · 四点投影 %s"), C()->GetREHairChainEnergy(), C()->GetProjectionInterpolation() ? TEXT("ON") : TEXT("OFF")));})];
        Right->AddSlot().AutoHeight()[Slider(80,400,[this]{return C()->GetHDRPaperWhite();},[this](float V){C()->SetHDRPaperWhite(V);})];
        Right->AddSlot().AutoHeight().Padding(0,5,0,0)[Check(TEXT("显示模拟引导线（调试）"),[this]{return C()->GetHairGuides();},[this](bool V){C()->SetHairGuides(V);})];
        Right->AddSlot().AutoHeight().Padding(0,20,0,0)[SNew(SBox).Visibility_Lambda([this]{return bShowHelp?EVisibility::Visible:EVisibility::Collapsed;})
            [Label(TEXT("Q / E  切换模型   R  重置\n左键拖动  旋转模型   右键拖动\n平移\n滚轮  缩放   空格  暂停动画\nEnter  隐藏 / 显示说明"),17)]];
        Right->AddSlot().AutoHeight().Padding(0,20,0,0)[SNew(SExpandableArea).InitiallyCollapsed(true)
            .HeaderContent()[Label(TEXT("全部动画 / 毛发参数"),16)]
            .BodyContent()[Advanced()]];

        const auto Canvas=SNew(SConstraintCanvas)
            +SConstraintCanvas::Slot().Alignment(FVector2D::ZeroVector).Offset(FMargin(59,32,500,58)).AutoSize(false)[Label(TEXT("里昂（夹克）"),30)]
            +SConstraintCanvas::Slot().Alignment(FVector2D::ZeroVector).Offset(FMargin(25,81,286,786)).AutoSize(false)
                [SNew(SBorder).BorderImage(LeonStageStyle::White()).BorderBackgroundColor(LeonStageStyle::PanelColor).Padding(FMargin(20,19,20,18))[Left]]
            +SConstraintCanvas::Slot().Alignment(FVector2D::ZeroVector).Offset(FMargin(1514,81,355,793)).AutoSize(false)
                [SNew(SBorder).BorderImage(LeonStageStyle::White()).BorderBackgroundColor(LeonStageStyle::PanelColor).Padding(FMargin(20,20,16,20))
                    [SNew(SScrollBox).ScrollBarThickness(FVector2D(8,8))+SScrollBox::Slot()[Right]]];
        ChildSlot[SNew(SScaleBox).Stretch(EStretch::ScaleToFit).HAlign(HAlign_Center).VAlign(VAlign_Center)
            [SNew(SBox).WidthOverride(1894).HeightOverride(899)[Canvas]]];
        RefreshAnimations(TEXT(""));
    }
    bool SupportsKeyboardFocus() const override{return true;}
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent& Event) override
    {
        if(Event.GetEffectingButton()==EKeys::LeftMouseButton||Event.GetEffectingButton()==EKeys::RightMouseButton)
        {DragButton=Event.GetEffectingButton();return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this));}
        return FReply::Unhandled();
    }
    FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent&) override{DragButton=FKey();return FReply::Handled().ReleaseMouseCapture();}
    FReply OnMouseMove(const FGeometry&,const FPointerEvent& Event) override
    {
        if(!HasMouseCapture())return FReply::Unhandled();
        const FVector2D D=Event.GetCursorDelta();
        if(DragButton==EKeys::LeftMouseButton)
            C()->SetModelRotation(C()->GetModelYaw()-D.X*.32f,0.0f);
        else if(DragButton==EKeys::RightMouseButton)C()->OffsetCameraPan(D.X,D.Y);
        return FReply::Handled();
    }
    FReply OnMouseWheel(const FGeometry&,const FPointerEvent& Event) override
    {C()->SetCameraOrbit(C()->GetCameraYaw(),C()->GetCameraPitch(),C()->GetCameraDistance()*FMath::Pow(.88f,Event.GetWheelDelta()));return FReply::Handled();}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent& Event) override
    {
        if(Event.GetKey()==EKeys::SpaceBar){C()->SetPlaying(!C()->GetPlaying());return FReply::Handled();}
        if(Event.GetKey()==EKeys::R){C()->ResetCamera();return FReply::Handled();}
        if(Event.GetKey()==EKeys::Enter){bShowHelp=!bShowHelp;return FReply::Handled();}
        if(Event.GetKey()==EKeys::Q||Event.GetKey()==EKeys::E)return FReply::Handled();
        return FReply::Unhandled();
    }
private:
    TWeakObjectPtr<ALeonPreviewController> Controller;
    ALeonPreviewController* C() const{return Controller.Get();}
    bool bShowHelp=true;
    FKey DragButton;
    TArray<TSharedPtr<FString>> Motions;
    TSharedPtr<SListView<TSharedPtr<FString>>> MotionList;
    TSharedPtr<FString> Selected;
    TSharedRef<SWidget> Label(const FString& Text,int32 Size)
    {return SNew(STextBlock).Text(FText::FromString(Text)).Font(LeonStageStyle::Font(Size)).ColorAndOpacity(LeonStageStyle::TextColor).AutoWrapText(true);}
    TSharedRef<SWidget> Row(const FString& Text,TFunction<bool()> SelectedFn,TFunction<void()> Action,float Height)
    {return SNew(SBox).HeightOverride(Height)[SNew(SLeonReferenceRow).Label(Text).Selected(SelectedFn).Action(Action)];}
    TSharedRef<SWidget> Slider(float Min,float Max,TFunction<float()> Get,TFunction<void(float)> Set)
    {return SNew(SBox).HeightOverride(22)[SNew(SSlider).IsFocusable(false).MinValue(Min).MaxValue(Max).StepSize((Max-Min)/360.f).SliderBarColor(FLinearColor(.08f,.08f,.08f,1)).SliderHandleColor(FLinearColor(.65f,.65f,.65f,1)).Value_Lambda([Get]{return Get();}).OnValueChanged_Lambda([Set](float V){Set(V);})];}
    TSharedRef<SWidget> Check(const FString& Text,TFunction<bool()> Get,TFunction<void(bool)> Set,bool Enabled=true,const FString& Tooltip=TEXT(""))
    {return SNew(SBox).HeightOverride(31)[SNew(SCheckBox).IsFocusable(false).CheckBoxContentUsesAutoWidth(false).IsEnabled(Enabled).ToolTipText(FText::FromString(Tooltip)).IsChecked_Lambda([Get]{return Get()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([Set](ECheckBoxState State){Set(State==ECheckBoxState::Checked);})[Label(Text,12)]];}
    TSharedRef<SWidget> Scalar(const FString& Text,float Min,float Max,TFunction<float()> Get,TFunction<void(float)> Set)
    {return SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(LeonStageStyle::Font(14)).ColorAndOpacity(LeonStageStyle::TextColor).Text_Lambda([Text,Get]{return FText::FromString(FString::Printf(TEXT("%s  %.3f"),*Text,Get()));})]+SVerticalBox::Slot().AutoHeight()[Slider(Min,Max,Get,Set)];}
    TSharedRef<SWidget> Advanced()
    {
        const auto Box=SNew(SVerticalBox);
        Box->AddSlot().AutoHeight().Padding(0,8)[Row(TEXT("播放 / 暂停"),[]{return false;},[this]{C()->SetPlaying(!C()->GetPlaying());},36)];
        Box->AddSlot().AutoHeight()[Check(TEXT("循环播放"),[this]{return C()->GetLooping();},[this](bool V){C()->SetLooping(V);})];
        Box->AddSlot().AutoHeight()[Scalar(TEXT("播放速度"),.05f,3,[this]{return C()->GetPlaybackSpeed();},[this](float V){C()->SetPlaybackSpeed(V);})];
        Box->AddSlot().AutoHeight()[Slider(0,1,[this]{return C()->GetAnimationLength()>0?C()->GetAnimationTime()/C()->GetAnimationLength():0;},[this](float V){C()->ScrubAnimation(V);})];
        Box->AddSlot().AutoHeight()[SNew(SSearchBox).HintText(FText::FromString(TEXT("搜索 30,832 个原生动画"))).OnTextChanged_Lambda([this](const FText& T){RefreshAnimations(T.ToString());})];
        Box->AddSlot().AutoHeight().Padding(0,5)[SNew(STextBlock).Font(LeonStageStyle::Font(12)).ColorAndOpacity(LeonStageStyle::TextColor).Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("%d 个匹配动画"),Motions.Num()));})];
        Box->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(200)[SAssignNew(MotionList,SListView<TSharedPtr<FString>>).ListItemsSource(&Motions).OnGenerateRow_Lambda([](TSharedPtr<FString> Item,const TSharedRef<STableViewBase>& Owner){return SNew(STableRow<TSharedPtr<FString>>,Owner)[SNew(STextBlock).Text(FText::FromString(FPaths::GetBaseFilename(*Item))).Font(LeonStageStyle::Font(11)).ToolTipText(FText::FromString(*Item))];}).OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item,ESelectInfo::Type){Selected=Item;})]];
        Box->AddSlot().AutoHeight()[Row(TEXT("播放选中动画"),[]{return false;},[this]{if(Selected)C()->SetPreviewAnimation(*Selected);},36)];
        Box->AddSlot().AutoHeight().Padding(0,12)[Check(TEXT("显示毛发"),[this]{return C()->GetHairVisible();},[this](bool V){C()->SetHairVisible(V);})];
        Box->AddSlot().AutoHeight()[Scalar(TEXT("发丝宽度 (cm)"),.001f,.06f,[this]{return C()->GetGroomWidth();},[this](float V){C()->SetGroomWidth(V);})];
        for(const TPair<FString,FName>& Item : TArray<TPair<FString,FName>>{{TEXT("干发粗糙度"),TEXT("DryRoughness")},{TEXT("湿发程度"),TEXT("WetAmount")},{TEXT("湿发粗糙度"),TEXT("WetRoughness")},{TEXT("高光"),TEXT("Specular")},{TEXT("散射"),TEXT("Scatter")},{TEXT("背光"),TEXT("Backlit")},{TEXT("环境遮蔽"),TEXT("AmbientOcclusion")}})
            Box->AddSlot().AutoHeight()[Scalar(Item.Key,0,1,[this,Name=Item.Value]{return C()->GetGroomScalar(Name);},[this,Name=Item.Value](float V){C()->SetGroomScalar(Name,V);})];
        Box->AddSlot().AutoHeight()[Row(TEXT("重置毛发参数"),[]{return false;},[this]{C()->ResetGroom();},40)];
        return Box;
    }
    void RefreshAnimations(const FString& Search)
    {Motions.Reset();if(C())for(const FString& Path:C()->GetAnimationPaths())if(Search.IsEmpty()||Path.Contains(Search))Motions.Add(MakeShared<FString>(Path));if(MotionList)MotionList->RequestListRefresh();}
};

TSharedRef<SWidget> MakeLeonStageUI(ALeonPreviewController* Controller)
{return SNew(SLeonStageUI).Controller(Controller);}
