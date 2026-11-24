<?php

declare(strict_types=1);

require __DIR__ . '/wifi-common.php';

function wifiDeviceResult(mixed $body): array
{
    if (!$body instanceof stdClass || !wifiId($body->id ?? null)
        || !in_array($body->status ?? null, ['scanned', 'saved', 'failed'], true)) {
        wifiBadRequest();
    }
    $result = ['id' => $body->id, 'status' => $body->status];
    if ($body->status !== 'scanned') {
        if (count((array) $body) !== 2) {
            wifiBadRequest();
        }
        return $result;
    }
    $networks = $body->networks ?? null;
    if (count((array) $body) !== 3 || !is_array($networks) || count($networks) > 12) {
        wifiBadRequest();
    }
    $result['networks'] = [];
    foreach ($networks as $network) {
        if (!$network instanceof stdClass || count((array) $network) !== 3
            || !is_string($network->ssid ?? null) || $network->ssid === '' || strlen($network->ssid) > 32
            || str_contains($network->ssid, "\0") || !is_int($network->rssi ?? null)
            || !is_bool($network->secured ?? null)) {
            wifiBadRequest();
        }
        $result['networks'][] = ['ssid' => $network->ssid, 'rssi' => $network->rssi, 'secured' => $network->secured];
    }
    return $result;
}

requireMethod('POST');
try {
    $deviceId = wifiAuthenticate('device_token_sha256');
    $request = wifiJsonBody();
    $hasResult = property_exists($request, 'result');
    if (($request->device_id ?? null) !== $deviceId || !wifiId($request->boot_id ?? null)
        || !is_bool($request->accept_command ?? null) || (!$request->accept_command && $hasResult)
        || count((array) $request) !== ($hasResult ? 4 : 3)) {
        wifiBadRequest();
    }
    $result = $hasResult ? wifiDeviceResult($request->result) : null;
    $response = ['ok' => true, 'command' => null, 'result_id' => null];
    if (!$request->accept_command) {
        // Candidate-network probes do not read or update the mailbox.
        reply(200, $response);
    }

    $bootId = $request->boot_id;
    $response = wifiMailbox(static function (array &$state, int $now) use ($bootId, $result, $response): array {
        if ($bootId !== $state['boot_id']) {
            if ($state['job'] !== null) {
                $state['job'] = wifiExpireJob($state['job']);
            }
            foreach ($state['recent'] as $id => $job) {
                $state['recent'][$id] = wifiExpireJob($job);
            }
            $state['boot_id'] = $bootId;
        }
        $state['last_seen_ms'] = $now;

        $job = $state['job'];
        if ($result !== null) {
            $id = $result['id'];
            $receipt = ($job['id'] ?? null) === $id ? $job : ($state['recent'][$id] ?? null);
            if (($receipt['boot_id'] ?? null) === $bootId && $receipt['status'] === $result['status']) {
                $response['result_id'] = $id;
            } elseif (($job['id'] ?? null) === $id && $job['status'] === 'pending'
                && $job['boot_id'] === $bootId && ($result['status'] === 'failed'
                    || $result['status'] === ($job['kind'] === 'scan' ? 'scanned' : 'saved'))) {
                unset($job['encrypted']);
                $job['status'] = $result['status'];
                if ($result['status'] === 'scanned') {
                    $job['networks'] = $result['networks'];
                }
                $state['job'] = $job;
                $response['result_id'] = $id;
            }
        }
        if (($job['status'] ?? null) === 'pending' && $job['boot_id'] === $bootId) {
            $response['command'] = [
                'id' => $job['id'],
                'kind' => $job['kind'],
                'expires_in_ms' => $job['expires_at_ms'] - $now,
            ];
            if ($job['kind'] === 'connect') {
                $response['command']['encrypted'] = $job['encrypted'];
            }
        }
        return $response;
    });
    reply(200, $response);
} catch (Throwable) {
    wifiUnavailable();
}
