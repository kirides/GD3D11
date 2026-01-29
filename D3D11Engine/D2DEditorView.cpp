#include "pch.h"
#include "zCMaterial.h"
#include "D2DEditorView.h"
#include "D2DView.h"
#include "SV_Border.h"
#include "SV_Panel.h"
#include "SV_Button.h"
#include "SV_Checkbox.h"
#include "SV_Label.h"
#include "SV_Slider.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "BaseLineRenderer.h"
#include "SV_TabControl.h"
#include "GVegetationBox.h"
#include "oCGame.h"
#include "zCVob.h"
#include "zCCamera.h"
#include "MeshModifier.h"
#include "BaseAntTweakBar.h"
#include "D3D7\MyDirectDrawSurface7.h"
#include "D3D11Texture.h"
#include "D2DVobSettingsDialog.h"
#include "zCOption.h"
#include "SV_NamedSlider.h"
#include "zCModel.h"
#include "D2DView.h"
#include "Widget_TransRot.h"
#include "WidgetContainer.h"

D2DEditorView::D2DEditorView( D2DView* view, D2DSubView* parent ) : D2DSubView( view, parent ) {
	InitControls();

	IsEnabled = false;

	Mode = EM_IDLE;
	DraggedBoxMinLocal = XMFLOAT3( -700, -500, -700 );
	DraggedBoxMaxLocal = XMFLOAT3( 700, 500, 700 );
	DraggedBoxCenter = XMFLOAT3( 0, 0, 0 );

	memset( SelectedTriangle, 0, sizeof( SelectedTriangle ) );
	memset( MButtons, 0, sizeof( MButtons ) );

	memset( Keys, 0, sizeof( Keys ) );

	Mode = EM_IDLE;

	Selection.Reset();

	MPrevCursor = nullptr;
	TracedMesh = nullptr;
	TracedMaterial = nullptr;
	TracedVobInfo = nullptr;
	TracedSkeletalVobInfo = nullptr;
	CPitch = 0.0f;
	CYaw = 0.0f;
	VegLastUniformScale = 1.0f;

	TracedVegetationBox = nullptr;

	CStartMousePosition.x = CStartMousePosition.y = 0;
	MMovedAfterClick = false;

	SelectedSomething = false;

	Widgets = new WidgetContainer;
}


D2DEditorView::~D2DEditorView() {
	delete Widgets;
}

