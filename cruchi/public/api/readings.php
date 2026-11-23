<?php

declare(strict_types=1);

require __DIR__ . '/common.php';
requireMethod('GET');

const MAX_READINGS = 1200;
$after = $_GET['after'] ?? null;
if ($after !== null && (!is_string($after) || !preg_match('/\A(?:0|[1-9][0-9]{0,19})\z/', $after)
    || (strlen($after) === 20 && strcmp($after, '18446744073709551615') > 0))) {
    reply(400, ['ok' => false, 'error' => 'Cursor inválido.']);
}

try {
    $db = database(config());
    $columns = 'id, received_at, sample_age_ms, temperature_c, humidity_pct, sensor_status';
    if ($after === null) {
        $query = $db->query("SELECT $columns FROM readings ORDER BY id DESC LIMIT " . MAX_READINGS);
    } else {
        $query = $db->prepare("SELECT $columns FROM readings WHERE id > CAST(? AS UNSIGNED) ORDER BY id LIMIT " . MAX_READINGS);
        $query->execute([$after]);
    }
    $rows = $query->fetchAll();
    reply(200, ['ok' => true, 'readings' => array_map('publicReading', $after === null ? array_reverse($rows) : $rows)]);
} catch (Throwable $error) {
    dataUnavailable($error);
}
