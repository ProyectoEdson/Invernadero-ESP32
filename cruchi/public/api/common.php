<?php

declare(strict_types=1);

function reply(int $status, array $body): void
{
    http_response_code($status);
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo json_encode($body, JSON_THROW_ON_ERROR);
    exit;
}

function requireMethod(string $method): void
{
    if (($_SERVER['REQUEST_METHOD'] ?? '') !== $method) {
        header('Allow: ' . $method);
        reply(405, ['ok' => false, 'error' => 'Método no permitido.']);
    }
}

function config(): array
{
    $path = dirname($_SERVER['DOCUMENT_ROOT']) . '/.invernadero-cruchi.php';
    if (!is_readable($path)) {
        reply(503, ['ok' => false, 'error' => 'Servicio de datos sin configurar.']);
    }
    return require $path;
}

function database(array $config): PDO
{
    return new PDO($config['mysql_dsn'], $config['mysql_user'], $config['mysql_password'], [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_EMULATE_PREPARES => false,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    ]);
}

function publicReading(array $row): array
{
    return [
        'sample_id' => (string) $row['id'],
        'received_at' => str_replace(' ', 'T', $row['received_at']) . 'Z',
        'sample_age_ms' => (int) $row['sample_age_ms'],
        'temperature_c' => $row['temperature_c'] === null ? null : (float) $row['temperature_c'],
        'humidity_pct' => $row['humidity_pct'] === null ? null : (float) $row['humidity_pct'],
        'sensor_status' => $row['sensor_status'],
    ];
}

function dataUnavailable(Throwable $error): void
{
    error_log('Invernadero: ' . get_class($error) . ' (' . $error->getCode() . ')');
    reply(503, ['ok' => false, 'error' => 'Servicio de datos no disponible.']);
}
