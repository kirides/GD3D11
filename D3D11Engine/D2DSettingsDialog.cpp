#include "D2DSettingsDialog.h"

#include "BaseGraphicsEngine.h"
#include "D2DView.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "SV_Checkbox.h"
#include "SV_Label.h"
#include "SV_Panel.h"
#include "SV_Slider.h"

#include "D3D11GraphicsEngine.h"

#include <locale>

Languages getUserLanguage() {
    char locale[8] = { 0 };
    GetLocaleInfoA( GetUserDefaultLangID(), LOCALE_SISO639LANGNAME, locale, sizeof( locale ) );
    if ( locale[0] == 'p' && locale[1] == 'l' ) {
        return LANGUAGE_POLISH;
    }
    return LANGUAGE_ENGLISH;
}

D2DSettingsDialog::D2DSettingsDialog( D2DView* view, D2DSubView* parent ) : D2DDialog( view, parent ) {
	SetPositionCentered( D2D1::Point2F( view->GetRenderTarget()->GetSize().width / 2, view->GetRenderTarget()->GetSize().height / 2 ), D2D1::SizeF( 520, 382 ) );

	// Get display modes
	// TODO: reenable-superresolution, workaround: Nvidia DSR/AMD VSR
	Engine::GraphicsEngine->GetDisplayModeList( &Resolutions, false );

	// Find current
    TextureQuality = 16384;
	ResolutionSetting = 0;
	for ( unsigned int i = 0; i < Resolutions.size(); i++ ) {
		if ( Resolutions[i].Width == Engine::GraphicsEngine->GetResolution().x && Resolutions[i].Height == Engine::GraphicsEngine->GetResolution().y ) {
			ResolutionSetting = i;
			break;
		}
	}

	CheckedChangedState = nullptr;
	InitControls();
}

D2DSettingsDialog::~D2DSettingsDialog() {
	SAFE_DELETE( CheckedChangedState );
}