/** Initializes the controls of this view */
XRESULT D2DEditorView::InitControls() {
	SV_Panel* subPanel = new SV_Panel( MainView, this );
	subPanel->SetRect( D2D1::RectF( 0, 0, 290, MainView->GetRenderTarget()->GetSize().height ) );
	subPanel->SetPanelShadow( true, 50.0f );
	MainPanel = subPanel;

	/** Vob settings */
	VobSettingsDialog = new D2DVobSettingsDialog( MainView, this );
	VobSettingsDialog->SetHidden( true );

	/** Save/Load */
	float thirdSize = (290 - 5 * 5) / 3.0f;
	SV_Button* saveLevelButton = new SV_Button( MainView, subPanel );
	saveLevelButton->SetPositionAndSize( D2D1::Point2F( 5, MainView->GetRenderTarget()->GetSize().height - 30 ), D2D1::SizeF( thirdSize, 25 ) );
	saveLevelButton->SetPressedCallback( SaveLevelPressed, this );
	saveLevelButton->SetCaption( "Save level" );

	SV_Button* loadLevelButton = new SV_Button( MainView, subPanel );
	loadLevelButton->SetPositionAndSize( D2D1::Point2F( 5, MainView->GetRenderTarget()->GetSize().height - 30 ), D2D1::SizeF( thirdSize, 25 ) );
	loadLevelButton->SetPressedCallback( LoadLevelPressed, this );
	loadLevelButton->SetCaption( "Load level" );
	loadLevelButton->AlignRightTo( saveLevelButton, 5 );

	/*SV_Button * infoButton = new SV_Button(MainView, subPanel);
	infoButton->SetPositionAndSize(D2D1::Point2F(5, MainView->GetRenderTarget()->GetSize().height - 30), D2D1::SizeF(thirdSize, 25));
	infoButton->SetPressedCallback(InfoPressed, this);
	infoButton->SetCaption("Info");
	infoButton->AlignRightTo(loadLevelButton, 5);*/

	//SV_Panel * vegPanel = new SV_Panel(MainView, subPanel);
	//vegPanel->SetRect(D2D1::RectF(20, 20, 270, 180));
	//vegPanel->SetPanelShadow(true, 20.0f);

	//SV_Button * subButton = new SV_Button(MainView, subPanel);
	//subButton->SetPositionAndSize(D2D1::Point2F(20, 30), D2D1::SizeF(70, 40));

	// Test Tab-Control
	MainTabControl = new SV_TabControl( MainView, subPanel );
	MainTabControl->SetSize( D2D1::SizeF( 200, 300 ) );
	MainTabControl->SetRect( D2D1::RectF( 20, 20, 270, 180 ) );

	MainTabControl->SetTabSwitchedCallback( MainTabSwitched, this );

	/** Vegetation placement */
	SV_Button* addVegButton = new SV_Button( MainView, MainTabControl->GetTabPanel() );
	addVegButton->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 100, 25 ) );
	addVegButton->SetPressedCallback( AddVegButtonPressed, this );
	addVegButton->SetCaption( "Place Volume" );
	MainTabControl->AddControlToTab( addVegButton, "Vegetation" );

	SV_Button* fillVegButton = new SV_Button( MainView, MainTabControl->GetTabPanel() );
	fillVegButton->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 100, 25 ) );
	fillVegButton->SetPressedCallback( FillVegButtonPressed, this );
	fillVegButton->AlignRightTo( addVegButton, 10.0f );
	fillVegButton->SetCaption( "Fill Selection" );
	MainTabControl->AddControlToTab( fillVegButton, "Vegetation" );


	VegRestrictByTextureCheckBox = new SV_Checkbox( MainView, MainTabControl->GetTabPanel() );
	VegRestrictByTextureCheckBox->SetSize( D2D1::SizeF( 160, 20 ) );
	VegRestrictByTextureCheckBox->AlignUnder( addVegButton, 8.0f );
	VegRestrictByTextureCheckBox->SetCaption( L"Texture aware" );
	MainTabControl->AddControlToTab( VegRestrictByTextureCheckBox, "Vegetation" );

	VegCircularShapeCheckBox = new SV_Checkbox( MainView, MainTabControl->GetTabPanel() );
	VegCircularShapeCheckBox->SetSize( D2D1::SizeF( 160, 20 ) );
	VegCircularShapeCheckBox->AlignUnder( VegRestrictByTextureCheckBox, 5.0f );
	VegCircularShapeCheckBox->SetCaption( L"Circular shape" );
	MainTabControl->AddControlToTab( VegCircularShapeCheckBox, "Vegetation" );

	SV_Button* removeVegButton = new SV_Button( MainView, MainTabControl->GetTabPanel() );
	removeVegButton->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 100, 25 ) );
	removeVegButton->SetPressedCallback( RemoveVegButtonPressed, this );
	removeVegButton->AlignUnder( VegCircularShapeCheckBox, 30.0f );
	removeVegButton->SetCaption( "Remove" );
	MainTabControl->AddControlToTab( removeVegButton, "Vegetation" );

	/** Selection */
	SelectTrianglesOnlyCheckBox = new SV_Checkbox( MainView, MainTabControl->GetTabPanel() );
	SelectTrianglesOnlyCheckBox->SetSize( D2D1::SizeF( 160, 20 ) );
	SelectTrianglesOnlyCheckBox->SetPosition( D2D1::Point2F( 10, 10 ) );
	SelectTrianglesOnlyCheckBox->SetCaption( L"Select Triangles Only CheckBox" );
	MainTabControl->AddControlToTab( SelectTrianglesOnlyCheckBox, "Selection" );
	SelectTrianglesOnlyCheckBox->SetChecked( false );

	SV_Button* testbutton1 = new SV_Button( MainView, MainTabControl->GetTabPanel() );
	testbutton1->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 120, 25 ) );
	testbutton1->SetCaption( "Vegetation" );
	MainTabControl->AddControlToTab( testbutton1, "Test 1" );

	SV_Button* testbutton2 = new SV_Button( MainView, MainTabControl->GetTabPanel() );
	testbutton2->SetPositionAndSize( D2D1::Point2F( 10, 40 ), D2D1::SizeF( 120, 25 ) );
	testbutton2->SetCaption( "Test 2" );
	MainTabControl->AddControlToTab( testbutton2, "Test 2" );

	MainTabControl->SetActiveTab( "Vegetation" );

	// Selection tab
	SelectionTabControl = new SV_TabControl( MainView, subPanel );
	SelectionTabControl->SetRect( D2D1::RectF( 20, 20, 250, 280 ) );
	SelectionTabControl->SetSize( D2D1::SizeF( 270, 380 ) );
	SelectionTabControl->AlignUnder( MainTabControl, 40.0f );
	SelectionTabControl->SetOnlyShowActiveTab( true );
	//SelectionTabControl->SetTabSwitchedCallback(MainTabSwitched, this);

	SelectedImagePanel = new SV_Panel( MainView, SelectionTabControl->GetTabPanel() );
	SelectedImagePanel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 128, 128 ) );
	SelectedImagePanel->SetRenderMode( SV_Panel::EPanelRenderMode::PR_Image );
	SelectionTabControl->AddControlToTab( SelectedImagePanel, "Selection/Texture" );

	SelectedImageNameLabel = new SV_Label( MainView, SelectionTabControl->GetTabPanel() );
	SelectedImageNameLabel->SetSize( D2D1::SizeF( 270, 15 ) );
	SelectedImageNameLabel->AlignUnder( SelectedImagePanel, 5.0f );
	SelectedImageNameLabel->SetDrawBackground( true );
	SelectionTabControl->AddControlToTab( SelectedImageNameLabel, "Selection/Texture" );


	// Texture properties
	float textwidth = 80;
	float alignDistance = -3.0f;
	SelectedTexNrmStrSlider = new SV_NamedSlider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedTexNrmStrSlider->AlignUnder( SelectedImageNameLabel, alignDistance + 5.0f );
	SelectedTexNrmStrSlider->GetLabel()->SetCaption( L"Normalmap:" );
	SelectedTexNrmStrSlider->GetLabel()->SetSize( D2D1::SizeF( textwidth, SelectedTexNrmStrSlider->GetLabel()->GetSize().height ) );
	SelectedTexNrmStrSlider->GetSlider()->SetPositionAndSize( D2D1::Point2F( 0, 0 ), D2D1::SizeF( 150, 15 ) );
	SelectedTexNrmStrSlider->UpdateDimensions();
	SelectedTexNrmStrSlider->GetSlider()->SetSliderChangedCallback( TextureSettingsSliderChanged, this );
	SelectedTexNrmStrSlider->GetSlider()->SetMinMax( -2.0f, 2.0f );
	SelectedTexNrmStrSlider->GetSlider()->SetValue( 1.0f );
	SelectionTabControl->AddControlToTab( SelectedTexNrmStrSlider, "Selection/Texture" );

	SelectedTexSpecIntensSlider = new SV_NamedSlider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedTexSpecIntensSlider->AlignUnder( SelectedTexNrmStrSlider, alignDistance );
	SelectedTexSpecIntensSlider->GetLabel()->SetCaption( L"Spec intens:" );
	SelectedTexSpecIntensSlider->GetLabel()->SetSize( D2D1::SizeF( textwidth, SelectedTexSpecIntensSlider->GetLabel()->GetSize().height ) );
	SelectedTexSpecIntensSlider->GetSlider()->SetPositionAndSize( D2D1::Point2F( 0, 0 ), D2D1::SizeF( 150, 15 ) );
	SelectedTexSpecIntensSlider->UpdateDimensions();
	SelectedTexSpecIntensSlider->GetSlider()->SetSliderChangedCallback( TextureSettingsSliderChanged, this );
	SelectedTexSpecIntensSlider->GetSlider()->SetMinMax( 0.0f, 5.0f );
	SelectedTexSpecIntensSlider->GetSlider()->SetValue( 1.0f );
	SelectionTabControl->AddControlToTab( SelectedTexSpecIntensSlider, "Selection/Texture" );

	SelectedTexSpecPowerSlider = new SV_NamedSlider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedTexSpecPowerSlider->AlignUnder( SelectedTexSpecIntensSlider, alignDistance );
	SelectedTexSpecPowerSlider->GetLabel()->SetCaption( L"Spec power:" );
	SelectedTexSpecPowerSlider->GetLabel()->SetSize( D2D1::SizeF( textwidth, SelectedTexSpecPowerSlider->GetLabel()->GetSize().height ) );
	SelectedTexSpecPowerSlider->GetSlider()->SetPositionAndSize( D2D1::Point2F( 0, 0 ), D2D1::SizeF( 150, 15 ) );
	SelectedTexSpecPowerSlider->UpdateDimensions();
	SelectedTexSpecPowerSlider->GetSlider()->SetSliderChangedCallback( TextureSettingsSliderChanged, this );
	SelectedTexSpecPowerSlider->GetSlider()->SetMinMax( 0.1f, 200.0f );
	SelectedTexSpecPowerSlider->GetSlider()->SetValue( 90.0f );
	SelectionTabControl->AddControlToTab( SelectedTexSpecPowerSlider, "Selection/Texture" );


	SV_Label* worldMeshSettingsInfoLabel = new SV_Label( MainView, SelectionTabControl->GetTabPanel() );
	worldMeshSettingsInfoLabel->SetSize( D2D1::SizeF( 270, 15 ) );
	worldMeshSettingsInfoLabel->AlignUnder( SelectedTexSpecPowerSlider, alignDistance );
	worldMeshSettingsInfoLabel->SetDrawBackground( true );
	worldMeshSettingsInfoLabel->SetCaption( L" WorldMesh-Settings:" );
	SelectionTabControl->AddControlToTab( worldMeshSettingsInfoLabel, "Selection/Texture" );

	SelectedTexDisplacementSlider = new SV_NamedSlider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedTexDisplacementSlider->AlignUnder( worldMeshSettingsInfoLabel, alignDistance + 5.0f );
	SelectedTexDisplacementSlider->GetLabel()->SetCaption( L"Displacement:" );
	SelectedTexDisplacementSlider->GetLabel()->SetSize( D2D1::SizeF( textwidth, SelectedTexDisplacementSlider->GetLabel()->GetSize().height ) );
	SelectedTexDisplacementSlider->GetSlider()->SetPositionAndSize( D2D1::Point2F( 0, 0 ), D2D1::SizeF( 150, 15 ) );
	SelectedTexDisplacementSlider->UpdateDimensions();
	SelectedTexDisplacementSlider->GetSlider()->SetSliderChangedCallback( TextureSettingsSliderChanged, this );
	SelectedTexDisplacementSlider->GetSlider()->SetMinMax( -2.0f, 2.0f );
	SelectedTexDisplacementSlider->GetSlider()->SetValue( 1.0f );
	SelectionTabControl->AddControlToTab( SelectedTexDisplacementSlider, "Selection/Texture" );

	SelectedMeshTessAmountSlider = new SV_NamedSlider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedMeshTessAmountSlider->AlignUnder( SelectedTexDisplacementSlider, alignDistance * 2 );
	SelectedMeshTessAmountSlider->GetLabel()->SetCaption( L"Tesselation:" );
	SelectedMeshTessAmountSlider->GetLabel()->SetSize( D2D1::SizeF( textwidth, SelectedMeshTessAmountSlider->GetLabel()->GetSize().height ) );
	SelectedMeshTessAmountSlider->GetSlider()->SetPositionAndSize( D2D1::Point2F( 0, 0 ), D2D1::SizeF( 150, 15 ) );
	SelectedMeshTessAmountSlider->UpdateDimensions();
	SelectedMeshTessAmountSlider->GetSlider()->SetSliderChangedCallback( TextureSettingsSliderChanged, this );
	SelectedMeshTessAmountSlider->GetSlider()->SetMinMax( 0.0f, 2.0f );
	SelectedMeshTessAmountSlider->GetSlider()->SetValue( 0.0f );
	SelectionTabControl->AddControlToTab( SelectedMeshTessAmountSlider, "Selection/Texture" );

	SelectedMeshRoundnessSlider = new SV_NamedSlider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedMeshRoundnessSlider->AlignUnder( SelectedMeshTessAmountSlider, alignDistance );
	SelectedMeshRoundnessSlider->GetLabel()->SetCaption( L"Roundness:" );
	SelectedMeshRoundnessSlider->GetLabel()->SetSize( D2D1::SizeF( textwidth, SelectedMeshRoundnessSlider->GetLabel()->GetSize().height ) );
	SelectedMeshRoundnessSlider->GetSlider()->SetPositionAndSize( D2D1::Point2F( 0, 0 ), D2D1::SizeF( 150, 15 ) );
	SelectedMeshRoundnessSlider->UpdateDimensions();
	SelectedMeshRoundnessSlider->GetSlider()->SetSliderChangedCallback( TextureSettingsSliderChanged, this );
	SelectedMeshRoundnessSlider->GetSlider()->SetMinMax( 0.0f, 1.0f );
	SelectedMeshRoundnessSlider->GetSlider()->SetValue( 1.0f );
	SelectionTabControl->AddControlToTab( SelectedMeshRoundnessSlider, "Selection/Texture" );

	SV_Label* subdivInfoLabel = new SV_Label( MainView, SelectionTabControl->GetTabPanel() );
	subdivInfoLabel->SetSize( D2D1::SizeF( 270, 15 ) );
	subdivInfoLabel->AlignUnder( SelectedMeshRoundnessSlider, 2.0f );
	subdivInfoLabel->SetDrawBackground( false );
	subdivInfoLabel->SetCaption( L"Press Space to subdivide the selected surface.\n(Not saved yet)" );
	SelectionTabControl->AddControlToTab( subdivInfoLabel, "Selection/Texture" );


	/*SelectedTexSpecModulationSlider = new SV_NamedSlider(MainView, SelectionTabControl->GetTabPanel());
	SelectedTexSpecModulationSlider->AlignUnder(SelectedTexSpecPowerSlider, alignDistance);
	SelectedTexSpecModulationSlider->GetLabel()->SetCaption("Spec. Modulate:");
	SelectedTexSpecModulationSlider->GetLabel()->SetSize(D2D1::SizeF(textwidth, SelectedTexSpecModulationSlider->GetLabel()->GetSize().height));
	SelectedTexSpecModulationSlider->GetSlider()->SetPositionAndSize(D2D1::Point2F(0, 0), D2D1::SizeF(150, 15));
	SelectedTexSpecModulationSlider->UpdateDimensions();
	SelectedTexSpecModulationSlider->GetSlider()->SetSliderChangedCallback(TextureSettingsSliderChanged, this);
	SelectedTexSpecModulationSlider->GetSlider()->SetMinMax(0.0f, 1.0f);
	SelectedTexSpecModulationSlider->GetSlider()->SetValue(1.0f);
	SelectionTabControl->AddControlToTab(SelectedTexSpecModulationSlider, "Selection/Texture");*/








	// Selected vegetation properties (Size)
	SelectedVegSizeSlider = new SV_Slider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedVegSizeSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	SelectedVegSizeSlider->SetMinMax( 0.0f, 3.0f );
	SelectedVegSizeSlider->SetValue( 1.0f );
	SelectedVegSizeSlider->SetSliderChangedCallback( VegetationScaleSliderChanged, this );
	SelectionTabControl->AddControlToTab( SelectedVegSizeSlider, "Selection/Vegetation" );

	SV_Label* selVegSizeLabel = new SV_Label( MainView, SelectionTabControl->GetTabPanel() );
	selVegSizeLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	selVegSizeLabel->SetCaption( L"Vegetation size:" );
	SelectionTabControl->AddControlToTab( selVegSizeLabel, "Selection/Vegetation" );

	SelectedVegSizeSlider->AlignUnder( selVegSizeLabel, 5 );

	// Amount
	SelectedVegAmountSlider = new SV_Slider( MainView, SelectionTabControl->GetTabPanel() );
	SelectedVegAmountSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	SelectedVegAmountSlider->SetSliderChangedCallback( VegetationAmountSliderChanged, this );
	SelectedVegAmountSlider->SetMinMax( 0.0f, 3.0f );
	SelectedVegAmountSlider->SetValue( 1.0f );
	SelectionTabControl->AddControlToTab( SelectedVegAmountSlider, "Selection/Vegetation" );

	SV_Label* selVegAmountLabel = new SV_Label( MainView, SelectionTabControl->GetTabPanel() );
	selVegAmountLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	selVegAmountLabel->SetCaption( L"Vegetation density:" );
	SelectionTabControl->AddControlToTab( selVegAmountLabel, "Selection/Vegetation" );

	selVegAmountLabel->AlignUnder( SelectedVegSizeSlider, 8 );
	SelectedVegAmountSlider->AlignUnder( selVegAmountLabel, 5 );

	SelectedVegModifiedWarningLabel = new SV_Label( MainView, SelectionTabControl->GetTabPanel() );
	SelectedVegModifiedWarningLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	SelectedVegModifiedWarningLabel->SetCaption( L"You may lose changes made to the volume after changing its density!" );
	SelectedVegModifiedWarningLabel->SetTextColor( D2D1::ColorF( 1, 0, 0, 1 ) );
	SelectedVegModifiedWarningLabel->SetTextSize( 9 );
	SelectionTabControl->AddControlToTab( SelectedVegModifiedWarningLabel, "Selection/Vegetation" );
	SelectedVegModifiedWarningLabel->AlignUnder( SelectedVegAmountSlider, 5 );

	SelectionTabControl->SetActiveTab( "Selection/Texture" );
	//SelectionTabControl->SetHidden(true);

	return XR_SUCCESS;
}

