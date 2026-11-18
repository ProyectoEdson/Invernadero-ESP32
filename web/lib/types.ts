export type SensorStatus = "ok" | "read_error" | "out_of_range";

export type GpioLevels = {
  peltier: 0 | 1;
  pump: 0 | 1;
  fans: 0 | 1;
};

/** Vista pública: no incluye credenciales, MAC, identidad de arranque ni órdenes. */
export type DeviceState = {
  id: "invernadero";
  sample_id: string;
  received_at: string;
  sample_age_ms: number;
  temperature_c: number | null;
  humidity_pct: number | null;
  sensor_status: SensorStatus;
  wifi_connected: boolean;
  memory_config_ok: boolean;
  peltier_locked: boolean;
  gpio_levels: GpioLevels;
  outputs_safe: boolean;
  operating_mode: "monitoring";
};

export const PUBLIC_DEVICE_ID = "invernadero";
export const SAMPLE_INTERVAL_MS = 3_000;
export const STALE_AFTER_MS = 15_000;
export const MAX_SESSION_POINTS = 1_200;
export const COOLING_ON_C = 24;
export const COOLING_OFF_C = 22;

/** Edad estimada: incorpora el tiempo de cola en el ESP32, no solo la recepción. */
export function sampleAgeMs(state: DeviceState, now: number): number {
  return Math.max(0, now - Date.parse(state.received_at)) + state.sample_age_ms;
}