/** Initializes the controls of this view */
XRESULT D2DSettingsDialog::InitControls() {
	D2DSubView::InitControls();

    Languages userLanguage = getUserLanguage();
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: Header->SetCaption( L"Ustawienia" ); break;
    default: Header->SetCaption( L"Settings" ); break;
    }

    switch ( userLanguage ) {
    case LANGUAGE_POLISH:
        AddButton( "Anuluj", CloseButtonPressed, this );
        AddButton( "[*] Zastosuj", ApplyButtonPressed, this );
        break;
    default:
        AddButton( "Close", CloseButtonPressed, this );
        AddButton( "[*] Apply", ApplyButtonPressed, this );
        break;
    }

	SV_Label* resolutionLabel = new SV_Label( MainView, MainPanel );
	resolutionLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	resolutionLabel->AlignUnder( Header, 5 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: resolutionLabel->SetCaption( L"Rozdzielczoœæ [*]:" ); break;
    default: resolutionLabel->SetCaption( L"Resolution [*]:" ); break;
    }
	resolutionLabel->SetPosition( D2D1::Point2F( 5, resolutionLabel->GetPosition().y ) );

	SV_Slider* resolutionSlider = new SV_Slider( MainView, MainPanel );
	resolutionSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	resolutionSlider->AlignUnder( resolutionLabel, 5 );
	resolutionSlider->SetSliderChangedCallback( ResolutionSliderChanged, this );
	resolutionSlider->SetIsIntegralSlider( true );
	resolutionSlider->SetMinMax( 0.0f, static_cast<float>(Resolutions.size()) );

	// Construct string array for resolutions slider
	std::vector<std::string> resStrings;
	for ( unsigned int i = 0; i < Resolutions.size(); i++ ) {
		std::string str = std::to_string( Resolutions[i].Width ) + "x" + std::to_string( Resolutions[i].Height );
		resStrings.emplace_back( std::move( str ) );
	}
	resolutionSlider->SetDisplayValues( resStrings );
	resolutionSlider->SetValue( static_cast<float>(ResolutionSetting) );

    SV_Label* modeLabel = new SV_Label( MainView, MainPanel );
    modeLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
    modeLabel->AlignUnder( resolutionSlider, 5 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: modeLabel->SetCaption( L"Tryb [**]:" ); break;
    default: modeLabel->SetCaption( L"Mode [**]:" ); break;
    }

    modeSlider = new SV_Slider( MainView, MainPanel );
    modeSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
    modeSlider->AlignUnder( modeLabel, 5 );
    modeSlider->SetSliderChangedCallback( ModeSliderChanged, this );
    modeSlider->SetIsIntegralSlider( true );
    modeSlider->SetMinMax( 1.0f, 4.0f );
    modeSlider->SetDisplayValues( { "0", "Fullscreen Exclusive", "Fullscreen Borderless", "Fullscreen Lowlatency", "Windowed" } );
    modeSlider->SetValue( 1.0f );

	SV_Checkbox* normalmapsCheckbox = new SV_Checkbox( MainView, MainPanel );
	normalmapsCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: normalmapsCheckbox->SetCaption( L"Normalmapy" ); break;
    default: normalmapsCheckbox->SetCaption( L"Enable Normalmaps" ); break;
    }
	normalmapsCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps );
	normalmapsCheckbox->AlignUnder( modeSlider, 10 );
	normalmapsCheckbox->SetPosition( D2D1::Point2F( 5, normalmapsCheckbox->GetPosition().y ) );
	normalmapsCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps );

	SV_Checkbox* numpadCheckbox = new SV_Checkbox( MainView, MainPanel );
	numpadCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: numpadCheckbox->SetCaption( L"Klawisze Numeryczne" ); break;
    default: numpadCheckbox->SetCaption( L"Enable Numpad Keys" ); break;
    }
	numpadCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys );
	numpadCheckbox->AlignUnder( normalmapsCheckbox, 5 );
	numpadCheckbox->SetPosition( D2D1::Point2F( 5, numpadCheckbox->GetPosition().y ) );
	numpadCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys );

	SV_Checkbox* hbaoCheckbox = new SV_Checkbox( MainView, MainPanel );
	hbaoCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: hbaoCheckbox->SetCaption( L"HBAO+" ); break;
    default: hbaoCheckbox->SetCaption( L"Enable HBAO+" ); break;
    }
	hbaoCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Enabled );
	hbaoCheckbox->AlignUnder( numpadCheckbox, 5 );
	hbaoCheckbox->SetPosition( D2D1::Point2F( 5, hbaoCheckbox->GetPosition().y ) );
	hbaoCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Enabled );
    if ( FeatureLevel10Compatibility ) {
        hbaoCheckbox->SetDisabled( true );
    }

	SV_Checkbox* vsyncCheckbox = new SV_Checkbox( MainView, MainPanel );
	vsyncCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: vsyncCheckbox->SetCaption( L"Synchronizacja Pionowa" ); break;
    default: vsyncCheckbox->SetCaption( L"Enable VSync" ); break;
    }
	vsyncCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.EnableVSync );
	vsyncCheckbox->AlignUnder( hbaoCheckbox, 5 );
	vsyncCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.EnableVSync );

	SV_Checkbox* godraysCheckbox = new SV_Checkbox( MainView, MainPanel );
	godraysCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: godraysCheckbox->SetCaption( L"GodRays" ); break;
    default: godraysCheckbox->SetCaption( L"Enable GodRays" ); break;
    }
	godraysCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.EnableGodRays );
	godraysCheckbox->AlignUnder( vsyncCheckbox, 5 );
	godraysCheckbox->SetPosition( D2D1::Point2F( 5, godraysCheckbox->GetPosition().y ) );
	godraysCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.EnableGodRays );

	SV_Checkbox* smaaCheckbox = new SV_Checkbox( MainView, MainPanel );
	smaaCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: smaaCheckbox->SetCaption( L"SMAA" ); break;
    default: smaaCheckbox->SetCaption( L"Enable SMAA" ); break;
    }
	smaaCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.EnableSMAA );
	smaaCheckbox->AlignUnder( godraysCheckbox, 5 );
	smaaCheckbox->SetPosition( D2D1::Point2F( 5, smaaCheckbox->GetPosition().y ) );
	smaaCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.EnableSMAA );
    if ( FeatureLevel10Compatibility ) {
        smaaCheckbox->SetDisabled( true );
    }

	/*SV_Checkbox* tesselationCheckbox = new SV_Checkbox(MainView, MainPanel);
	tesselationCheckbox->SetSize(D2D1::SizeF(160, 20));
	tesselationCheckbox->SetCaption("Enable Tesselation");
	tesselationCheckbox->SetDataToUpdate(&Engine::GAPI->GetRendererState().RendererSettings.EnableTesselation);
	tesselationCheckbox->AlignUnder(smaaCheckbox, 5);
	tesselationCheckbox->SetPosition(D2D1::Point2F(5, tesselationCheckbox->GetPosition().y));
	tesselationCheckbox->SetChecked(Engine::GAPI->GetRendererState().RendererSettings.EnableTesselation);*/

	SV_Checkbox* hdrCheckbox = new SV_Checkbox( MainView, MainPanel );
	hdrCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: hdrCheckbox->SetCaption( L"HDR" ); break;
    default: hdrCheckbox->SetCaption( L"Enable HDR" ); break;
    }
	hdrCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.EnableHDR );
	hdrCheckbox->AlignUnder( smaaCheckbox, 5 );
	hdrCheckbox->SetPosition( D2D1::Point2F( 5, hdrCheckbox->GetPosition().y ) );
	hdrCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.EnableHDR );

    InitialSettings.EnableShadows = Engine::GAPI->GetRendererState().RendererSettings.EnableShadows;
	SV_Checkbox* shadowsCheckbox = new SV_Checkbox( MainView, MainPanel );
	shadowsCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: shadowsCheckbox->SetCaption( L"Cienie [*]" ); break;
    default: shadowsCheckbox->SetCaption( L"Enable Shadows [*]" ); break;
    }
	shadowsCheckbox->SetDataToUpdate( &InitialSettings.EnableShadows );
	shadowsCheckbox->AlignUnder( hdrCheckbox, 5 );
	shadowsCheckbox->SetPosition( D2D1::Point2F( 5, shadowsCheckbox->GetPosition().y ) );
	shadowsCheckbox->SetChecked( InitialSettings.EnableShadows );

    InitialSettings.EnableSoftShadows = Engine::GAPI->GetRendererState().RendererSettings.EnableSoftShadows;
	SV_Checkbox* filterShadowsCheckbox = new SV_Checkbox( MainView, MainPanel );
	filterShadowsCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: filterShadowsCheckbox->SetCaption( L"Filtrowanie Cieni [*]" ); break;
    default: filterShadowsCheckbox->SetCaption( L"Shadow Filtering [*]" ); break;
    }
	filterShadowsCheckbox->SetDataToUpdate( &InitialSettings.EnableSoftShadows );
	filterShadowsCheckbox->AlignUnder( shadowsCheckbox, 5 );
	filterShadowsCheckbox->SetChecked( InitialSettings.EnableSoftShadows );

    SV_Checkbox* compressBackBufferCheckbox = new SV_Checkbox( MainView, MainPanel );
    compressBackBufferCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: compressBackBufferCheckbox->SetCaption( L"Skompresuj Kolory" ); break;
    default: compressBackBufferCheckbox->SetCaption( L"Compress BackBuffer" ); break;
    }
    compressBackBufferCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer );
    compressBackBufferCheckbox->AlignUnder( filterShadowsCheckbox, 5 );
    compressBackBufferCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer );
    compressBackBufferCheckbox->SetCheckedChangedCallback( CompressBackBufferCheckedChanged, this );

    SV_Checkbox* animateStaticModelsCheckbox = new SV_Checkbox( MainView, MainPanel );
    animateStaticModelsCheckbox->SetSize( D2D1::SizeF( 160, 20 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: animateStaticModelsCheckbox->SetCaption( L"Animuj Statyczne Voby" ); break;
    default: animateStaticModelsCheckbox->SetCaption( L"Animate Static Vobs" ); break;
    }
    animateStaticModelsCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.AnimateStaticVobs );
    animateStaticModelsCheckbox->AlignUnder( compressBackBufferCheckbox, 5 );
    animateStaticModelsCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.AnimateStaticVobs );

	SV_Label* fpsLimitLabel = new SV_Label( MainView, MainPanel );
	fpsLimitLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	fpsLimitLabel->AlignUnder( animateStaticModelsCheckbox, 10 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: fpsLimitLabel->SetCaption( L"Limit Klatek(FPS):" ); break;
    default: fpsLimitLabel->SetCaption( L"Framerate Limit:" ); break;
    }

	SV_Slider* fpsLimitSlider = new SV_Slider( MainView, MainPanel );
	fpsLimitSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	fpsLimitSlider->AlignUnder( fpsLimitLabel, 5 );
	fpsLimitSlider->SetSliderChangedCallback( FpsLimitSliderChanged, this );
	fpsLimitSlider->SetIsIntegralSlider( true );

	const int FPSLIMIT_MAX = 240;
	fpsLimitSlider->SetMinMax( 0.0f, static_cast<float>( FPSLIMIT_MAX ) );
	std::vector<std::string> fpsValues;
	for ( size_t i = 0; i <= FPSLIMIT_MAX; i++ ) {
		if ( i <= 25 ) {
			fpsValues.emplace_back( "off" );
		} else {
            if ( i >= 28 && i <= 32 ) {
                fpsValues.emplace_back( "30" );
            } else if ( i >= 58 && i <= 62 ) {
                fpsValues.emplace_back( "60" );
            } else if ( i >= 73 && i <= 77 ) {
                fpsValues.emplace_back( "75" );
            } else if ( i >= 88 && i <= 92 ) {
                fpsValues.emplace_back( "90" );
            } else if ( i >= 98 && i <= 102 ) {
                fpsValues.emplace_back( "100" );
            } else if ( i >= 118 && i <= 122 ) {
                fpsValues.emplace_back( "120" );
            } else if ( i >= 142 && i <= 146 ) {
                fpsValues.emplace_back( "144" );
            } else {
                fpsValues.emplace_back( std::move( std::to_string( i ) ) );
            }
		}
	}
	fpsLimitSlider->SetDisplayValues( fpsValues );

	// Fix the fps value
	fpsLimitSlider->SetValue( static_cast<float>(Engine::GAPI->GetRendererState().RendererSettings.FpsLimit) );

	// Next column
    SV_Label* textureQualityLabel = new SV_Label( MainView, MainPanel );
    textureQualityLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
    textureQualityLabel->AlignUnder( Header, 5 );
    textureQualityLabel->SetPosition( D2D1::Point2F( 180, textureQualityLabel->GetPosition().y ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: textureQualityLabel->SetCaption( L"Jakoœæ Textur [*]:" ); break;
    default: textureQualityLabel->SetCaption( L"Texture Quality [*]:" ); break;
    }

    SV_Slider* textureQualitySlider = new SV_Slider( MainView, MainPanel );
    textureQualitySlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
    textureQualitySlider->AlignUnder( textureQualityLabel, 5 );
    textureQualitySlider->SetSliderChangedCallback( TextureQualitySliderChanged, this );
    textureQualitySlider->SetDisplayValues( { "0", "Potato", "Ultra Low", "Low", "Medium", "High", "Ultra High" } );
    textureQualitySlider->SetIsIntegralSlider( true );
    textureQualitySlider->SetMinMax( 1.0f, 6.0f );

    // Fix the texture quality range
    TextureQuality = Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize;
    switch ( Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize ) {
    case   32: textureQualitySlider->SetValue( 1 ); break;
    case   64: textureQualitySlider->SetValue( 2 ); break;
    case  128: textureQualitySlider->SetValue( 3 ); break;
    case  256: textureQualitySlider->SetValue( 4 ); break;
    case  512: textureQualitySlider->SetValue( 5 ); break;
    case 16384: textureQualitySlider->SetValue( 6 ); break;
    }

	SV_Label* outdoorVobsDDLabel = new SV_Label( MainView, MainPanel );
	outdoorVobsDDLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 160, 12 ) );
	outdoorVobsDDLabel->AlignUnder( textureQualitySlider, 10 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: outdoorVobsDDLabel->SetCaption( L"Zasiêg Rysowania Obiektów:" ); break;
    default: outdoorVobsDDLabel->SetCaption( L"Object draw distance:" ); break;
    }

	SV_Slider* outdoorVobsDDSlider = new SV_Slider( MainView, MainPanel );
	outdoorVobsDDSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	outdoorVobsDDSlider->AlignUnder( outdoorVobsDDLabel, 5 );
	outdoorVobsDDSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius );
	outdoorVobsDDSlider->SetIsIntegralSlider( true );
	outdoorVobsDDSlider->SetDisplayMultiplier( 0.001f );
	if ( GMPModeActive ) {
		outdoorVobsDDSlider->SetMinMax( 20000.0f, 99999.0f );
	} else {
		outdoorVobsDDSlider->SetMinMax( 0.0f, 99999.0f );
	}
	outdoorVobsDDSlider->SetValue( Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius );

	SV_Label* outdoorVobsSmallDDLabel = new SV_Label( MainView, MainPanel );
	outdoorVobsSmallDDLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 170, 12 ) );
	outdoorVobsSmallDDLabel->AlignUnder( outdoorVobsDDSlider, 8 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: outdoorVobsSmallDDLabel->SetCaption( L"Zasiêg Rys. Ma³ych Obiektów:" ); break;
    default: outdoorVobsSmallDDLabel->SetCaption( L"Small object draw distance:" ); break;
    }

	SV_Slider* outdoorVobsSmallDDSlider = new SV_Slider( MainView, MainPanel );
	outdoorVobsSmallDDSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	outdoorVobsSmallDDSlider->AlignUnder( outdoorVobsSmallDDLabel, 5 );
	outdoorVobsSmallDDSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius );
	outdoorVobsSmallDDSlider->SetIsIntegralSlider( true );
	outdoorVobsSmallDDSlider->SetDisplayMultiplier( 0.001f );
	if ( GMPModeActive ) {
		outdoorVobsSmallDDSlider->SetMinMax( 20000.0f, 99999.0f );
	} else {
		outdoorVobsSmallDDSlider->SetMinMax( 0.0f, 99999.0f );
	}
	outdoorVobsSmallDDSlider->SetValue( Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius );

	SV_Label* visualFXDDLabel = new SV_Label( MainView, MainPanel );
	visualFXDDLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	visualFXDDLabel->AlignUnder( outdoorVobsSmallDDSlider, 8 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: visualFXDDLabel->SetCaption( L"Zasiêg Rysowania Efektów:" ); break;
    default: visualFXDDLabel->SetCaption( L"VisualFX draw distance:" ); break;
    }

	SV_Slider* visualFXDDSlider = new SV_Slider( MainView, MainPanel );
	visualFXDDSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
    visualFXDDSlider->AlignUnder( visualFXDDLabel, 10 );
	visualFXDDSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius );
	visualFXDDSlider->SetIsIntegralSlider( true );
	visualFXDDSlider->SetDisplayMultiplier( 0.001f );
    visualFXDDSlider->SetMinMax( 0.0f, 30000.0f );
	visualFXDDSlider->SetValue( Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius );

	SV_Label* worldDDLabel = new SV_Label( MainView, MainPanel );
	worldDDLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	worldDDLabel->AlignUnder( visualFXDDSlider, 8 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: worldDDLabel->SetCaption( L"Zasiêg Rysowania Œwiata:" ); break;
    default: worldDDLabel->SetCaption( L"World draw distance:" ); break;
    }

	SV_Slider* worldDDSlider = new SV_Slider( MainView, MainPanel );
	worldDDSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	worldDDSlider->AlignUnder( worldDDLabel, 5 );
	worldDDSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius );
	worldDDSlider->SetIsIntegralSlider( true );
	worldDDSlider->SetMinMax( 1.0f, 20.0f );
	worldDDSlider->SetValue( static_cast<float>(Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius) );

    SV_Label* shadowmapSizeLabel = new SV_Label( MainView, MainPanel );
    shadowmapSizeLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
    shadowmapSizeLabel->AlignUnder( worldDDSlider, 10 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: shadowmapSizeLabel->SetCaption( L"Jakoœæ Cieni:" ); break;
    default: shadowmapSizeLabel->SetCaption( L"Shadow Quality:" ); break;
    }

    SV_Slider* shadowmapSizeSlider = new SV_Slider( MainView, MainPanel );
    shadowmapSizeSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
    shadowmapSizeSlider->AlignUnder( shadowmapSizeLabel, 5 );
    shadowmapSizeSlider->SetSliderChangedCallback( ShadowQualitySliderChanged, this );
    if ( FeatureLevel10Compatibility ) {
        shadowmapSizeSlider->SetDisplayValues( { "0", "512", "1024", "2048", "4096", "8192" } );
    } else {
        shadowmapSizeSlider->SetDisplayValues( { "0", "512", "1024", "2048", "4096", "8192", "16384" } );
    }
    shadowmapSizeSlider->SetIsIntegralSlider( true );
    shadowmapSizeSlider->SetMinMax( 1.0f, 6.0f );

    // Fix the shadow range
    switch ( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize ) {
    case   512: shadowmapSizeSlider->SetValue( 1 ); break;
    case  1024: shadowmapSizeSlider->SetValue( 2 ); break;
    case  2048: shadowmapSizeSlider->SetValue( 3 ); break;
    case  4096: shadowmapSizeSlider->SetValue( 4 ); break;
    case  8192: shadowmapSizeSlider->SetValue( 5 ); break;
    case 16384: shadowmapSizeSlider->SetValue( 6 ); break;
    }

	SV_Label* dynShadowLabel = new SV_Label( MainView, MainPanel );
	dynShadowLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	dynShadowLabel->AlignUnder( shadowmapSizeSlider, 8 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: dynShadowLabel->SetCaption( L"Dynamiczne Cienie:" ); break;
    default: dynShadowLabel->SetCaption( L"Dynamic shadows:" ); break;
    }

	SV_Slider* dynShadowSlider = new SV_Slider( MainView, MainPanel );
	dynShadowSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	dynShadowSlider->AlignUnder( dynShadowLabel, 5 );
	dynShadowSlider->SetDataToUpdate( reinterpret_cast<int*>(&Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows) );
	dynShadowSlider->SetIsIntegralSlider( true );
	dynShadowSlider->SetMinMax( 0.0f, GothicRendererSettings::_PLS_NUM_SETTINGS - 1 );
	dynShadowSlider->SetDisplayValues( { "Disabled", "Static", "Update dynamic", "Full" } );
	dynShadowSlider->SetValue( static_cast<float>(Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows) );

    SV_Checkbox* drawSectionIntersectionsCheckbox = new SV_Checkbox( MainView, MainPanel );
    drawSectionIntersectionsCheckbox->SetSize( D2D1::SizeF( 160, 35 ) );
    drawSectionIntersectionsCheckbox->SetCaption( L"Draw World Section Intersections" );
    drawSectionIntersectionsCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.DrawSectionIntersections );
    drawSectionIntersectionsCheckbox->AlignUnder( dynShadowSlider, 17 );
    drawSectionIntersectionsCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.DrawSectionIntersections );

	// Third column
	// FOV
	SV_Checkbox* fovOverrideCheckbox = new SV_Checkbox( MainView, MainPanel );
	fovOverrideCheckbox->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 160, 20 ) );
	fovOverrideCheckbox->AlignUnder( Header, 5 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: fovOverrideCheckbox->SetCaption( L"Zast¹p Pole Widzenia" ); break;
    default: fovOverrideCheckbox->SetCaption( L"Enable FOV Override" ); break;
    }
	fovOverrideCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.ForceFOV );
	fovOverrideCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.ForceFOV );
	fovOverrideCheckbox->SetPosition( D2D1::Point2F( 170 + 160 + 30, fovOverrideCheckbox->GetPosition().y ) );

	SV_Label* horizFOVLabel = new SV_Label( MainView, MainPanel );
	horizFOVLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	horizFOVLabel->AlignUnder( fovOverrideCheckbox, 8 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: horizFOVLabel->SetCaption( L"Poziome Widzenie:" ); break;
    default: horizFOVLabel->SetCaption( L"Horizontal FOV:" ); break;
    }
	horizFOVLabel->SetPosition( D2D1::Point2F( 170 + 160 + 30, horizFOVLabel->GetPosition().y ) );

	SV_Slider* horizFOVSlider = new SV_Slider( MainView, MainPanel );
	horizFOVSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	horizFOVSlider->AlignUnder( horizFOVLabel, 5 );
	horizFOVSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.FOVHoriz );
	horizFOVSlider->SetIsIntegralSlider( true );
	horizFOVSlider->SetMinMax( 40.0f, 150.0f );
	horizFOVSlider->SetValue( Engine::GAPI->GetRendererState().RendererSettings.FOVHoriz );

	SV_Label* vertFOVLabel = new SV_Label( MainView, MainPanel );
	vertFOVLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	vertFOVLabel->AlignUnder( horizFOVSlider, 8 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: vertFOVLabel->SetCaption( L"Pionowe Widzenie:" ); break;
    default: vertFOVLabel->SetCaption( L"Vertical FOV:" ); break;
    }

	SV_Slider* vertFOVSlider = new SV_Slider( MainView, MainPanel );
	vertFOVSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	vertFOVSlider->AlignUnder( vertFOVLabel, 5 );
	vertFOVSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.FOVVert );
	vertFOVSlider->SetIsIntegralSlider( true );
	vertFOVSlider->SetMinMax( 40.0f, 150.0f );
	vertFOVSlider->SetValue( Engine::GAPI->GetRendererState().RendererSettings.FOVVert );

	/* THIS BELONGS TO FovOverrideCheckbox ! */
	CheckedChangedState = new FovOverrideCheckedChangedState;
	CheckedChangedState->SettingsDialog = this;
	CheckedChangedState->horizFOVLabel = horizFOVLabel;
	CheckedChangedState->horizFOVSlider = horizFOVSlider;
	CheckedChangedState->vertFOVLabel = vertFOVLabel;
	CheckedChangedState->vertFOVSlider = vertFOVSlider;

	fovOverrideCheckbox->SetCheckedChangedCallback( FovOverrideCheckedChanged, CheckedChangedState );
	FovOverrideCheckedChanged( fovOverrideCheckbox, CheckedChangedState ); // Apply initial settings

	SV_Label* brightnessLabel = new SV_Label( MainView, MainPanel );
	brightnessLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	brightnessLabel->AlignUnder( vertFOVSlider, 16 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: brightnessLabel->SetCaption( L"Jasnoœæ:" ); break;
    default: brightnessLabel->SetCaption( L"Brightness:" ); break;
    }

	SV_Slider* brightnessSlider = new SV_Slider( MainView, MainPanel );
	brightnessSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	brightnessSlider->AlignUnder( brightnessLabel, 5 );
	brightnessSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.BrightnessValue );
	brightnessSlider->SetMinMax( 0.1f, 3.0f );
	brightnessSlider->SetValue( Engine::GAPI->GetRendererState().RendererSettings.BrightnessValue );

	SV_Label* contrastLabel = new SV_Label( MainView, MainPanel );
	contrastLabel->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 150, 12 ) );
	contrastLabel->AlignUnder( brightnessSlider, 8 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: contrastLabel->SetCaption( L"Kontrast:" ); break;
    default: contrastLabel->SetCaption( L"Contrast:" ); break;
    }

	SV_Slider* contrastSlider = new SV_Slider( MainView, MainPanel );
	contrastSlider->SetPositionAndSize( D2D1::Point2F( 10, 22 ), D2D1::SizeF( 150, 15 ) );
	contrastSlider->AlignUnder( contrastLabel, 5 );
	contrastSlider->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.GammaValue );
	contrastSlider->SetMinMax( 0.1f, 2.0f );
	contrastSlider->SetValue( Engine::GAPI->GetRendererState().RendererSettings.GammaValue );

    SV_Checkbox* rainCheckbox = new SV_Checkbox( MainView, MainPanel );
    rainCheckbox->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 160, 20 ) );
    rainCheckbox->AlignUnder( contrastSlider, 65 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: rainCheckbox->SetCaption( L"W³¹cz Deszcz" ); break;
    default: rainCheckbox->SetCaption( L"Enable Rain" ); break;
    }
    rainCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.EnableRain );
    rainCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.EnableRain );
    rainCheckbox->SetPosition( D2D1::Point2F( 170 + 160 + 20, rainCheckbox->GetPosition().y ) );

    SV_Checkbox* rainEffectsCheckbox = new SV_Checkbox( MainView, MainPanel );
    rainEffectsCheckbox->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 160, 20 ) );
    rainEffectsCheckbox->AlignUnder( rainCheckbox, 5 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: rainEffectsCheckbox->SetCaption( L"W³¹cz Efekty Deszczu" ); break;
    default: rainEffectsCheckbox->SetCaption( L"Enable Rain Effects" ); break;
    }
    rainEffectsCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.EnableRainEffects );
    rainEffectsCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.EnableRainEffects );

    SV_Checkbox* lightCheckbox = new SV_Checkbox( MainView, MainPanel );
    lightCheckbox->SetPositionAndSize( D2D1::Point2F( 10, 10 ), D2D1::SizeF( 160, 20 ) );
    lightCheckbox->AlignUnder( rainEffectsCheckbox, 15 );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: lightCheckbox->SetCaption( L"Ogranicz Natê¿enie Œwiat³a" ); break;
    default: lightCheckbox->SetCaption( L"Limit Light Intensity" ); break;
    }
    lightCheckbox->SetDataToUpdate( &Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntesity );
    lightCheckbox->SetChecked( Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntesity );

    // Mode changing label
    SV_Label* modeChangingLabel = new SV_Label( MainView, MainPanel );
    modeChangingLabel->SetPositionAndSize( D2D1::Point2F( 5, GetSize().height - 5 - 30 ), D2D1::SizeF( 320, 12 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: modeChangingLabel->SetCaption( L"[**] -> Musisz zrestartowaæ grê ¿eby zmiany zadzia³a³y" ); break;
    default: modeChangingLabel->SetCaption( L"[**] -> You must restart game for the changes to take effect" ); break;
    }

	// Advanced settings label
	SV_Label* advancedSettingsLabel = new SV_Label( MainView, MainPanel );
	advancedSettingsLabel->SetPositionAndSize( D2D1::Point2F( 5, GetSize().height - 5 - 12 ), D2D1::SizeF( 300, 12 ) );
    switch ( userLanguage ) {
    case LANGUAGE_POLISH: advancedSettingsLabel->SetCaption( L"CTRL + F11 -> Zaawansowane Ustawienia" ); break;
    default: advancedSettingsLabel->SetCaption( L"CTRL + F11 -> Advanced Settings" ); break;
    }

	return XR_SUCCESS;
}