XMFLOAT3 GetCameraPosition() {
    if (oCGame::GetGame() && oCGame::GetGame()->_zCSession_camVob)
    {
        return oCGame::GetGame()->_zCSession_camVob->GetPositionWorld();
    }
    return XMFLOAT3(0, 0, 0);
}

XMVECTOR GetCameraPositionXM() {
    auto position = GetCameraPosition();
    return XMLoadFloat3(&position);
}

/** Draws this sub-view */
void D2DEditorView::Draw( const D2D1_RECT_F& clientRectAbs, float deltaTime ) {
	// If the editor is not open, dont draw it. Slide it in otherwise.

	std::wstring str;
	if ( !Engine::GAPI->GetRendererState().RendererSettings.DisableWatermark ) {
		// Draw GD3D11-Text
		str = (L"Development preview\n" + Toolbox::ToWideChar( VERSION_STRING ));

		MainView->GetBrush()->SetColor( D2D1::ColorF( 1, 1, 1, 0.5f ) );
		MainView->GetRenderTarget()->DrawText( str.c_str(), str.length(), MainView->GetTextFormatBig(), D2D1::RectF( 0, 0, 300, 50 ), MainView->GetBrush() );
	}


	D2D1_POINT_2F p;
	p.y = 0;
	if ( !IsEnabled ) {
#ifdef PUBLIC_RELEASE
		if ( deltaTime == 0.0f ) // Don't show it in menus
			p.x = -(MainPanel->GetSize().width + 50.0f);
		else
#endif
		{
			p.x = Toolbox::lerp( MainPanel->GetPosition().x, -(MainPanel->GetSize().width + 50.0f), std::min( deltaTime * 5.0f, 1.0f ) );
			SetHidden( GetRect().right < 0 );
			Parent->SetHidden( IsHidden() );
		}

		return;
	} else {
		p.x = Toolbox::lerp( MainPanel->GetPosition().x, 0, std::min( deltaTime * 8.0f, 1.0f ) );

		SetHidden( false );
		Parent->SetHidden( IsHidden() );
	}
	MainPanel->SetPosition( p );


    XMFLOAT3 wDir; XMStoreFloat3( &wDir, Engine::GAPI->UnprojectCursorXM() );
    XMFLOAT3 hit;
    XMFLOAT3 hitTri[3];
    auto hasHit = Engine::GAPI->TraceWorldMesh( GetCameraPosition(), wDir, hit, nullptr, hitTri );
    
	// Draw subviews
	if ( IsEnabled || MainPanel->GetPosition().x + 40.0f > -MainPanel->GetSize().width ) {
		// Draw mode-text
		switch ( Mode ) {
		case EM_IDLE:
			str = L"Controls:\n"
				L"Mouse 1 - Move\n"
				L"Mouse 2 - Look\n"
				L"Mouse 1+2 - Pan\n"
				L"Mousewheel - Scale\n"
				L"F1 - Close editor\n"
				L"Shift-Click - Place Hero\n"
				L"Pos: " + Toolbox::ToWideChar( float3( GetCameraPosition() ).toString() ) +
				L"\nTime: " + std::to_wstring( oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetMasterTime() ) +
				L"\nHas Hit: " + std::to_wstring( hasHit ) +
				L"\nHit Pos: " + std::to_wstring( hit.x ) + L" x " + std::to_wstring( hit.y ) + L" x " + std::to_wstring( hit.z ) +
				L"\nWetness: " + std::to_wstring( Engine::GAPI->GetSceneWetness() );
			break;

		case EM_SELECT_POLY:
			str = L"SelectPoly not implemented yet!";
			break;

		case EM_PLACE_VEGETATION:
			str = L"Click to place VegetationBox.";
			break;

		case EM_REMOVE_VEGETATION:
			str = L"Hold LCTRL and Click/Drag to remove\n"
				L"grass from the currently selected\n"
				L"VegetationBox\n";
			break;
		default:
			str = L"";
		}

		MainView->GetBrush()->SetColor( D2D1::ColorF( 1, 1, 1, 0.5f ) );
		float helpTextX = (MainPanel->GetRect().right + 20) + (MainPanel->GetPosition().x * 2.0f); // Pos.x is negative or zero, so it doubles up and hides the text

		MainView->GetRenderTarget()->DrawText( str.c_str(), str.length(), MainView->GetTextFormatBig(), D2D1::RectF( helpTextX, 0, 900, 600 ), MainView->GetBrush() );

		D2DSubView::Draw( clientRectAbs, deltaTime );
	}
}

