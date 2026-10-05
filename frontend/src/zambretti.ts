/**
 * Zambretti Barometric Forecaster Engine
 * Implementation based on the classic Negretti & Zambretti barometric forecasting algorithm (1915),
 * adapted for digital micro-adjustment of Numerical Weather Prediction (NWP) models.
 * 
 * Computes 3-hour pressure tendency (dp/dt) and maps local barometric pressure
 * reduced to sea level into 26 empirical local weather forecasts.
 */

export enum BarometricTendency {
  RISING_RAPIDLY = 'RISING_RAPIDLY',       // > +2.0 hPa / 3h
  RISING_SLOWLY = 'RISING_SLOWLY',         // +0.5 to +2.0 hPa / 3h
  STEADY = 'STEADY',                       // -0.5 to +0.5 hPa / 3h
  FALLING_SLOWLY = 'FALLING_SLOWLY',       // -2.0 to -0.5 hPa / 3h
  FALLING_RAPIDLY = 'FALLING_RAPIDLY',     // < -2.0 hPa / 3h
}

export enum Season {
  WINTER = 'WINTER',
  SUMMER = 'SUMMER',
}

export interface ZambrettiForecast {
  zambretti_code: string;
  zambretti_number: number;
  forecast_text: string;
  barometric_trend: BarometricTendency;
  dp_dt_3h: number;
  confidence_score: number;
}

const ZAMBRETTI_LOOKUP: Record<number, string> = {
  1: 'Settled Fine Weather',
  2: 'Fine Weather',
  3: 'Becoming Fine',
  4: 'Fairly Fine, Improving',
  5: 'Fairly Fine, Possible Showers Early',
  6: 'Fairly Fine, Showers Later',
  7: 'Showers Early, Improving',
  8: 'Changeable, Mending',
  9: 'Fairly Fine, Showers Likely',
  10: 'Mainly Cloudy, Showers Likely',
  11: 'Fairly Fine, Showers Later',
  12: 'Changeable, Rain at Times',
  13: 'Fairly Fine, Rain Later',
  14: 'Showery, Weather Deteriorating',
  15: 'Changeable, Rain Frequent',
  16: 'Rain at Times, Becoming Very Stormy',
  17: 'Rain at Frequent Intervals',
  18: 'Very Unsettled, Rain',
  19: 'Stormy, Much Rain',
  20: 'Settled Fine',
  21: 'Fine Weather',
  22: 'Becoming Fine',
  23: 'Fairly Fine, Improving',
  24: 'Fairly Fine, Showery Early',
  25: 'Showery Early, Improving',
  26: 'Changeable, Mending',
};

/**
 * Calculates Sea-Level Pressure (SLP) using barometric hypsometric formula.
 * @param station_p_hpa Absolute measured pressure at station
 * @param altitude_m Station elevation above sea level in meters
 * @param temp_c Ambient air temperature in Celsius
 */
export function calculate_sea_level_pressure(
  station_p_hpa: number,
  altitude_m: number,
  temp_c: number
): number {
  const temp_k = temp_c + 273.15;
  // Hypsometric equation: P0 = P * (1 - (0.0065 * h) / (T + 0.0065 * h + 273.15))^(-5.257)
  const lapse = 0.0065;
  return station_p_hpa * Math.pow(1.0 - (lapse * altitude_m) / (temp_k + lapse * altitude_m), -5.257);
}

/**
 * Evaluates 3-hour pressure rate of change.
 */
export function evaluate_tendency(p_current: number, p_3h_ago: number): { tendency: BarometricTendency; dp_dt: number } {
  const dp_dt = p_current - p_3h_ago;
  let tendency = BarometricTendency.STEADY;

  if (dp_dt > 2.0) {
    tendency = BarometricTendency.RISING_RAPIDLY;
  } else if (dp_dt > 0.5) {
    tendency = BarometricTendency.RISING_SLOWLY;
  } else if (dp_dt < -2.0) {
    tendency = BarometricTendency.FALLING_RAPIDLY;
  } else if (dp_dt < -0.5) {
    tendency = BarometricTendency.FALLING_SLOWLY;
  } else {
    tendency = BarometricTendency.STEADY;
  }

  return { tendency, dp_dt };
}

/**
 * Computes Zambretti empirical index number (Z).
 * Based on Negretti & Zambretti empirical polynomials.
 */
export function compute_zambretti(
  sea_level_pressure_hpa: number,
  p_3h_ago_hpa: number,
  wind_direction_deg: number = 0,
  season: Season = Season.SUMMER
): ZambrettiForecast {
  const { tendency, dp_dt } = evaluate_tendency(sea_level_pressure_hpa, p_3h_ago_hpa);
  const p = Math.max(947.0, Math.min(1050.0, sea_level_pressure_hpa));

  let z = 12;

  // Zambretti case evaluation: Rising (A), Falling (B), Steady (C)
  if (tendency === BarometricTendency.RISING_RAPIDLY || tendency === BarometricTendency.RISING_SLOWLY) {
    // Falling or Rising pressure formulae
    // Z = 0.179 * (1050 - P)
    z = Math.round(0.179 * (1050.0 - p));
    if (season === Season.WINTER) z -= 1; // Winter pressure adjustment
  } else if (tendency === BarometricTendency.FALLING_RAPIDLY || tendency === BarometricTendency.FALLING_SLOWLY) {
    // Z = 0.244 * (1050 - P)
    z = Math.round(0.244 * (1050.0 - p));
    if (season === Season.SUMMER) z += 1; // Summer convective rain adjustment
  } else {
    // Steady barometric tendency
    // Z = 0.147 * (1050 - P)
    z = Math.round(0.147 * (1050.0 - p));
  }

  // Clamping to [1, 26]
  z = Math.max(1, Math.min(26, z));

  // Determine confidence score based on magnitude of 3h barometric stability
  const confidence = Math.min(100, Math.round(60 + Math.abs(dp_dt) * 15));

  return {
    zambretti_code: `Z${z.toString().padStart(2, '0')}`,
    zambretti_number: z,
    forecast_text: ZAMBRETTI_LOOKUP[z] || 'Unsettled Conditions',
    barometric_trend: tendency,
    dp_dt_3h: parseFloat(dp_dt.toFixed(2)),
    confidence_score: confidence,
  };
}