void D2DSettingsDialog::FpsLimitSliderChanged( SV_Slider* sender, void* userdata ) {
	int newValue = static_cast<int>(sender->GetValue());
    if ( newValue <= 25 ) {
        newValue = 0;
    } else if ( newValue >= 28 && newValue <= 32 ) {
        newValue = 30;
    } else if ( newValue >= 58 && newValue <= 62 ) {
        newValue = 60;
    } else if ( newValue >= 73 && newValue <= 77 ) {
        newValue = 75;
    } else if ( newValue >= 88 && newValue <= 92 ) {
        newValue = 90;
    } else if ( newValue >= 98 && newValue <= 102 ) {
        newValue = 100;
    } else if ( newValue >= 118 && newValue <= 122 ) {
        newValue = 120;
    } else if ( newValue >= 142 && newValue <= 146 ) {
        newValue = 144;
    }
	Engine::GAPI->GetRendererState().RendererSettings.FpsLimit = newValue;
}

void D2DSettingsDialog::FovOverrideCheckedChanged( SV_Checkbox* sender, void* userdata ) {
	FovOverrideCheckedChangedState* state = reinterpret_cast<FovOverrideCheckedChangedState*>(userdata);
	auto newValue = sender->GetChecked();
	state->horizFOVLabel->SetDisabled( !newValue );
	state->horizFOVSlider->SetDisabled( !newValue );
	state->vertFOVLabel->SetDisabled( !newValue );
	state->vertFOVSlider->SetDisabled( !newValue );
}

