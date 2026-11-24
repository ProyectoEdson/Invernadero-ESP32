<?php

declare(strict_types=1);

require __DIR__ . '/wifi-common.php';

function wifiAdminJob(stdClass $body): array
{
    $kind = $body->kind ?? null;
    if (!wifiId($body->id ?? null) || !in_array($kind, ['scan', 'connect'], true)) {
        wifiBadRequest();
    }
    $job = ['id' => $body->id, 'kind' => $kind, 'status' => 'pending'];
    if ($kind === 'connect') {
        $encrypted = $body->encrypted ?? null;
        // 12-byte nonce, 16-byte tag, and 2–96 bytes of encrypted payload.
        if (count((array) $body) !== 3 || !is_string($encrypted)
            || !preg_match('/\A[0-9a-f]{56}(?:[0-9a-f]{2}){2,96}\z/', $encrypted)) {
            wifiBadRequest();
        }
        $job['encrypted'] = $encrypted;
    } elseif (count((array) $body) !== 2) {
        wifiBadRequest();
    }
    return $job;
}

$method = $_SERVER['REQUEST_METHOD'] ?? '';
if ($method !== 'GET' && $method !== 'POST') {
    header('Allow: GET, POST');
    reply(405, ['ok' => false, 'error' => 'Método no permitido.']);
}
try {
    wifiAuthenticate('wifi_admin_token_sha256');
    $requested = $method === 'POST' ? wifiAdminJob(wifiJsonBody()) : null;
    [$status, $body] = wifiMailbox(static function (array &$state, int $now) use ($requested): array {
        $online = wifiIsOnline($state, $now);
        $current = $state['job'];
        if ($requested === null) {
            return [200, ['ok' => true, 'online' => $online, 'job' => wifiJobView($current)]];
        }

        $id = $requested['id'];
        $existing = ($current['id'] ?? null) === $id ? $current : ($state['recent'][$id] ?? null);
        if ($existing !== null) {
            // Never create a second job for a retry, including after completion.
            if ($existing['kind'] !== $requested['kind'] || ($existing['status'] === 'pending'
                && ($existing['encrypted'] ?? null) !== ($requested['encrypted'] ?? null))) {
                return [409, ['ok' => false, 'error' => 'La solicitud de Wi-Fi está en conflicto.']];
            }
            return [200, ['ok' => true, 'online' => $online, 'job' => wifiJobView($existing)]];
        }
        if (!$online) {
            return [409, ['ok' => false, 'error' => 'El dispositivo no está disponible.']];
        }
        if (($current['status'] ?? null) === 'pending' || count($state['recent']) >= WIFI_MAX_RECENT_JOBS) {
            return [409, ['ok' => false, 'error' => 'El servicio de Wi-Fi está ocupado.']];
        }
        if ($current !== null && $now < $current['expires_at_ms']) {
            $state['recent'][$current['id']] = $current;
        }
        $state['job'] = $requested + [
            'boot_id' => $state['boot_id'],
            'expires_at_ms' => $now + WIFI_JOB_TTL_MS,
        ];
        return [200, ['ok' => true, 'online' => $online, 'job' => wifiJobView($state['job'])]];
    });
    reply($status, $body);
} catch (Throwable) {
    wifiUnavailable();
}
