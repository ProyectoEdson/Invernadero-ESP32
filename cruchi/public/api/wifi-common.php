<?php

declare(strict_types=1);

require __DIR__ . '/common.php';

const WIFI_HEARTBEAT_MS = 15000;
const WIFI_JOB_TTL_MS = 90000;
const WIFI_MAX_RECENT_JOBS = 32;

function wifiAuthenticate(string $tokenKey): string
{
    $authorization = $_SERVER['HTTP_AUTHORIZATION'] ?? $_SERVER['REDIRECT_HTTP_AUTHORIZATION'] ?? '';
    if (!preg_match('/\ABearer ([0-9a-f]{64})\z/', $authorization, $bearer)) {
        reply(401, ['ok' => false, 'error' => 'No autorizado.']);
    }

    $settings = config();
    $deviceHash = $settings['device_token_sha256'] ?? null;
    $adminHash = $settings['wifi_admin_token_sha256'] ?? null;
    $deviceId = $settings['device_id'] ?? null;
    if (!is_string($deviceHash) || !preg_match('/\A[0-9a-f]{64}\z/', $deviceHash)
        || !is_string($adminHash) || !preg_match('/\A[0-9a-f]{64}\z/', $adminHash)
        || hash_equals($deviceHash, $adminHash)
        || !is_string($deviceId) || !preg_match('/\Ainv-[0-9a-f]{12}\z/', $deviceId)) {
        wifiUnavailable();
    }
    if (!hash_equals($settings[$tokenKey], hash('sha256', $bearer[1]))) {
        reply(401, ['ok' => false, 'error' => 'No autorizado.']);
    }
    return $deviceId;
}

function wifiUnavailable(): void
{
    reply(503, ['ok' => false, 'error' => 'Servicio de Wi-Fi no disponible.']);
}

function wifiBadRequest(): void
{
    reply(400, ['ok' => false, 'error' => 'Solicitud de Wi-Fi inválida.']);
}

function wifiJsonBody(): stdClass
{
    if (!preg_match('/\Aapplication\/json(?:\s*;|\z)/i', $_SERVER['CONTENT_TYPE'] ?? '')) {
        reply(415, ['ok' => false, 'error' => 'Se requiere application/json.']);
    }
    $raw = file_get_contents('php://input', false, null, 0, 4097);
    if ($raw !== false && strlen($raw) > 4096) {
        reply(413, ['ok' => false, 'error' => 'Solicitud de Wi-Fi demasiado grande.']);
    }
    $body = $raw === false ? null : json_decode($raw, false, 8);
    if (!$body instanceof stdClass) {
        wifiBadRequest();
    }
    return $body;
}

function wifiId(mixed $value): bool
{
    return is_string($value) && preg_match('/\A[0-9a-f]{32}\z/', $value) === 1;
}

function wifiIsOnline(array $state, int $now): bool
{
    return $state['last_seen_ms'] !== null && $now >= $state['last_seen_ms']
        && $now - $state['last_seen_ms'] < WIFI_HEARTBEAT_MS;
}

function wifiExpireJob(array $job): array
{
    unset($job['encrypted'], $job['networks']);
    $job['status'] = 'expired';
    return $job;
}

function wifiJobView(?array $job): ?array
{
    if ($job === null) {
        return null;
    }
    $view = ['id' => $job['id'], 'kind' => $job['kind'], 'status' => $job['status']];
    if ($job['status'] === 'scanned') {
        $view['networks'] = $job['networks'];
    }
    return $view;
}

function wifiMailboxDirectory(): string
{
    $root = realpath($_SERVER['DOCUMENT_ROOT'] ?? '');
    if ($root === false || dirname($root) === $root) {
        throw new RuntimeException('Missing document root.');
    }
    $directory = dirname($root) . '/.invernadero-wifi-' . substr(hash('sha256', $root), 0, 16);
    if (!is_dir($directory)) {
        @mkdir($directory, 0700);
    }
    clearstatcache(true, $directory);
    $mode = @fileperms($directory);
    if (realpath($directory) !== $directory || !is_dir($directory) || $mode === false || ($mode & 0777) !== 0700) {
        throw new RuntimeException('Unsafe Wi-Fi mailbox directory.');
    }
    return $directory;
}

function wifiReadMailbox(string $path): array
{
    if (is_link($path)) {
        throw new RuntimeException('Unsafe Wi-Fi mailbox state.');
    }
    if (!file_exists($path)) {
        return ['boot_id' => null, 'last_seen_ms' => null, 'job' => null, 'recent' => []];
    }
    $mode = @fileperms($path);
    if (!is_file($path) || $mode === false || ($mode & 0777) !== 0600) {
        throw new RuntimeException('Unsafe Wi-Fi mailbox state.');
    }
    $raw = @file_get_contents($path, false, null, 0, 131073);
    if ($raw === false || strlen($raw) > 131072) {
        throw new RuntimeException('Unreadable Wi-Fi mailbox.');
    }
    return json_decode($raw, true, 16, JSON_THROW_ON_ERROR);
}

function wifiWriteMailbox(string $path, string $next, array $state): void
{
    $body = json_encode($state, JSON_THROW_ON_ERROR);
    $file = @fopen($next, 'xb');
    if ($file === false) {
        throw new RuntimeException('Unwritable Wi-Fi mailbox.');
    }
    try {
        // Put restrictive permissions in place before writing any data.
        $written = @chmod($next, 0600) && @fwrite($file, $body) === strlen($body) && @fflush($file);
    } finally {
        $closed = @fclose($file);
    }
    if (!$written || !$closed || !@rename($next, $path)) {
        throw new RuntimeException('Cannot replace Wi-Fi mailbox.');
    }
}

function wifiMailbox(callable $action): array
{
    $directory = wifiMailboxDirectory();
    $lockPath = $directory . '/lock';
    if (is_link($lockPath) || (file_exists($lockPath) && !is_file($lockPath))) {
        throw new RuntimeException('Unsafe Wi-Fi mailbox lock.');
    }
    $lock = @fopen($lockPath, 'c+b');
    if ($lock === false) {
        throw new RuntimeException('Unavailable Wi-Fi mailbox lock.');
    }
    try {
        if (!@chmod($lockPath, 0600) || !@flock($lock, LOCK_EX)) {
            throw new RuntimeException('Unavailable Wi-Fi mailbox lock.');
        }
        $path = $directory . '/state.json';
        $next = $directory . '/state.next';
        // A terminated writer may leave an unpublished replacement behind.
        if ((file_exists($next) || is_link($next)) && !@unlink($next)) {
            throw new RuntimeException('Cannot clean Wi-Fi mailbox.');
        }
        $state = $before = wifiReadMailbox($path);
        $now = (int) (microtime(true) * 1000);
        $clockWentBack = $state['last_seen_ms'] !== null && $now < $state['last_seen_ms'];
        if ($state['job'] !== null && ($clockWentBack || $now >= $state['job']['expires_at_ms'])) {
            $state['job'] = wifiExpireJob($state['job']);
        }
        foreach ($state['recent'] as $id => $job) {
            if ($clockWentBack || $now >= $job['expires_at_ms']) {
                unset($state['recent'][$id]);
            }
        }
        if ($clockWentBack) {
            $state['last_seen_ms'] = null;
        }

        $response = $action($state, $now);
        if ($state !== $before) {
            try {
                wifiWriteMailbox($path, $next, $state);
            } finally {
                if (file_exists($next)) {
                    @unlink($next);
                }
            }
        }
        return $response;
    } finally {
        @fclose($lock);
    }
}