void D2DSettingsDialog::MTResourceManagerCheckedChanged( SV_Checkbox*, void* ) {
    Engine::GAPI->UpdateMTResourceManager();
}

void D2DSettingsDialog::CompressBackBufferCheckedChanged( SV_Checkbox*, void* ) {
    Engine::GAPI->UpdateCompressBackBuffer();
}

/** Tab in main tab-control was switched */
void D2DSettingsDialog::TextureQualitySliderChanged( SV_Slider* sender, void* userdata ) {
    D2DSettingsDialog* d = reinterpret_cast<D2DSettingsDialog*>(userdata);
    switch ( static_cast<int>(sender->GetValue() + 0.5f) ) {
    case 1: d->TextureQuality = 32; break;
    case 2: d->TextureQuality = 64; break;
    case 3: d->TextureQuality = 128; break;
    case 4: d->TextureQuality = 256; break;
    case 5: d->TextureQuality = 512; break;
    case 6: d->TextureQuality = 16384; break;
    }
}

void D2DSettingsDialog::ShadowQualitySliderChanged( SV_Slider* sender, void* userdata ) {
	switch ( static_cast<int>(sender->GetValue() + 0.5f) ) {
	case 1: Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 512; break;
	case 2: Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 1024; break;
	case 3: Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 2048; break;
	case 4: Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 4096; break;
	case 5: Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 8192; break;
	case 6: Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 16384; break;
	}
}

