import type { GpioLevels, SensorStatus } from "./types";

export type Telemetry = {
  type: "sample";
  device_id: string;
  boot_id: string;
  sequence: number;
  uptime_ms: number;
  sent_uptime_ms: number;
  sensor_status: SensorStatus;
  temperature_c: number | null;
  humidity_pct: number | null;
  wifi_connected: boolean;
  memory_config_ok: boolean;
  peltier_locked: true;
  gpio_levels: GpioLevels;
  outputs_safe: true;
};

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function integer(value: unknown, minimum: number, maximum = Number.MAX_SAFE_INTEGER): value is number {
  return typeof value === "number" && Number.isSafeInteger(value) && value >= minimum && value <= maximum;
}

function finiteIn(value: unknown, minimum: number, maximum: number): value is number {
  return typeof value === "number" && Number.isFinite(value) && value >= minimum && value <= maximum;
}

/** Fase 3 es solo supervisión; ninguna muestra puede declarar una salida activa. */
export function validateTelemetry(value: unknown, expectedDevice: string): Telemetry | null {
  if (!record(value) || value.type !== "sample" || value.device_id !== expectedDevice ||
      typeof value.device_id !== "string" || !/^inv-[0-9a-f]{12}$/.test(value.device_id) ||
      typeof value.boot_id !== "string" || !/^[0-9a-f]{32}$/.test(value.boot_id) ||
      !integer(value.sequence, 1, 0xffffffff) || !integer(value.uptime_ms, 0) ||
      !integer(value.sent_uptime_ms, value.uptime_ms) ||
      value.sent_uptime_ms - value.uptime_ms > 15_000 ||
      typeof value.wifi_connected !== "boolean" || typeof value.memory_config_ok !== "boolean" ||
      value.peltier_locked !== true || value.outputs_safe !== true || !record(value.gpio_levels) ||
      value.gpio_levels.peltier !== 0 || value.gpio_levels.pump !== 0 || value.gpio_levels.fans !== 0) {
    return null;
  }
  if (value.sensor_status === "ok") {
    if (!finiteIn(value.temperature_c, 0, 50) || !finiteIn(value.humidity_pct, 0, 100)) return null;
  } else if (value.sensor_status === "read_error" || value.sensor_status === "out_of_range") {
    if (value.temperature_c !== null || value.humidity_pct !== null) return null;
  } else {
    return null;
  }
  // Crear un objeto permitido evita pasar campos extra al RPC privilegiado.
  return {
    type: "sample",
    device_id: value.device_id,
    boot_id: value.boot_id,
    sequence: value.sequence,
    uptime_ms: value.uptime_ms,
    sent_uptime_ms: value.sent_uptime_ms,
    sensor_status: value.sensor_status,
    temperature_c: value.temperature_c as number | null,
    humidity_pct: value.humidity_pct as number | null,
    wifi_connected: value.wifi_connected,
    memory_config_ok: value.memory_config_ok,
    peltier_locked: true,
    gpio_levels: { peltier: 0, pump: 0, fans: 0 },
    outputs_safe: true,
  };
}
