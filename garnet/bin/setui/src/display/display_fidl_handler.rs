// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::base::{SettingInfo, SettingType},
    crate::display::types::{LowLightMode, SetDisplayInfo, Theme, ThemeMode, ThemeType},
    crate::fidl_hanging_get_responder,
    crate::fidl_process,
    crate::fidl_processor::settings::RequestContext,
    crate::handler::base::Request,
    crate::request_respond,
    crate::switchboard::base::FidlResponseErrorLogger,
    fidl::endpoints::ServiceMarker,
    fidl_fuchsia_settings::{
        DisplayMarker, DisplayRequest, DisplaySettings, DisplayWatch2Responder,
        DisplayWatchLightSensor2Responder, DisplayWatchLightSensorResponder, DisplayWatchResponder,
        Error, LightSensorData, LowLightMode as FidlLowLightMode, Theme as FidlTheme,
        ThemeMode as FidlThemeMode, ThemeType as FidlThemeType,
    },
    fuchsia_async as fasync,
};

fidl_hanging_get_responder!(
    DisplayMarker,
    DisplaySettings,
    DisplayWatchResponder,
    DisplaySettings,
    DisplayWatch2Responder,
    LightSensorData,
    DisplayWatchLightSensor2Responder,
    LightSensorData,
    DisplayWatchLightSensorResponder,
);

impl From<SettingInfo> for LightSensorData {
    fn from(response: SettingInfo) -> Self {
        if let SettingInfo::LightSensor(data) = response {
            let mut sensor_data = fidl_fuchsia_settings::LightSensorData::EMPTY;
            sensor_data.illuminance_lux = Some(data.illuminance);
            sensor_data.color = Some(data.color);
            sensor_data
        } else {
            panic!("incorrect value sent to display");
        }
    }
}

impl From<FidlThemeMode> for ThemeMode {
    fn from(fidl: FidlThemeMode) -> Self {
        ThemeMode::from_bits(FidlThemeMode::bits(&fidl)).unwrap()
    }
}

impl From<ThemeMode> for FidlThemeMode {
    fn from(fidl: ThemeMode) -> Self {
        FidlThemeMode::from_bits(ThemeMode::bits(&fidl)).unwrap()
    }
}

impl From<FidlLowLightMode> for LowLightMode {
    fn from(fidl_low_light_mode: FidlLowLightMode) -> Self {
        match fidl_low_light_mode {
            FidlLowLightMode::Disable => LowLightMode::Disable,
            FidlLowLightMode::DisableImmediately => LowLightMode::DisableImmediately,
            FidlLowLightMode::Enable => LowLightMode::Enable,
        }
    }
}

impl From<FidlThemeType> for ThemeType {
    fn from(fidl_theme_type: FidlThemeType) -> Self {
        match fidl_theme_type {
            FidlThemeType::Default => ThemeType::Default,
            FidlThemeType::Auto => ThemeType::Auto,
            FidlThemeType::Light => ThemeType::Light,
            FidlThemeType::Dark => ThemeType::Dark,
        }
    }
}

impl From<FidlTheme> for Theme {
    fn from(fidl_theme: FidlTheme) -> Self {
        Self {
            theme_type: fidl_theme.theme_type.map(Into::into),
            theme_mode: fidl_theme.theme_mode.map(Into::into).unwrap_or_else(|| ThemeMode::empty()),
        }
    }
}

impl From<SettingInfo> for DisplaySettings {
    fn from(response: SettingInfo) -> Self {
        if let SettingInfo::Brightness(info) = response {
            let mut display_settings = fidl_fuchsia_settings::DisplaySettings::EMPTY;

            display_settings.auto_brightness = Some(info.auto_brightness);
            display_settings.low_light_mode = Some(match info.low_light_mode {
                LowLightMode::Enable => FidlLowLightMode::Enable,
                LowLightMode::Disable => FidlLowLightMode::Disable,
                LowLightMode::DisableImmediately => FidlLowLightMode::DisableImmediately,
            });

            if !info.auto_brightness {
                display_settings.brightness_value = Some(info.manual_brightness_value);
            }

            display_settings.screen_enabled = Some(info.screen_enabled);

            display_settings.theme = Some(FidlTheme {
                theme_type: match info.theme {
                    Some(Theme { theme_type: Some(theme_type), .. }) => match theme_type {
                        ThemeType::Unknown => None,
                        ThemeType::Default => Some(FidlThemeType::Default),
                        ThemeType::Light => Some(FidlThemeType::Light),
                        ThemeType::Dark => Some(FidlThemeType::Dark),
                        ThemeType::Auto => Some(FidlThemeType::Auto),
                    },
                    _ => None,
                },
                theme_mode: match info.theme {
                    Some(Theme { theme_mode, .. }) if !theme_mode.is_empty() => {
                        Some(FidlThemeMode::from(theme_mode))
                    }
                    _ => None,
                },
                ..FidlTheme::EMPTY
            });

            display_settings
        } else {
            panic!("incorrect value sent to display");
        }
    }
}

