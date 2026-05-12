#include "Widget_TransRot.h"

#include "BaseLineRenderer.h"
#include "D3D11GraphicsEngineBase.h"
#include "D3D11ShaderManager.h"
#include "EditorLinePrimitive.h"
#include "WidgetContainer.h"
#include "zCVob.h"

Widget_TransRot::Widget_TransRot( WidgetContainer* container ) : BaseWidget( container ) {
	ActiveWidget = WTR_None;
	ActiveSelection = WTR_None;

	LineVertex vx[2];
	float TransLength = BASEWIDGET_TRANS_LENGTH;
	// X-Axis
	vx[0].Position = float4( TransLength, 0, 0, 0 );
	vx[0].Color = float4( 1, 0, 0, 1 );

	vx[1].Position = float3( 0, 0, 0 );
	vx[1].Color = float4( 1, 0, 0, 1 );

	TransLines[0] = new EditorLinePrimitive;
	TransLines[0]->CreatePrimitive( vx, 2 );

	// Y-Axis
	vx[0].Position = float4( 0, TransLength, 0, 0 );
	vx[0].Color = float4( 0, 1, 0, 1 );

	vx[1].Position = float3( 0, 0, 0 );
	vx[1].Color = float4( 0, 1, 0, 1 );

	TransLines[1] = new EditorLinePrimitive;
	TransLines[1]->CreatePrimitive( vx, 2 );

	// Z-Axis
	vx[0].Position = float4( 0, 0, TransLength, 0 );
	vx[0].Color = float4( 0, 0, 1, 1 );

	vx[1].Position = float3( 0, 0, 0 );
	vx[1].Color = float4( 0, 0, 1, 1 );

	TransLines[2] = new EditorLinePrimitive;
	TransLines[2]->CreatePrimitive( vx, 2 );

	int Detail = 50;

	// Create rotation widget
	Circles[0] = new EditorLinePrimitive;
	Circles[1] = new EditorLinePrimitive;
	Circles[2] = new EditorLinePrimitive;

	Circles[0]->CreateCirclePrimitive( 1, Detail, float4( 0, 1, 0, 1 ), 0 );
	Circles[1]->CreateCirclePrimitive( 1, Detail, float4( 1, 0, 0, 1 ), 1 );
	Circles[2]->CreateCirclePrimitive( 1, Detail, float4( 0, 0, 1, 1 ), 2 );

	//LE_R(TransRotWidget.Rot_Bgr.CreateFilledCirclePrimitive(1,Detail, &XMFLOAT4(0, 0, 0, 0), 0));
	//TransRotWidget.Rot_Bgr.SetShader(ColorShader);

	//LE_R(TransRotWidget.Rot_Bgr_Line.CreateCirclePrimitive(1.025f,Detail, &XMFLOAT4(0.5, 0.5, 0.5, 1), 0));
	//TransRotWidget.Rot_Bgr_Line.SetShader(ColorShader);

	Arrows[0] = new EditorLinePrimitive;
	Arrows[1] = new EditorLinePrimitive;
	Arrows[2] = new EditorLinePrimitive;
	CreateArrowCone( 25, 0, float4( 1, 0, 0, 1 ), Arrows[0] );
	CreateArrowCone( 25, 1, float4( 0, 1, 0, 1 ), Arrows[1] );
	CreateArrowCone( 25, 2, float4( 0, 0, 1, 1 ), Arrows[2] );

	ActiveSelection = WTR_None;
}

Widget_TransRot::~Widget_TransRot() {
	delete Arrows[0];
	delete Arrows[1];
	delete Arrows[2];

	delete Circles[0];
	delete Circles[1];
	delete Circles[2];

	delete TransLines[0];
	delete TransLines[1];
	delete TransLines[2];
}

/** Called when an object was added to the selection */
void Widget_TransRot::OnSelectionAdded( zCVob* vob ) {
	XMStoreFloat3( &Position, vob->GetPositionWorldXM() );
	ApplyTransforms();
}

