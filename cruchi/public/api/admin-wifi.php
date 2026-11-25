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
    [$status, $body] = wifiAdminMailbox($requested);
    reply($status, $body);
} catch (Throwable) {
    wifiUnavailable();
}