/** Updates the editor */
void D2DEditorView::Update( float deltaTime ) {
	if ( !IsEnabled || Engine::AntTweakBar->GetActive() )
		return;

    Keys[VK_LSHIFT] = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
    Keys[VK_RSHIFT] = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    Keys[VK_SHIFT] = (GetAsyncKeyState(VK_SHIFT)  & 0x8000) != 0;
    Keys[VK_SHIFT] = Keys[VK_SHIFT] || Keys[VK_LSHIFT] || Keys[VK_RSHIFT];
    
    Keys[VK_LCONTROL] = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
    Keys[VK_RCONTROL] = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    Keys[VK_CONTROL]  = (GetAsyncKeyState(VK_CONTROL)  & 0x8000) != 0;
    Keys[VK_CONTROL]  = Keys[VK_CONTROL] || Keys[VK_LCONTROL] || Keys[VK_RCONTROL];

    Keys[VK_LMENU]    = (GetAsyncKeyState(VK_LMENU)    & 0x8000) != 0;
    Keys[VK_RMENU]    = (GetAsyncKeyState(VK_RMENU)    & 0x8000) != 0;
    Keys[VK_MENU]     = (GetAsyncKeyState(VK_MENU)     & 0x8000) != 0;
    Keys[VK_MENU]     = Keys[VK_MENU] || Keys[VK_LMENU] || Keys[VK_RMENU];
    
	Widgets->Render();

	if ( Selection.SelectedMesh ) {
		VisualizeMeshInfo( Selection.SelectedMesh, XMFLOAT4( 1, 0, 0, 1 ) );
	}

	if ( Selection.SelectedVegetationBox ) {
		if ( Selection.SelectedVegetationBox )
			Selection.SelectedVegetationBox->VisualizeGrass( XMFLOAT4( 1, 0, 0, 1 ) );
	}

	if ( IsMouseInsideEditorWindow() || Widgets->IsWidgetClicked() ) {
		return;
	}

	if ( !MMovedAfterClick ) {
		if ( Mode == EM_PLACE_VEGETATION ) {
			DoVegetationPlacement();
		} else if ( Mode == EM_SELECT_POLY || Mode == EM_IDLE ) {
			DoSelection();
		} else if ( Mode == EM_REMOVE_VEGETATION ) {
			DoVegetationRemove();
		}

	} else if ( !Keys[VK_CONTROL] ) {
		// Clicked and moving
		DoEditorMovement();
	}

}

/** Handles vegetation removing */
void D2DEditorView::DoVegetationRemove() {
	XMFLOAT3 wDir; XMStoreFloat3( &wDir, Engine::GAPI->UnprojectCursorXM() );
	XMFLOAT3 hit;
	XMFLOAT3 hitTri[3];

	float removeRange = 250.0f * (1.0f + MMWDelta * 0.01f);
    
	if ( Selection.SelectedVegetationBox ) {
		if ( Engine::GAPI->TraceWorldMesh( GetCameraPosition(), wDir, hit, nullptr, hitTri ) ) {
			XMFLOAT4 c;

			// Do this when only Mouse1 and CTRL are pressed
			if ( MButtons[0] && !MButtons[1] && !MButtons[2] && Keys[VK_CONTROL] ) {
				Selection.SelectedVegetationBox->RemoveVegetationAt( hit, removeRange );

				// Delete if empty
				if ( Selection.SelectedVegetationBox->IsEmpty() ) {
					Engine::GAPI->RemoveVegetationBox( Selection.SelectedVegetationBox );
					Selection.SelectedVegetationBox = nullptr;
				}

				c = XMFLOAT4( 1, 0, 0, 1 );
			} else {
				c = XMFLOAT4( 1, 1, 1, 1 );
			}

			Engine::GraphicsEngine->GetLineRenderer()->AddAABB( hit, XMFLOAT3( removeRange, removeRange, removeRange ), c );
		}
	}
}

/** Handles vegetationbox placement */
void D2DEditorView::DoVegetationPlacement() {
	XMFLOAT3 wDir; XMStoreFloat3( &wDir, Engine::GAPI->UnprojectCursorXM() );
	XMFLOAT3 hit;
	XMFLOAT3 hitTri[3];

	TracedTexture = "";

	// Check for restricted by texture
	std::string* rtp = nullptr;
	if ( VegRestrictByTextureCheckBox->GetChecked() )
		rtp = &TracedTexture;

	// Trace the worldmesh from the cursor
	if ( Engine::GAPI->TraceWorldMesh( GetCameraPosition(), wDir, hit, rtp, hitTri ) ) {
		// Update the position if successful
		DraggedBoxCenter = hit;

		// Visualize box
		XMFLOAT3 minAABB;
		XMFLOAT3 maxAABB;
		XMStoreFloat3( &minAABB, XMLoadFloat3( &DraggedBoxCenter ) + XMLoadFloat3( &DraggedBoxMinLocal ) * (1 + MMWDelta * 0.01f) );
		XMStoreFloat3( &maxAABB, XMLoadFloat3( &DraggedBoxCenter ) + XMLoadFloat3( &DraggedBoxMaxLocal ) * (1 + MMWDelta * 0.01f) );
		Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax( minAABB, maxAABB );

		// Visualize triangle
		FXMVECTOR nrm = Toolbox::ComputeNormal( hitTri[0], hitTri[1], hitTri[2] );
		XMFLOAT3 hitTri0_XMFLOAT3;
		XMFLOAT3 hitTri1_XMFLOAT3;
		XMFLOAT3 hitTri2_XMFLOAT3;
		XMStoreFloat3( &hitTri0_XMFLOAT3, XMLoadFloat3( &hitTri[0] ) + nrm );
		XMStoreFloat3( &hitTri1_XMFLOAT3, XMLoadFloat3( &hitTri[1] ) + nrm );
		XMStoreFloat3( &hitTri2_XMFLOAT3, XMLoadFloat3( &hitTri[2] ) + nrm );
		Engine::GraphicsEngine->GetLineRenderer()->AddTriangle( hitTri0_XMFLOAT3, hitTri1_XMFLOAT3, hitTri2_XMFLOAT3 );
	}
}

/** Finds the GVegetationBox from its mesh-info */
GVegetationBox* D2DEditorView::FindVegetationFromMeshInfo( MeshInfo* info ) {
    for ( GVegetationBox* vegetationBox : Engine::GAPI->GetVegetationBoxes() ) {
        if ( vegetationBox->GetWorldMeshPart() == info )
            return vegetationBox;
    }

	return nullptr;
}



