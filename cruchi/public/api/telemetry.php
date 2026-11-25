<?php

declare(strict_types=1);

require __DIR__ . '/common.php';

const MAX_BODY_BYTES = 4096;

function inRange(mixed $value, float $maximum): bool
{
    return (is_int($value) || is_float($value)) && is_finite($value)
        && $value >= 0 && $value <= $maximum;
}

function validSample(mixed $sample, string $deviceId): bool
{
    if (!$sample instanceof stdClass || ($sample->type ?? null) !== 'sample'
        || ($sample->device_id ?? null) !== $deviceId
        || !is_string($sample->boot_id ?? null) || !preg_match('/\A[0-9a-f]{32}\z/', $sample->boot_id)
        || !is_int($sample->sequence ?? null) || $sample->sequence < 1 || $sample->sequence > 4294967295
        || !is_int($sample->uptime_ms ?? null) || $sample->uptime_ms < 0
        || !is_int($sample->sent_uptime_ms ?? null) || $sample->sent_uptime_ms > 9007199254740991
        || $sample->sent_uptime_ms < $sample->uptime_ms || $sample->sent_uptime_ms - $sample->uptime_ms > 15000
        || !is_bool($sample->wifi_connected ?? null) || !is_bool($sample->memory_config_ok ?? null)
        || ($sample->peltier_locked ?? null) !== true || ($sample->outputs_safe ?? null) !== true
        || !($sample->gpio_levels ?? null) instanceof stdClass || ($sample->gpio_levels->peltier ?? null) !== 0
        || !in_array($sample->gpio_levels->pump ?? null, [0, 1], true)
        || !property_exists($sample, 'temperature_c') || !property_exists($sample, 'humidity_pct')) {
        return false;
    }
    if (($sample->operating_mode ?? null) === 'monitoring') {
        if (($sample->phase4_control_ready ?? null) !== false
            || $sample->gpio_levels->pump !== 0 || ($sample->gpio_levels->leds ?? null) !== 0
            || count(get_object_vars($sample->gpio_levels)) !== 3
            || !property_exists($sample, 'illuminance_lux')
            || ($sample->illuminance_lux !== null && !inRange($sample->illuminance_lux, 54612.5))) {
            return false;
        }
    } elseif (!in_array($sample->operating_mode ?? null, ['manual', 'automatic'], true)
        || ($sample->phase4_control_ready ?? null) !== true
        || !in_array($sample->gpio_levels->fans ?? null, [0, 1], true)
        || property_exists($sample->gpio_levels, 'leds') || property_exists($sample, 'illuminance_lux')) {
        return false;
    }
    // Los acuses de órdenes se ignoran: CruChi solo monitoriza.
    return match ($sample->sensor_status ?? null) {
        'ok' => inRange($sample->temperature_c, 50) && inRange($sample->humidity_pct, 100),
        'read_error', 'out_of_range' => $sample->temperature_c === null && $sample->humidity_pct === null,
        default => false,
    };
}

function storeSample(PDO $db, stdClass $sample): bool
{
    $db->beginTransaction();
    try {
        $latest = $db->query("SELECT r.boot_id, r.sequence_no, r.uptime_ms
            FROM device_state AS s LEFT JOIN readings AS r ON r.id = s.reading_id
            WHERE s.id = 'invernadero' FOR UPDATE")->fetch();
        if ($latest === false) {
            throw new RuntimeException('No existe el cursor inicial.');
        }
        $boot = hex2bin($sample->boot_id);
        if ($latest['boot_id'] === $boot) {
            if ($sample->sequence <= $latest['sequence_no'] || $sample->uptime_ms < $latest['uptime_ms']) {
                $db->rollBack();
                return false;
            }
        } elseif ($latest['boot_id'] !== null) {
            $seen = $db->prepare('SELECT 1 FROM readings WHERE boot_id = ? LIMIT 1');
            $seen->execute([$boot]);
            if ($seen->fetchColumn() !== false) {
                $db->rollBack();
                return false;
            }
        }

        $insert = $db->prepare('INSERT INTO readings (boot_id, sequence_no, uptime_ms, received_at,
            sample_age_ms, temperature_c, humidity_pct, illuminance_lux, sensor_status, wifi_connected, memory_config_ok,
            pump_gpio, fans_gpio, leds_gpio, operating_mode) VALUES (?, ?, ?, UTC_TIMESTAMP(3), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)');
        $insert->execute([
            $boot, $sample->sequence, $sample->uptime_ms, $sample->sent_uptime_ms - $sample->uptime_ms,
            $sample->temperature_c, $sample->humidity_pct, $sample->illuminance_lux ?? null, $sample->sensor_status,
            (int) $sample->wifi_connected, (int) $sample->memory_config_ok,
            $sample->gpio_levels->pump, $sample->gpio_levels->fans ?? null, $sample->gpio_levels->leds ?? null,
            $sample->operating_mode,
        ]);
        $advance = $db->prepare("UPDATE device_state SET reading_id = ? WHERE id = 'invernadero'");
        $advance->execute([$db->lastInsertId()]);
        $db->commit();
        return true;
    } catch (Throwable $error) {
        if ($db->inTransaction()) {
            $db->rollBack();
        }
        throw $error;
    }
}

requireMethod('POST');
$authorization = $_SERVER['HTTP_AUTHORIZATION'] ?? $_SERVER['REDIRECT_HTTP_AUTHORIZATION'] ?? '';
if (!preg_match('/\ABearer ([0-9a-f]{64})\z/', $authorization, $bearer)) {
    reply(401, ['ok' => false, 'error' => 'No autorizado.']);
}
try {
    $config = config();
    $hash = $config['device_token_sha256'] ?? '';
    $deviceId = $config['device_id'] ?? '';
    if (!preg_match('/\A[0-9a-f]{64}\z/', $hash) || !preg_match('/\Ainv-[0-9a-f]{12}\z/', $deviceId)) {
        reply(503, ['ok' => false, 'error' => 'Servicio de telemetría sin configurar.']);
    }
    if (!hash_equals($hash, hash('sha256', $bearer[1]))) {
        reply(401, ['ok' => false, 'error' => 'No autorizado.']);
    }
    if (!preg_match('/\Aapplication\/json(?:\s*;|\z)/i', $_SERVER['CONTENT_TYPE'] ?? '')) {
        reply(415, ['ok' => false, 'error' => 'Se requiere application/json.']);
    }
    $raw = file_get_contents('php://input', false, null, 0, MAX_BODY_BYTES + 1);
    if ($raw !== false && strlen($raw) > MAX_BODY_BYTES) {
        reply(413, ['ok' => false, 'error' => 'Cuerpo de telemetría demasiado grande.']);
    }
    $sample = $raw === false ? null : json_decode($raw, false, 16);
    if (!validSample($sample, $deviceId)) {
        reply(400, ['ok' => false, 'error' => 'Muestra inválida o fuera de rango.']);
    }
    $accepted = storeSample(database($config), $sample);
    reply(200, ['ok' => true, 'accepted' => $accepted, 'command' => null]);
} catch (Throwable $error) {
    dataUnavailable($error);
}