/** Applys the transforms to all parts of the widget */
void Widget_TransRot::ApplyTransforms() {
	// Apply transforms to selection
	auto selection = OwningContainer->GetSelection();
	for ( auto it = selection.cbegin(); it != selection.cend(); ++it ) {
		(*it)->Vob->SetPositionWorldDX( Position );

		// Get this again from the vob, in case it didn't move or something
		XMStoreFloat3( &Position, (*it)->Vob->GetPositionWorldXM() );
	}

	FXMVECTOR pos = XMLoadFloat3( &Position );
	for ( int i = 0; i < 3; i++ ) {
		Arrows[i]->SetLocation( pos );
		Circles[i]->SetLocation( pos );
		TransLines[i]->SetLocation( pos );
	}
}

/** Returns whether this widget is active or not */
bool Widget_TransRot::IsActive() const {
	return ActiveWidget != WTR_None;
}

/** Renders the widget */
void Widget_TransRot::RenderWidget() {
	if ( ActiveWidget == WTR_None ) {
		DoHoverTest( Engine::GAPI->GetOutputWindow() );
	}

	if ( ActiveWidget != WTR_None ) {
		float2 delta = GetMouseDelta();
		switch ( ActiveWidget ) {
		case WTR_TransX:
			Position.x += delta.x;
			break;

		case WTR_TransY:
			Position.y -= delta.y;
			break;

		case WTR_TransZ:
			Position.z += delta.x;
			break;
		}

		ApplyTransforms();
	}

	//return;
	static XMVECTORF32 scale = { 100, 100, 100, 0 };
	for ( int i = 0; i < 3; i++ ) {
		Arrows[i]->RenderPrimitive();
		Arrows[i]->SetScale( scale );

		//Circles[i]->RenderPrimitive();
		TransLines[i]->RenderPrimitive();
		TransLines[i]->SetScale( scale );
	}
}

/** Called when the owning window got a message */
void Widget_TransRot::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
	switch ( msg ) {
	case WM_LBUTTONDOWN:
		ActiveWidget = ActiveSelection;
		if ( ActiveWidget != WTR_None ) {
			SetMouseVisibility( false );
		}
		break;

	case WM_LBUTTONUP:
		if ( ActiveWidget != WTR_None ) {
			SetMouseVisibility( true );
		}
		ActiveWidget = WTR_None;
		break;
	}
}

/** Called when a mousebutton was clicked */
void Widget_TransRot::OnMButtonClick( int button ) {}

void Widget_TransRot::DoHoverTest( HWND hw ) {
	FXMVECTOR Dir = Engine::GAPI->UnprojectCursorXM();
	FXMVECTOR Pos = Engine::GAPI->GetCameraPositionXM();

	// Check the trans widget
	float Width = 0.15f;
	float Eps = 0.01f;

	auto ColorShaderID = PShaderID::PS_Lines;
	auto SelectedShaderID = PShaderID::PS_LinesSel;

	for ( int i = 0; i < 3; i++ ) {
		Arrows[i]->SetSolidShader( ColorShaderID );
		TransLines[i]->SetShader( ColorShaderID );
	}

	float Dist = Width;
	ActiveSelection = WTR_None;


	float TransXDist = TransLines[0]->IntersectPrimitive( Pos, Dir, Eps );
	float TransYDist = TransLines[1]->IntersectPrimitive( Pos, Dir, Eps );
	float TransZDist = TransLines[2]->IntersectPrimitive( Pos, Dir, Eps );

	ActiveSelection = WTR_None;

	if ( TransXDist < Dist ) {
		TransLines[0]->SetShader( SelectedShaderID );
		Arrows[0]->SetSolidShader( SelectedShaderID );
		Dist = TransXDist;

		ActiveSelection = WTR_TransX;
		//StartValue = (*Targets.begin())->GetLocation()->x;
	} else if ( TransYDist < Dist ) {
		TransLines[1]->SetShader( SelectedShaderID );
		Arrows[1]->SetSolidShader( SelectedShaderID );
		Dist = TransYDist;

		ActiveSelection = WTR_TransY;
		//StartValue = (*Targets.begin())->GetLocation()->y;
	} else if ( TransZDist < Dist ) {
		TransLines[2]->SetShader( SelectedShaderID );
		Arrows[2]->SetSolidShader( SelectedShaderID );
		Dist = TransZDist;

		ActiveSelection = WTR_TransZ;
		//StartValue = (*Targets.begin())->GetLocation()->z;
	}
}