/** Handles selection */
void D2DEditorView::DoSelection() {
	XMFLOAT3 wDir; XMStoreFloat3( &wDir, Engine::GAPI->UnprojectCursorXM() );
	XMFLOAT3 hitVob( FLT_MAX, FLT_MAX, FLT_MAX ), hitSkel( FLT_MAX, FLT_MAX, FLT_MAX ), hitWorld( FLT_MAX, FLT_MAX, FLT_MAX );
	XMFLOAT3 hitTri[3];
	MeshInfo* hitMesh;
	zCMaterial* hitMaterial = nullptr, * hitMaterialVob = nullptr;
	TracedTexture = "";

	VobInfo* tVob = nullptr;
	SkeletalVobInfo* tSkelVob = nullptr;

	TracedSkeletalVobInfo = nullptr;
	TracedVobInfo = nullptr;
	TracedMaterial = nullptr;

	// Trace mesh-less vegetationboxes
	TracedVegetationBox = TraceVegetationBoxes( GetCameraPosition(), wDir );
	if ( TracedVegetationBox ) {
		TracedVegetationBox->VisualizeGrass( XMFLOAT4( 1, 1, 1, 1 ) );
		return;
	}

	// Trace vobs
	tVob = Engine::GAPI->TraceStaticMeshVobsBB( GetCameraPosition(), wDir, hitVob, &hitMaterialVob );
	tSkelVob = Engine::GAPI->TraceSkeletalMeshVobsBB( GetCameraPosition(), wDir, hitSkel );

	// Trace the worldmesh from the cursor
	Engine::GAPI->TraceWorldMesh( GetCameraPosition(), wDir, hitWorld, &TracedTexture, hitTri, &hitMesh, &hitMaterial );

	float lenVob;
	XMStoreFloat( &lenVob, XMVector3Length( GetCameraPositionXM() - XMLoadFloat3( &hitVob ) ) );
	float lenSkel;
	XMStoreFloat( &lenSkel, XMVector3Length( GetCameraPositionXM() - XMLoadFloat3( &hitSkel ) ) );
	float lenWorld;
	XMStoreFloat( &lenWorld, XMVector3Length( GetCameraPositionXM() - XMLoadFloat3( &hitWorld ) ) );

	// Check world hit
	if ( lenWorld < lenVob && lenWorld < lenSkel ) {
		TracedPosition = hitWorld;

		if ( SelectTrianglesOnlyCheckBox->GetChecked() ) {
			memcpy( SelectedTriangle, hitTri, sizeof( SelectedTriangle ) );

			// Visualize triangle
			FXMVECTOR nrm = Toolbox::ComputeNormal( hitTri[0], hitTri[1], hitTri[2] ) ;
			XMFLOAT3 hitTri0_XMFLOAT3;
			XMFLOAT3 hitTri1_XMFLOAT3;
			XMFLOAT3 hitTri2_XMFLOAT3;
			XMStoreFloat3( &hitTri0_XMFLOAT3, XMLoadFloat3( &hitTri[0] ) + nrm );
			XMStoreFloat3( &hitTri1_XMFLOAT3, XMLoadFloat3( &hitTri[1] ) + nrm );
			XMStoreFloat3( &hitTri2_XMFLOAT3, XMLoadFloat3( &hitTri[2] ) + nrm );
			Engine::GraphicsEngine->GetLineRenderer()->AddTriangle( hitTri0_XMFLOAT3, hitTri1_XMFLOAT3, hitTri2_XMFLOAT3 );

			TracedMaterial = hitMaterial;
		} else {
			// Try to find a vegetationbox for this mesh
			TracedVegetationBox = FindVegetationFromMeshInfo( hitMesh );
			if ( TracedVegetationBox ) {
				if ( Selection.SelectedVegetationBox != TracedVegetationBox )
					TracedVegetationBox->VisualizeGrass( XMFLOAT4( 1, 1, 1, 1 ) );
				return; // Vegetation has priority over mesh
			}

			TracedMesh = hitMesh;
			TracedMaterial = hitMaterial;

			if ( Selection.SelectedMesh != TracedMesh )
				VisualizeMeshInfo( hitMesh );
		}

		return;
	}

	// Check skeletal hit
	if ( tSkelVob && lenSkel < lenVob && lenSkel < lenWorld ) {
		TracedSkeletalVobInfo = tSkelVob;
		XMFLOAT3 minAABB;
		XMFLOAT3 maxAABB;
		XMStoreFloat3( &minAABB, XMVectorSet( TracedSkeletalVobInfo->Vob->GetBBoxLocal().Min.x, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Min.y, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Min.z, 0 ) + TracedSkeletalVobInfo->Vob->GetPositionWorldXM() );
		XMStoreFloat3( &maxAABB, XMVectorSet( TracedSkeletalVobInfo->Vob->GetBBoxLocal().Max.x, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Max.y, TracedSkeletalVobInfo->Vob->GetBBoxLocal().Max.z, 0 ) + TracedSkeletalVobInfo->Vob->GetPositionWorldXM() );
		Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax( minAABB, maxAABB, XMFLOAT4( 1, 1, 1, 1 ) );

		if ( !TracedSkeletalVobInfo->VisualInfo->Meshes.empty() )
			TracedMaterial = (*TracedSkeletalVobInfo->VisualInfo->Meshes.begin()).first;

		return;
	}

	// Check vob hit
	if ( tVob && lenVob < lenSkel && lenVob < lenWorld ) {
		TracedVobInfo = tVob;
		XMFLOAT3 min_XMFloat3;
		XMFLOAT3 max_XMFloat3;
		XMStoreFloat3( &min_XMFloat3, XMVectorSet( TracedVobInfo->Vob->GetBBoxLocal().Min.x, TracedVobInfo->Vob->GetBBoxLocal().Min.y, TracedVobInfo->Vob->GetBBoxLocal().Min.z, 0 ) + TracedVobInfo->Vob->GetPositionWorldXM() );
		XMStoreFloat3( &max_XMFloat3, XMVectorSet( TracedVobInfo->Vob->GetBBoxLocal().Max.x, TracedVobInfo->Vob->GetBBoxLocal().Max.y, TracedVobInfo->Vob->GetBBoxLocal().Max.z, 0 ) + TracedVobInfo->Vob->GetPositionWorldXM() );
		Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax( min_XMFloat3, max_XMFloat3, XMFLOAT4( 1, 1, 1, 1 ) );

		TracedMaterial = hitMaterialVob;

		if ( Selection.SelectedVobInfo != TracedVobInfo && hitMaterialVob ) {
			XMFLOAT4X4 world;
			XMStoreFloat4x4( &world, XMMatrixTranspose(XMLoadFloat4x4(TracedVobInfo->Vob->GetWorldMatrixPtr())) );
			VisualizeMeshInfo( TracedVobInfo->VisualInfo->Meshes[hitMaterialVob][0], XMFLOAT4( 1, 1, 1, 1 ), false, &world );
		}

		return;
	}

}

/** Visualizes a mesh info */
void D2DEditorView::VisualizeMeshInfo( MeshInfo* m, const XMFLOAT4& color, bool showBounds, const XMFLOAT4X4* world ) {
	for ( unsigned int i = 0; i < m->Indices.size(); i += 3 ) {
		XMFLOAT3 tri[3];
		float edge[3];

		tri[0] = *m->Vertices[m->Indices[i]].Position.toXMFLOAT3();
		tri[1] = *m->Vertices[m->Indices[i + 1]].Position.toXMFLOAT3();
		tri[2] = *m->Vertices[m->Indices[i + 2]].Position.toXMFLOAT3();

		edge[0] = m->Vertices[m->Indices[i]].TexCoord2.x;
		edge[1] = m->Vertices[m->Indices[i + 1]].TexCoord2.x;
		edge[2] = m->Vertices[m->Indices[i + 2]].TexCoord2.x;

		if ( world ) {
			XMMATRIX XMV_world = XMLoadFloat4x4( world );
			XMStoreFloat3( &tri[0], XMVector3TransformCoord( XMLoadFloat3( &tri[0] ), XMV_world ) );
			XMStoreFloat3( &tri[1], XMVector3TransformCoord( XMLoadFloat3( &tri[1] ), XMV_world ) );
			XMStoreFloat3( &tri[2], XMVector3TransformCoord( XMLoadFloat3( &tri[2] ), XMV_world ) );
		}

		// Visualize triangle
		//FXMVECTOR NRM_XM = Toolbox::ComputeNormal(tri[0], tri[1], tri[2]);
		// XMFloat3 nrm;
		// XMStoreFloat3 ( &nrm, NRM_XM);
		//Engine::GraphicsEngine->GetLineRenderer()->AddTriangle(tri[0] + nrm, tri[1] + nrm, tri[2] + nrm, color);

		if ( showBounds ) {
			Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( tri[0], XMFLOAT4( 1, edge[0], 0, 1 ) ), LineVertex( tri[1], XMFLOAT4( 1, edge[1], 0, 1 ) ) );
			Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( tri[0], XMFLOAT4( 1, edge[0], 0, 1 ) ), LineVertex( tri[2], XMFLOAT4( 1, edge[2], 0, 1 ) ) );
			Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( tri[1], XMFLOAT4( 1, edge[1], 0, 1 ) ), LineVertex( tri[2], XMFLOAT4( 1, edge[2], 0, 1 ) ) );
		} else {
			Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( tri[0], color ), LineVertex( tri[1], color ) );
			Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( tri[0], color ), LineVertex( tri[2], color ) );
			Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( tri[1], color ), LineVertex( tri[2], color ) );
		}
	}
}

/** Called on xBUTTONDOWN */
void D2DEditorView::OnMouseButtonDown( int button ) {
	// Catch clicks inside the editor window
	if ( IsMouseInsideEditorWindow() ) {
		return;
	}

	// If nothing is pressed right now, reset the moved state
	if ( !MButtons[0] && !MButtons[1] && !MButtons[2] ) {
		MMovedAfterClick = false;

		GetCursorPos( &CStartMousePosition );

		// Set capture for this buttondown
		if ( !GetCapture() )
			SetCapture( Engine::GAPI->GetOutputWindow() );
	}

	// Register buttonpress
	ButtonPressedLastTime[button] = Toolbox::timeSinceStartMs();
	MButtons[button] = true;
}