void D2DSettingsDialog::ResolutionSliderChanged( SV_Slider* sender, void* userdata ) {
	D2DSettingsDialog* d = reinterpret_cast<D2DSettingsDialog*>(userdata);

	unsigned int val = static_cast<unsigned int>(sender->GetValue() + 0.5f);

	if ( val >= d->Resolutions.size() )
		val = d->Resolutions.size() - 1;

	d->ResolutionSetting = val;
}

void D2DSettingsDialog::ModeSliderChanged( SV_Slider* sender, void* userdata ) {
    D2DSettingsDialog* d = reinterpret_cast<D2DSettingsDialog*>(userdata);

    int val = static_cast<int>(sender->GetValue() + 0.5f);
    d->CurrentWindowMode = val;
}

/** Close button */
void D2DSettingsDialog::CloseButtonPressed( SV_Button* sender, void* userdata ) {
	D2DSettingsDialog* d = reinterpret_cast<D2DSettingsDialog*>(userdata);

	if ( d->NeedsApply() )
		d->ApplyButtonPressed( sender, userdata );

	d->SetHidden( true );

	Engine::GAPI->SetEnableGothicInput( true );
}

/** Apply button */
void D2DSettingsDialog::ApplyButtonPressed( SV_Button* sender, void* userdata ) {
	GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
	D2DSettingsDialog* d = reinterpret_cast<D2DSettingsDialog*>(userdata);

	// Check for shader reload
	if ( d->InitialSettings.EnableShadows != settings.EnableShadows || d->InitialSettings.EnableSoftShadows != settings.EnableSoftShadows ) {
        settings.EnableShadows = d->InitialSettings.EnableShadows;
        settings.EnableSoftShadows = d->InitialSettings.EnableSoftShadows;
		Engine::GraphicsEngine->ReloadShaders();
	}

    // Check for texture quality change
    if ( d->TextureQuality != settings.textureMaxSize ) {
        settings.textureMaxSize = d->TextureQuality;
        Engine::GAPI->UpdateTextureMaxSize();
    }

    // Check for mode change
    if ( d->CurrentWindowMode != d->ActiveWindowMode ) {
        d->ActiveWindowMode = d->CurrentWindowMode;
        settings.ChangeWindowPreset = d->ActiveWindowMode;
    }

	// Check for resolution change
	if ( d->Resolutions[d->ResolutionSetting].Width != Engine::GraphicsEngine->GetResolution().x || d->Resolutions[d->ResolutionSetting].Height != Engine::GraphicsEngine->GetResolution().y ) {
		Engine::GraphicsEngine->OnResize( INT2(d->Resolutions[d->ResolutionSetting].Width, d->Resolutions[d->ResolutionSetting].Height) );
        // reposition the window at the center, 
        // or we might not be able to see it 
        d->SetPositionCentered( D2D1::Point2F( d->MainView->GetRenderTarget()->GetSize().width / 2, d->MainView->GetRenderTarget()->GetSize().height / 2 ), D2D1::SizeF( 520, 382 ) );
	}
	Engine::GAPI->SaveRendererWorldSettings( settings );
	Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
}