fn to_request(settings: DisplaySettings) -> Option<Request> {
    let set_display_info = SetDisplayInfo {
        manual_brightness_value: settings.brightness_value,
        auto_brightness: settings.auto_brightness,
        screen_enabled: settings.screen_enabled,
        low_light_mode: settings.low_light_mode.map(Into::into),
        theme: settings.theme.map(Into::into),
    };
    match set_display_info {
        // No values being set is invalid
        SetDisplayInfo {
            manual_brightness_value: None,
            auto_brightness: None,
            screen_enabled: None,
            low_light_mode: None,
            theme: None,
        } => None,
        _ => Some(Request::SetDisplayInfo(set_display_info)),
    }
}

fidl_process!(
    Display,
    SettingType::Display,
    process_request,
    SettingType::Display,
    DisplaySettings,
    DisplayWatch2Responder,
    process_request_2,
    SettingType::LightSensor,
    LightSensorData,
    DisplayWatchLightSensorResponder,
    process_sensor_request,
    SettingType::LightSensor,
    LightSensorData,
    DisplayWatchLightSensor2Responder,
    process_sensor_request_2,
);

async fn process_request(
    context: RequestContext<DisplaySettings, DisplayWatchResponder>,
    req: DisplayRequest,
) -> Result<Option<DisplayRequest>, anyhow::Error> {
    // Support future expansion of FIDL.
    #[allow(unreachable_patterns)]
    match req {
        DisplayRequest::Set { settings, responder } => {
            if let Some(request) = to_request(settings) {
                fasync::Task::spawn(async move {
                    request_respond!(
                        context,
                        responder,
                        SettingType::Display,
                        request,
                        Ok(()),
                        Err(Error::Failed),
                        DisplayMarker
                    );
                })
                .detach();
            } else {
                responder
                    .send(&mut Err(Error::Unsupported))
                    .log_fidl_response_error(DisplayMarker::DEBUG_NAME);
            }
        }
        DisplayRequest::Watch { responder } => {
            context.watch(responder, true).await;
        }
        _ => {
            return Ok(Some(req));
        }
    }

    return Ok(None);
}

async fn process_sensor_request(
    context: RequestContext<LightSensorData, DisplayWatchLightSensorResponder>,
    req: DisplayRequest,
) -> Result<Option<DisplayRequest>, anyhow::Error> {
    if let DisplayRequest::WatchLightSensor { delta, responder } = req {
        context
            .watch_with_change_fn(
                // Bucket watch requests to the nearest 0.01.
                format!("{:.2}", delta),
                Box::new(move |old_data: &LightSensorData, new_data: &LightSensorData| {
                    if let (Some(old_lux), Some(new_lux)) =
                        (old_data.illuminance_lux, new_data.illuminance_lux)
                    {
                        (new_lux - old_lux).abs() >= delta
                    } else {
                        true
                    }
                }),
                responder,
                true,
            )
            .await;
    } else {
        return Ok(Some(req));
    }

    return Ok(None);
}

async fn process_request_2(
    context: RequestContext<DisplaySettings, DisplayWatch2Responder>,
    req: DisplayRequest,
) -> Result<Option<DisplayRequest>, anyhow::Error> {
    // Support future expansion of FIDL.
    #[allow(unreachable_patterns)]
    match req {
        DisplayRequest::Watch2 { responder } => {
            context.watch(responder, true).await;
        }
        _ => {
            return Ok(Some(req));
        }
    }

    return Ok(None);
}

async fn process_sensor_request_2(
    context: RequestContext<LightSensorData, DisplayWatchLightSensor2Responder>,
    req: DisplayRequest,
) -> Result<Option<DisplayRequest>, anyhow::Error> {
    if let DisplayRequest::WatchLightSensor2 { delta, responder } = req {
        context
            .watch_with_change_fn(
                // Bucket watch requests to the nearest 0.01.
                format!("{:.2}", delta),
                Box::new(move |old_data: &LightSensorData, new_data: &LightSensorData| {
                    if let (Some(old_lux), Some(new_lux)) =
                        (old_data.illuminance_lux, new_data.illuminance_lux)
                    {
                        (new_lux - old_lux).abs() >= delta
                    } else {
                        true
                    }
                }),
                responder,
                true,
            )
            .await;
    } else {
        return Ok(Some(req));
    }

    return Ok(None);
}