void D2DEditorView::OnMouseButtonUp( int button ) {
	MButtons[button] = false; // Release is possible anywhere

	if ( GetCapture() ) // Release mouse capture, if there was any
		ReleaseCapture();

	// Catch clicks inside the editor window
	if ( IsMouseInsideEditorWindow() ) {
		return;
	}

	// If nothing is pressed right now, reset the moved state
	if ( !MButtons[0] && !MButtons[1] && !MButtons[2] ) {
		if ( MMovedAfterClick ) {
			// Show cursor again
			if ( MPrevCursor )
				SetCursor( MPrevCursor );

			// Restore mouse position after dragging
			SetCursorPos( CStartMousePosition.x, CStartMousePosition.y );
		}

		MMovedAfterClick = false;
	}

	// Catch clicks inside the editor window
	if ( IsMouseInsideEditorWindow() ) {
		return;
	}

	// Check for click
	if ( !(MButtons[0] || MButtons[1] || MButtons[2]) && !MMovedAfterClick ) {
		OnMouseClick( button );
	} else // This was a drag
	{

	}

}

void D2DEditorView::OnMouseClick( int button ) {
	if ( button == 0 ) {
		SelectionTabControl->SetActiveTab( "Selection/Texture" ); // Default

		if ( Keys[VK_SHIFT] ) {
		    XMFLOAT3 wDir; XMStoreFloat3( &wDir, Engine::GAPI->UnprojectCursorXM() );
		    XMFLOAT3 hit;
		    XMFLOAT3 hitTri[3];
		    auto hasHit = Engine::GAPI->TraceWorldMesh( GetCameraPosition(), wDir, hit, nullptr, hitTri );
            if (hasHit) {
			    XMFLOAT3 pos = hit;
			    LogInfo() << "Setting player position to: " << float3( pos ).toString();
                pos.y += 500.0f; // Spawn above ground to avoid getting stuck in terrain
			    Engine::GAPI->SetPlayerPosition( pos );
            }
		} else if ( Mode == EM_PLACE_VEGETATION ) {
			// Place the currently dragged vegetationbox
			PlaceDraggedVegetationBox();
		} else if ( Mode == EM_SELECT_POLY || Mode == EM_IDLE ) {
			// Reset selection and apply what ever has the most priority
			MMWDelta = 0;
			Selection.Reset();

			VegLastUniformScale = 1.0f;
			SelectedVegAmountSlider->SetValue( 1.0f );
			SelectedVegSizeSlider->SetValue( 1.0f );


			if ( TracedVobInfo ) {
				Selection.SelectedVobInfo = TracedVobInfo;
				Selection.SelectedMaterial = TracedMaterial;

#ifndef BUILD_SPACER
				VobSettingsDialog->SetHidden( false );
				VobSettingsDialog->SetVobInfo( TracedVobInfo );
#endif

				UpdateSelectionPanel();

				Widgets->ClearSelection();
				Widgets->AddSelection( TracedVobInfo );

			} else if ( TracedSkeletalVobInfo ) {
				Selection.SelectedSkeletalVob = TracedSkeletalVobInfo;

#ifndef BUILD_SPACER
				VobSettingsDialog->SetHidden( false );
				VobSettingsDialog->SetVobInfo( TracedSkeletalVobInfo );
#endif

				Selection.SelectedMaterial = TracedMaterial;
				UpdateSelectionPanel();

				Widgets->ClearSelection();
				Widgets->AddSelection( TracedSkeletalVobInfo );
			} else if ( TracedVegetationBox ) {
				Selection.SelectedVegetationBox = TracedVegetationBox;

				SelectionTabControl->SetActiveTab( "Selection/Vegetation" );
				SelectedVegModifiedWarningLabel->SetHidden( !Selection.SelectedVegetationBox->HasBeenModified() );


				// Trick the slider into not updating the just selected volume
				float d = Selection.SelectedVegetationBox->GetDensity();
				Selection.SelectedVegetationBox = nullptr;
				SelectedVegAmountSlider->SetValue( d );
				Selection.SelectedVegetationBox = TracedVegetationBox;
			} else // Vegetation has priority over mesh
			{
				Selection.SelectedMesh = TracedMesh;
				Selection.SelectedMaterial = TracedMaterial;

				UpdateSelectionPanel();
			}
		}
	}

	if ( button == 1 ) {
		POINT p; GetCursorPos( &p );
		DWORD lp = (p.y << 16) | p.x;

		// Notify the game about a rightclick
		Engine::GAPI->SendMessageToGameWindow( WM_RBUTTONDOWN, 0, lp );
		Engine::GAPI->SendMessageToGameWindow( WM_RBUTTONUP, 0, lp );
	}
}

/** Updates the selection-panel */
void D2DEditorView::UpdateSelectionPanel() {
	// Update selection panel
	if ( Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture() ) {
		// Select preferred texture for the texture settings
		Engine::AntTweakBar->SetPreferredTextureForSettings( Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt() );
		SelectedImageNameLabel->SetCaption( Toolbox::ToWideChar( Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt() ) );

		// Update thumbnail
		MyDirectDrawSurface7* surface = Engine::GAPI->GetSurface( Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt() );

		if ( surface ) {
            auto& thumb = (surface->GetEngineTexture())->GetThumbnail();
            if ( !thumb.Get() ) {
				XLE( (surface->GetEngineTexture())->CreateThumbnail() );
                auto& thumb2 = (surface->GetEngineTexture())->GetThumbnail();
                SelectedImagePanel->SetD3D11TextureAsImage( thumb2.Get(), INT2( 256, 256 ) );
            } else {
			SelectedImagePanel->SetD3D11TextureAsImage( thumb.Get(), INT2( 256, 256 ) );
		}
	}
    }

	if ( Selection.SelectedMesh ) {
		WorldMeshInfo* info = (WorldMeshInfo*)Selection.SelectedMesh;
		MaterialInfo* mi;

		if ( Selection.SelectedMaterial )
			mi = Engine::GAPI->GetMaterialInfoFrom( Selection.SelectedMaterial->GetTexture() );
		else
			mi = nullptr;

		// Get settings from MI, if possible
        SelectedTexDisplacementSlider->GetSlider()->SetValue( 0.f );
        SelectedMeshRoundnessSlider->GetSlider()->SetValue( 0.f );
        SelectedMeshTessAmountSlider->GetSlider()->SetValue( 0.f );
	}
}


void D2DEditorView::OnMouseWheel( int delta ) {
	if ( Selection.SelectedVegetationBox ) {
		// Adjust size of grassblades if not in removing-mode
		if ( Mode == EM_IDLE ) {
			Selection.SelectedVegetationBox->ApplyUniformScaling( delta < 0 ? 0.9f : 1.1f );
		} else if ( Mode == EM_REMOVE_VEGETATION ) {
			// Resizing the BBox of the remove-brush is handled elsewhere
		}
	}
}

void D2DEditorView::ResetEditorCamera() {
	if ( !oCGame::GetGame() || !oCGame::GetGame()->_zCSession_camVob )
		return;

	// Save current camera-matrix
	CStartWorld = *oCGame::GetGame()->_zCSession_camVob->GetWorldMatrixPtr();
	constexpr XMVECTORF32 c_XM_1000 = { { { 1, 0, 0, 0 } } };
	XMVECTOR dir = XMVector3Transform( c_XM_1000, XMLoadFloat4x4( &CStartWorld ) );
	CYaw = asinf( -XMVectorGetZ(dir) / XMVectorGetX( XMVector3Length( dir ) ) ) + XM_PIDIV2;
	CPitch = 0;//atan(- CStartWorld._31 / sqrt(CStartWorld._32 * CStartWorld._32 + CStartWorld._33 * CStartWorld._33));
}