/** Checks if a change needs to reload the shaders */
bool D2DSettingsDialog::NeedsApply() {
	GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;

	// Check for shader reload
	if ( InitialSettings.EnableShadows != settings.EnableShadows || InitialSettings.EnableSoftShadows != settings.EnableSoftShadows ) {
		return true;
	}

	// Check for resolution change
	if ( Resolutions[ResolutionSetting].Width != Engine::GraphicsEngine->GetResolution().x || Resolutions[ResolutionSetting].Height != Engine::GraphicsEngine->GetResolution().y ) {
		return true;
	}

	return false;
}

/** Called when the settings got re-opened */
void D2DSettingsDialog::OnOpenedSettings() {
	//InitialSettings = Engine::GAPI->GetRendererState().RendererSettings;

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    CurrentWindowMode = engine->GetWindowMode();
    ActiveWindowMode = CurrentWindowMode;
    modeSlider->SetValue( static_cast<float>(CurrentWindowMode) );
}

/** Sets if this control is hidden */
void D2DSettingsDialog::SetHidden( bool hidden ) {
	if ( IsHidden() && !hidden )
		OnOpenedSettings(); // Changed visibility from hidden to non-hidden

	D2DDialog::SetHidden( hidden );
    if ( hidden ) {
        Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_ClosedSettings );
    }
}
