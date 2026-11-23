<?php

declare(strict_types=1);

require __DIR__ . '/common.php';
requireMethod('GET');

try {
    $db = database(config());
    $row = $db->query("SELECT r.id, r.received_at, r.sample_age_ms, r.temperature_c, r.humidity_pct,
        r.sensor_status, r.wifi_connected, r.memory_config_ok, r.pump_gpio, r.fans_gpio, r.operating_mode
        FROM device_state AS s JOIN readings AS r ON r.id = s.reading_id WHERE s.id = 'invernadero'")->fetch();
    $state = $row === false ? null : publicReading($row) + [
        'id' => 'invernadero',
        'wifi_connected' => (bool) $row['wifi_connected'],
        'memory_config_ok' => (bool) $row['memory_config_ok'],
        'peltier_locked' => true,
        'outputs_safe' => true,
        'gpio_levels' => ['peltier' => 0, 'pump' => (int) $row['pump_gpio'], 'fans' => (int) $row['fans_gpio']],
        'operating_mode' => $row['operating_mode'],
    ];
    reply(200, ['ok' => true, 'state' => $state]);
} catch (Throwable $error) {
    dataUnavailable($error);
}