/** Handles the editor movement */
void D2DEditorView::DoEditorMovement() {
	// Hide cursor
	SetCursor( nullptr );

	// Get current cursor pos
	POINT p; GetCursorPos( &p );
	//= D2DView::GetCursorPosition();

	RECT r;
	GetWindowRect( Engine::GAPI->GetOutputWindow(), &r );

	POINT mid;
	mid.x = (int)(r.left / 2 + r.right / 2);
	mid.y = (int)(r.top / 2 + r.bottom / 2);

	// Get difference to last frame
	XMFLOAT2 diff;
	diff.x = (float)(p.x - mid.x);
	diff.y = (float)(p.y - mid.y);

	// Lock the mouse in center
	SetCursorPos( mid.x, mid.y );

	// Move the camera-vob
	zCVob* cVob = oCGame::GetGame()->_zCSession_camVob;

	XMFLOAT4X4* m = cVob->GetWorldMatrixPtr();

	float rSpeed = 0.005f;
	float mSpeed = 15.0f;

	XMFLOAT3 position = Engine::GAPI->GetCameraPosition();


	if ( !MButtons[0] && MButtons[1] ) // Rightclick -> Rotate only
	{
		CPitch += diff.y * rSpeed;
		CYaw += diff.x * rSpeed;

	} else if ( MButtons[0] && !MButtons[1] ) // Leftclick -> Rotate yaw and move xz
	{
		XMFLOAT2 fwd2d = XMFLOAT2( sinf( CYaw ), cosf( CYaw ) );

		position.x += fwd2d.x * -diff.y * mSpeed;
		position.z += fwd2d.y * -diff.y * mSpeed;

		// Rotate yaw only
		CYaw += diff.x * rSpeed;
	} else if ( MButtons[0] && MButtons[1] ) // Both, move sideways
	{
		FXMVECTOR fwd = XMVectorSet( sinf( CYaw ), 0.0f, cosf( CYaw ), 0 );

		constexpr XMVECTORF32 up = { { { 0, 1, 0, 0 } } };
		FXMVECTOR side = XMVector3Cross( fwd, up );

		XMFLOAT3 position_add;
		XMStoreFloat3( &position_add, ( (side * -diff.x) + (up * -diff.y ) ) * mSpeed );
		position.x += position_add.x;
		position.y += position_add.y;
		position.z += position_add.z;
	}

	XMMATRIX world = XMMatrixTranslation( position.x, position.y, position.z );

	//XMMATRIX rot = XMMatrixTranspose( XMMatrixRotationRollPitchYaw(CPitch, CYaw, 0) ); //rot not used?

	XMStoreFloat4x4( &*m, XMMatrixTranspose( world ) );

	// Update camera
	zCCamera::GetCamera()->Activate();

	// Update GAPI
	Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );
}

/** Returns if the mouse is inside the editor window */
bool D2DEditorView::IsMouseInsideEditorWindow() {
	POINT p = D2DView::GetCursorPosition();

	// Catch clicks inside the editor window
	if ( D2DSubView::PointInsideRect( D2D1::Point2F( (float)p.x, (float)p.y ), MainPanel->GetRect() ) ) {
		return true;
	}

	return false;
}

/** Places the currently dragged vegetation box */
GVegetationBox* D2DEditorView::PlaceDraggedVegetationBox() {
	SetEditorMode( EM_IDLE );

	GVegetationBox::EShape shape = GVegetationBox::S_Box;
	if ( VegCircularShapeCheckBox->GetChecked() )
		shape = GVegetationBox::S_Circle;

	GVegetationBox* box = new GVegetationBox;
	XMFLOAT3 minp;
	XMFLOAT3 maxp;
	XMStoreFloat3( &minp, XMLoadFloat3( &DraggedBoxCenter ) + XMLoadFloat3( &DraggedBoxMinLocal ) * (1 + MMWDelta) );
	XMStoreFloat3( &maxp, XMLoadFloat3( &DraggedBoxCenter ) + XMLoadFloat3( &DraggedBoxMaxLocal ) * (1 + MMWDelta) );
	if ( XR_SUCCESS == box->InitVegetationBox( minp, maxp, "", 1.0f, 1.0f, TracedTexture, shape ) ) {
		Engine::GAPI->AddVegetationBox( box );
	} else {
		SAFE_DELETE( box );
	}

	Selection.SelectedVegetationBox = box;
	return box;
	//return Engine::GAPI->SpawnVegetationBoxAt(DraggedBoxCenter, DraggedBoxMinLocal, DraggedBoxMaxLocal, 1.0f, TracedTexture);
}

/** Processes a window-message. Return false to stop the message from going to children */
bool D2DEditorView::OnWindowMessage( HWND hWnd, unsigned int msg, WPARAM wParam, LPARAM lParam, const D2D1_RECT_F& clientRectAbs ) {
	// Don't do anything if the AntTweakBar is open
	if ( Engine::AntTweakBar->GetActive() )
		return true;

	bool enableEditorPanel = zCOption::GetOptions()->IsParameter( "XEnableEditorPanel" );

#if !defined(PUBLIC_RELEASE)
	// zCOptions not working for G1 yet
	enableEditorPanel = true;
#endif

	// Always allow opening/closing the editor
#ifdef BUILD_SPACER
	if ( msg == WM_KEYDOWN && (wParam == VK_F1 || wParam == 'O') && !zCOption::GetOptions()->IsParameter( "XNoDevMenu" ) )
#else
	if ( msg == WM_KEYDOWN && (wParam == VK_F1) )
#endif
	{
		if ( !enableEditorPanel )
			return false;

		IsEnabled = !IsEnabled;
		Engine::GAPI->GetRendererState().RendererSettings.DisableWatermark = false;

		if ( IsEnabled ) {
			// Enable free-cam, the easy way
			oCGame::GetGame()->TestKey( GOTHIC_KEY::F6 );

			Engine::GAPI->SetEnableGothicInput( false );
			ResetEditorCamera();

			// Reset the selection, so it doesn't crash on levelchange
			Selection.Reset();
		} else {
			// Disable free-cam, the easy way
			oCGame::GetGame()->TestKey( GOTHIC_KEY::F4 );

			Engine::GAPI->SetEnableGothicInput( true );
		}
		return false;
	}

	// Don't process any messages when disabled
	if ( !IsEnabled )
		return true;

	if ( IsEnabled )
		if ( !D2DSubView::OnWindowMessage( hWnd, msg, wParam, lParam, clientRectAbs ) )
			return false;

	switch ( msg ) {
	case WM_SYSKEYDOWN:
	case WM_KEYDOWN:
		Keys[wParam] = true;
		switch ( wParam ) {
		case VK_DELETE:

			OnDelete();
			break;

		case VK_SPACE:
			if ( Selection.SelectedMesh ) {
				SmoothMesh( (WorldMeshInfo*)Selection.SelectedMesh, true );

				if ( Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture() ) {
					MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( Selection.SelectedMaterial->GetTexture() );

					if ( info ) {
						// Overwrite shader
						info->TesselationShaderPair = "PNAEN_Tesselation";
					}
				}
			}
			break;
		}
	    break;

	case WM_SYSKEYUP:
	case WM_KEYUP:
		Keys[wParam] = false;
		break;

	case WM_MOUSEMOVE:
		if ( (MButtons[0] || MButtons[1] || MButtons[2]) && !MMovedAfterClick && !Keys[VK_CONTROL] ) {
			MMovedAfterClick = true;

			// Hide cursor during movement
			MPrevCursor = SetCursor( nullptr );

			//if (Mode != EM_???)
			{
				RECT r;
				GetWindowRect( Engine::GAPI->GetOutputWindow(), &r );

				// Lock the mouse in center
				SetCursorPos( (int)(r.left / 2 + r.right / 2),
					(int)(r.top / 2 + r.bottom / 2) );
			}
		}
		break;

	case WM_LBUTTONDOWN:
		OnMouseButtonDown( 0 );
		break;

	case WM_LBUTTONUP:
		OnMouseButtonUp( 0 );
		break;

	case WM_RBUTTONDOWN:
		OnMouseButtonDown( 1 );
		break;

	case WM_RBUTTONUP:
		OnMouseButtonUp( 1 );
		break;

	case WM_MBUTTONDOWN:
		OnMouseButtonDown( 2 );
		break;

	case WM_MBUTTONUP:
		OnMouseButtonUp( 2 );
		break;

	case WM_MOUSEWHEEL:
		MMWDelta += (int)(GET_WHEEL_DELTA_WPARAM( wParam ) * 0.1f);
		OnMouseWheel( (int)(GET_WHEEL_DELTA_WPARAM( wParam ) * 0.1f) );
		break;

	}

	Widgets->OnWindowMessage( hWnd, msg, wParam, lParam );

	return true;
}

/** Sets the editor-mode */
void D2DEditorView::SetEditorMode( EditorMode mode ) {
	Mode = mode;
}

/** Called on VK_DELETE */
void D2DEditorView::OnDelete() {
	// Find out what we have selected
	if ( Selection.SelectedVegetationBox ) {
		// Delete all attachments to this mesh
		Engine::GAPI->RemoveVegetationBox( Selection.SelectedVegetationBox );

		Selection.SelectedVegetationBox = nullptr;
		return;
	}

	if ( Selection.SelectedMesh && Selection.SelectedMaterial && Selection.SelectedMaterial->GetTexture() ) {
		// Find the section of this mesh
		FXMVECTOR Position0 = XMVectorSet( Selection.SelectedMesh->Vertices[0].Position.x, Selection.SelectedMesh->Vertices[0].Position.y, Selection.SelectedMesh->Vertices[0].Position.z, 0 );
		FXMVECTOR Position1 = XMVectorSet( Selection.SelectedMesh->Vertices[1].Position.x, Selection.SelectedMesh->Vertices[1].Position.y, Selection.SelectedMesh->Vertices[1].Position.z, 0 );
		FXMVECTOR Position2 = XMVectorSet( Selection.SelectedMesh->Vertices[2].Position.x, Selection.SelectedMesh->Vertices[2].Position.y, Selection.SelectedMesh->Vertices[2].Position.z, 0 );
		XMFLOAT3 avgPos;
		XMStoreFloat3( &avgPos, (Position0 + Position1 + Position2) / 3.0f );

		INT2 s = WorldConverter::GetSectionOfPos( avgPos );
		WorldMeshSectionInfo* section = &Engine::GAPI->GetWorldSections()[s.x][s.y];

		// Remove the texture from rendering
		Engine::GAPI->SupressTexture( section, Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt() );
	}

	SelectionTabControl->SetActiveTab( "Selection/Texture" );
}

/** Button to add a vegetation-volume was pressed */
void D2DEditorView::AddVegButtonPressed( SV_Button* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	v->MMWDelta = 0;

	if ( v->Mode != EM_IDLE ) {
		LogWarn() << "Editor must be in idle-state before you can add a vegetation-volume!";
		return;
	}

	v->SetEditorMode( EM_PLACE_VEGETATION );
}

/** Traces the set of placed vegatation boxes */
GVegetationBox* D2DEditorView::TraceVegetationBoxes( const XMFLOAT3& wPos, const XMFLOAT3& wDir ) {
	float nearest = FLT_MAX;
	GVegetationBox* b = nullptr;

    for ( GVegetationBox* vegetationBox : Engine::GAPI->GetVegetationBoxes() ) {
		if ( vegetationBox->GetWorldMeshPart() )
			continue; // Only take the usual boxes

		XMFLOAT3 bbMin, bbMax;
        vegetationBox->GetBoundingBox( &bbMin, &bbMax );

		float t;
		if ( Toolbox::IntersectBox( bbMin, bbMax, wPos, wDir, t ) ) {
			if ( t < nearest ) {
				b = vegetationBox;
				nearest = t;
			}
		}
	}

	return b;
}

/** Button to add a vegetation-volume was pressed */
void D2DEditorView::FillVegButtonPressed( SV_Button* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	if ( v->Selection.SelectedMesh && !v->FindVegetationFromMeshInfo( v->Selection.SelectedMesh ) ) {
		LogInfo() << "Filling selected mesh with vegetation";

		GVegetationBox* box = new GVegetationBox;
		if ( XR_SUCCESS == box->InitVegetationBox( v->Selection.SelectedMesh, "", 1.0f, 1.0f, v->Selection.SelectedMaterial->GetTexture() ) ) {
			Engine::GAPI->AddVegetationBox( box );
			//v->Selection.SelectedVegetationBox = box;
		} else {
			delete box;
		}
	}
}

/** Tab in main tab-control was switched */
void D2DEditorView::MainTabSwitched( SV_TabControl* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	//v->SetEditorMode(D2DEditorView::EM_SELECT_POLY);

	/*if (sender->GetActiveTab() == "Selection")
	{
		v->SetEditorMode(D2DEditorView::EM_SELECT_POLY);
	} else if (sender->GetActiveTab() == "Vegetation")
	{
		v->SetEditorMode(D2DEditorView::EM_PLACE_VEGETATION);
	} else
	{
		v->SetEditorMode(D2DEditorView::EM_IDLE);
	}*/
}

/** Tab in main tab-control was switched */
void D2DEditorView::RemoveVegButtonPressed( SV_Button* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	v->MMWDelta = 0;

	if ( v->Mode != EM_REMOVE_VEGETATION ) {
		v->SetEditorMode( EM_REMOVE_VEGETATION );
		sender->SetCaption( "Stop rem." );
	} else {
		v->SetEditorMode( EM_IDLE );
		sender->SetCaption( "Remove" );
	}
}


/** Save/Load-Buttons */
void D2DEditorView::SaveLevelPressed( SV_Button* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	//Engine::GAPI->SaveCustomZENResources();

	v->MainView->AddMessageBox( "Saved!", "Custom ZEN-Resources were saved!" );
}

void D2DEditorView::LoadLevelPressed( SV_Button* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	v->Selection.Reset();
	v->SelectionTabControl->SetActiveTab( "Selection/Texture" );
	Engine::GAPI->LoadCustomZENResources();
}

void D2DEditorView::InfoPressed( SV_Button* sender, void* userdata ) {
	// TODO
}

/** Tab in main tab-control was switched */
void D2DEditorView::VegetationAmountSliderChanged( SV_Slider* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	if ( v->Selection.SelectedVegetationBox ) {
		XMFLOAT3 min, max;
		v->Selection.SelectedVegetationBox->GetBoundingBox( &min, &max );
		v->Selection.SelectedVegetationBox->ResetVegetationWithDensity( sender->GetValue() );
		v->Selection.SelectedVegetationBox->SetBoundingBox( min, max );
	}
}

/** Tab in main tab-control was switched */
void D2DEditorView::VegetationScaleSliderChanged( SV_Slider* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	if ( v->Selection.SelectedVegetationBox ) {
		v->Selection.SelectedVegetationBox->ApplyUniformScaling( 1 + (sender->GetValue() - v->VegLastUniformScale) );
		v->VegLastUniformScale = sender->GetValue();
	}
}

void D2DEditorView::TextureSettingsSliderChanged( SV_Slider* sender, void* userdata ) {
	D2DEditorView* v = (D2DEditorView*)userdata;

	if ( v->Selection.SelectedMaterial && v->Selection.SelectedMaterial->GetTexture() ) {
		MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( v->Selection.SelectedMaterial->GetTexture() );

		if ( sender == v->SelectedTexNrmStrSlider->GetSlider() ) {
			info->buffer.NormalmapStrength = sender->GetValue();
		} else if ( sender == v->SelectedTexSpecIntensSlider->GetSlider() ) {
			info->buffer.SpecularIntensity = sender->GetValue();
		} else if ( sender == v->SelectedTexSpecPowerSlider->GetSlider() ) {
			info->buffer.SpecularPower = sender->GetValue();
		} else if ( sender == v->SelectedTexDisplacementSlider->GetSlider() && v->Selection.SelectedMesh ) {
		} else if ( sender == v->SelectedMeshTessAmountSlider->GetSlider() && v->Selection.SelectedMesh ) {
		} else if ( sender == v->SelectedMeshRoundnessSlider->GetSlider() && v->Selection.SelectedMesh ) {
		}

		/*else if (sender == v->SelectedTexSpecModulationSlider->GetSlider())
		{
			//info->buffer.NormalmapStrength = sender->GetValue();
		}*/

		// Update and save the info
		info->UpdateConstantbuffer();
		info->WriteToFile( v->Selection.SelectedMaterial->GetTexture()->GetNameWithoutExt() );
	}
}

/** Smoothes a mesh */
void D2DEditorView::SmoothMesh( WorldMeshInfo* mesh, bool tesselate ) {
	// Copy old vertices so we can directly write to the vectors again
	std::vector<ExVertexStruct> vxOld = mesh->Vertices;
	std::vector<unsigned short> ixOld = mesh->Indices;

	// Smooth
	//MeshModifier::DoCatmulClark(vxOld, ixOld, mesh->Vertices, mesh->Indices, 1);

	// Remove cracks (= texcoords)
	/*mesh->Vertices.clear();
	mesh->Indices.clear();
	MeshModifier::DropTexcoords(vxOld, ixOld, mesh->Vertices, mesh->Indices);*/

	// Mark dirty
	mesh->SaveInfo = true;
}

/** Called when a vob was removed from the world */
XRESULT D2DEditorView::OnVobRemovedFromWorld( zCVob* vob ) {
	bool reset = false;
	if ( TracedSkeletalVobInfo && TracedSkeletalVobInfo->Vob == vob ) {
		TracedSkeletalVobInfo = nullptr;
	}

	if ( TracedVobInfo && TracedVobInfo->Vob == vob ) {
		TracedVobInfo = nullptr;
	}

	if ( (Selection.SelectedSkeletalVob && Selection.SelectedSkeletalVob->Vob == vob) ||
		(Selection.SelectedVobInfo && Selection.SelectedVobInfo->Vob == vob) ) {
		Selection.Reset();
		VobSettingsDialog->SetHidden( true );
	}

	return XR_SUCCESS;
}
