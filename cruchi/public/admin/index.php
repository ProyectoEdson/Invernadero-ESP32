<?php

declare(strict_types=1);

require __DIR__ . '/../api/wifi-common.php';

const ADMIN_PATH = '/invernadero/admin/';
const ADMIN_SESSION_SECONDS = 3600;

function adminError(int $status, string $message): never
{
    reply($status, ['ok' => false, 'error' => $message]);
    exit;
}

function adminEscape(string $value): string
{
    return htmlspecialchars($value, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function adminLogin(string $url, #[SensitiveParameter] string $anonKey, string $adminId,
    string $email, #[SensitiveParameter] string $password): int
{
    $context = stream_context_create([
        'http' => [
            'method' => 'POST',
            'header' => "Content-Type: application/json\r\nAccept: application/json\r\napikey: " . $anonKey,
            'content' => json_encode(['email' => $email, 'password' => $password], JSON_THROW_ON_ERROR),
            'timeout' => 8,
            'follow_location' => 0,
            'ignore_errors' => true,
        ],
        'ssl' => ['verify_peer' => true, 'verify_peer_name' => true],
    ]);
    $stream = @fopen($url . '/auth/v1/token?grant_type=password', 'rb', false, $context);
    if ($stream === false) {
        throw new RuntimeException();
    }
    try {
        $response = stream_get_meta_data($stream)['wrapper_data'][0] ?? '';
        if (preg_match('/\AHTTP\/\d(?:\.\d)? (?:400|401|403)\b/', $response)) {
            return 0;
        }
        if (!preg_match('/\AHTTP\/\d(?:\.\d)? 200\b/', $response)) {
            throw new RuntimeException();
        }
        $body = stream_get_contents($stream, 65537);
        if ($body === false || strlen($body) > 65536) {
            throw new RuntimeException();
        }
        $result = json_decode($body, true, 32, JSON_THROW_ON_ERROR);
        $id = $result['user']['id'] ?? null;
        $seconds = $result['expires_in'] ?? null;
        if (!is_string($id) || !hash_equals($adminId, strtolower($id)) || !is_int($seconds) || $seconds < 1) {
            return 0;
        }
        return min($seconds, ADMIN_SESSION_SECONDS);
    } finally {
        fclose($stream);
    }
}

function adminConnectJob(string $id, string $ssid, #[SensitiveParameter] string $password,
    #[SensitiveParameter] string $key): array
{
    $nonce = random_bytes(12);
    $ciphertext = openssl_encrypt(chr(strlen($ssid)) . $ssid . $password, 'aes-256-gcm', hex2bin($key),
        OPENSSL_RAW_DATA, $nonce, $tag, $id, 16);
    if ($ciphertext === false) {
        throw new RuntimeException();
    }
    return ['id' => $id, 'kind' => 'connect', 'status' => 'pending', 'encrypted' => bin2hex($nonce . $tag . $ciphertext)];
}

header('Cache-Control: no-store');
header('Referrer-Policy: no-referrer');
header('X-Content-Type-Options: nosniff');
header('X-Robots-Tag: noindex, nofollow');
$nonce = base64_encode(random_bytes(18));
header("Content-Security-Policy: default-src 'none'; script-src 'nonce-$nonce'; style-src 'self' 'nonce-$nonce'; "
    . "font-src 'self'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");

if (!in_array(strtolower((string) ($_SERVER['HTTPS'] ?? '')), ['on', '1'], true)) {
    adminError(403, 'La administración requiere HTTPS.');
}
$method = $_SERVER['REQUEST_METHOD'] ?? '';
if (!in_array($method, ['GET', 'POST'], true)) {
    header('Allow: GET, POST');
    adminError(405, 'Método no permitido.');
}

$private = config();
$url = rtrim(is_string($private['supabase_url'] ?? null) ? $private['supabase_url'] : '', '/');
$anonKey = $private['supabase_anon_key'] ?? null;
$adminId = strtolower(is_string($private['supabase_admin_user_id'] ?? null) ? $private['supabase_admin_user_id'] : '');
if (!preg_match('/\Ahttps:\/\/[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\z/i', $url)
    || !is_string($anonKey) || !preg_match('/\A[A-Za-z0-9_.-]+\z/', $anonKey)
    || !preg_match('/\A[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\z/', $adminId)) {
    adminError(503, 'La administración no está configurada.');
}
$key = $private['wifi_device_key'] ?? null;
$canConnect = is_string($key) && preg_match('/\A[0-9a-f]{64}\z/i', $key) === 1 && function_exists('openssl_encrypt');
unset($private);

session_name('invernadero_admin');
if (!session_start([
    'use_strict_mode' => 1,
    'use_only_cookies' => 1,
    'use_trans_sid' => 0,
    'cookie_lifetime' => 0,
    'cookie_path' => ADMIN_PATH,
    'cookie_domain' => '',
    'cookie_secure' => true,
    'cookie_httponly' => true,
    'cookie_samesite' => 'Strict',
    'cache_limiter' => '',
])) {
    adminError(503, 'No se pudo iniciar la sesión.');
}
$loggedIn = ($_SESSION['admin_id'] ?? null) === $adminId && ($_SESSION['expires_at'] ?? 0) > time();
if (!$loggedIn && isset($_SESSION['admin_id'])) {
    $_SESSION = [];
}
$csrf = $_SESSION['csrf'] ??= bin2hex(random_bytes(32));
$action = $_POST['action'] ?? '';
$api = $method === 'GET' ? isset($_GET['status']) : ($action === 'scan' || $action === 'connect');
$error = '';

if ($method === 'POST') {
    $submitted = $_POST['csrf'] ?? null;
    if (!is_string($submitted) || !hash_equals($csrf, $submitted)) {
        adminError(403, 'La solicitud caducó. Recarga la página.');
    }
    if ($action === 'logout') {
        $_SESSION = [];
        if (!session_destroy()) {
            adminError(503, 'No se pudo cerrar la sesión.');
        }
        setcookie(session_name(), '', [
            'expires' => 1, 'path' => ADMIN_PATH, 'secure' => true, 'httponly' => true, 'samesite' => 'Strict',
        ]);
        header('Location: ' . ADMIN_PATH, true, 303);
        exit;
    }
    if ($action === 'login') {
        if ($loggedIn) {
            header('Location: ' . ADMIN_PATH, true, 303);
            exit;
        }
        $email = $_POST['email'] ?? null;
        $password = $_POST['password'] ?? null;
        unset($_POST['password']);
        $error = 'No se pudo iniciar sesión con esas credenciales.';
        if (is_string($email) && strlen($email) <= 254 && filter_var($email, FILTER_VALIDATE_EMAIL)
            && is_string($password) && $password !== '' && strlen($password) <= 4096) {
            try {
                $seconds = adminLogin($url, $anonKey, $adminId, $email, $password);
                if ($seconds > 0) {
                    if (!session_regenerate_id(true)) {
                        throw new RuntimeException();
                    }
                    $_SESSION = ['admin_id' => $adminId, 'expires_at' => time() + $seconds, 'csrf' => bin2hex(random_bytes(32))];
                    header('Location: ' . ADMIN_PATH, true, 303);
                    exit;
                }
            } catch (Throwable) {
                http_response_code(503);
                $error = 'El servicio de acceso no está disponible. Inténtalo más tarde.';
            }
        }
        unset($password);
    } elseif (!$api) {
        adminError(400, 'Acción no válida.');
    }
}
session_write_close();

if ($api) {
    if (!$loggedIn) {
        adminError(401, 'La sesión caducó. Vuelve a iniciar sesión.');
    }
    try {
        $job = null;
        if ($action === 'scan') {
            $job = ['id' => bin2hex(random_bytes(16)), 'kind' => 'scan', 'status' => 'pending'];
        } elseif ($action === 'connect') {
            if (!$canConnect) {
                adminError(503, 'El envío de redes no está configurado.');
            }
            $ssid = $_POST['ssid'] ?? null;
            $password = $_POST['psk'] ?? null;
            unset($_POST['psk']);
            if (!is_string($ssid) || strlen($ssid) < 1 || strlen($ssid) > 32 || str_contains($ssid, "\0")
                || preg_match('//u', $ssid) !== 1 || !is_string($password) || str_contains($password, "\0")
                || preg_match('//u', $password) !== 1 || ($password !== '' && (strlen($password) < 8 || strlen($password) > 63))) {
                wifiBadRequest();
            }
            $job = adminConnectJob(bin2hex(random_bytes(16)), $ssid, $password, $key);
            unset($password);
        }
        [$status, $body] = wifiAdminMailbox($job);
        if ($status === 200) {
            $body['can_connect'] = $canConnect;
        }
        reply($status, $body);
    } catch (Throwable) {
        wifiUnavailable();
    }
}
?>
<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Administración Wi-Fi | Invernadero ESP32</title>
  <link rel="stylesheet" href="../dashboard.css">
  <style nonce="<?= adminEscape($nonce) ?>">
    .admin-panel { max-width: 34rem; margin: 1.5rem auto 4rem; border-top: 3px solid var(--leaf); padding-top: 1.5rem; }
    .admin-panel h1 { color: var(--forest); font-size: clamp(1.6rem, 5vw, 2.4rem); line-height: 1.15; margin: 0 0 .75rem; }
    .admin-panel p { margin: 0 0 1rem; }
    .admin-panel form, .admin-panel label { display: grid; gap: .35rem; }
    .admin-panel form { gap: 1rem; margin-top: 1.25rem; }
    .admin-panel input, .admin-panel select, .admin-button { min-height: 44px; padding: .55rem .7rem; font: inherit; border: 1px solid var(--muted); border-radius: .35rem; }
    .admin-panel input, .admin-panel select { min-width: 0; width: 100%; background: white; color: var(--ink); }
    .admin-button { width: fit-content; max-width: 100%; background: var(--forest); color: white; border-color: var(--forest); cursor: pointer; }
    .admin-button:disabled { opacity: .6; cursor: not-allowed; }
    .admin-button:focus-visible, .admin-panel input:focus-visible, .admin-panel select:focus-visible { outline: 2px solid var(--forest); outline-offset: 3px; }
    .device-status { color: var(--forest); font-weight: 750; }
    .job-status { background: white; border-left: 3px solid var(--leaf); padding: .75rem; min-height: 3rem; }
    .admin-panel .admin-error { color: var(--amber); font-weight: 750; overflow-wrap: anywhere; }
    @media (max-width: 420px) { .admin-header { flex-wrap: wrap; gap: .5rem 1rem; padding-block: .65rem; } }
  </style>
</head>
<body>
  <div class="site-shell">
    <header class="site-header admin-header">
      <a class="identity" href="/invernadero/">Invernadero ESP32</a>
      <?php if ($loggedIn): ?>
      <form method="post" action="<?= ADMIN_PATH ?>">
        <input type="hidden" name="action" value="logout">
        <input type="hidden" name="csrf" value="<?= adminEscape($csrf) ?>">
        <button class="admin-button" type="submit">Cerrar sesión</button>
      </form>
      <?php endif; ?>
    </header>
    <main class="admin-panel">
      <h1>Administración Wi-Fi</h1>
      <?php if (!$loggedIn): ?>
      <p>Inicia sesión con la cuenta administradora del invernadero.</p>
      <?php if ($error !== ''): ?><p class="admin-error" role="alert"><?= adminEscape($error) ?></p><?php endif; ?>
      <form method="post" action="<?= ADMIN_PATH ?>">
        <input type="hidden" name="action" value="login">
        <input type="hidden" name="csrf" value="<?= adminEscape($csrf) ?>">
        <label>Correo electrónico <input type="email" name="email" autocomplete="username" maxlength="254" required></label>
        <label>Contraseña <input type="password" name="password" autocomplete="current-password" required></label>
        <button class="admin-button" type="submit">Iniciar sesión</button>
      </form>
      <?php else: ?>
      <p class="device-status" id="device-status" role="status">Consultando el ESP32…</p>
      <p>Busca las redes que el ESP32 detecta cerca del invernadero. Guardar una red no comprueba que el dispositivo pueda conectarse a ella.</p>
      <p class="job-status" id="job-status" role="status">Consultando el estado de Wi-Fi…</p>
      <p class="admin-error" id="action-error" role="alert" hidden></p>
      <p id="connect-unavailable"<?= $canConnect ? ' hidden' : '' ?>>El envío de redes no está configurado. Aún puedes buscar redes y consultar el estado.</p>
      <button class="admin-button" id="scan" type="button" disabled>Buscar redes</button>
      <form id="connect-form" method="post" action="<?= ADMIN_PATH ?>">
        <label>Red Wi-Fi <select id="networks" required disabled><option value="">Busca redes primero</option></select></label>
        <label id="psk-label" hidden>Contraseña de la red <input id="psk" type="password" autocomplete="off" disabled></label>
        <button class="admin-button" id="connect" type="submit" disabled>Guardar red en el ESP32</button>
      </form>
      <noscript><p>Activa JavaScript para buscar redes y consultar el estado.</p></noscript>
      <?php endif; ?>
    </main>
  </div>
  <?php if ($loggedIn): ?>
  <script nonce="<?= adminEscape($nonce) ?>">
  (() => {
    const csrf = document.querySelector('input[name="csrf"]').value;
    const device = document.getElementById('device-status');
    const status = document.getElementById('job-status');
    const error = document.getElementById('action-error');
    const unavailable = document.getElementById('connect-unavailable');
    const scan = document.getElementById('scan');
    const networks = document.getElementById('networks');
    const psk = document.getElementById('psk');
    const pskLabel = document.getElementById('psk-label');
    const connect = document.getElementById('connect');
    let online = false;
    let pending = false;
    let busy = false;
    let poll = null;
    let canConnect = false;
    let expired = false;
    let scanId = '';

    function announce(node, text) {
      if (node.textContent !== text) node.textContent = text;
    }

    function updateControls() {
      const selected = networks.selectedOptions[0];
      const secured = selected?.dataset.secured === 'true';
      scan.disabled = !online || pending || busy;
      networks.disabled = networks.options.length < 2 || pending || busy;
      pskLabel.hidden = !canConnect || !secured;
      psk.disabled = networks.disabled || pskLabel.hidden;
      psk.required = secured;
      connect.disabled = scan.disabled || !canConnect || !selected?.value;
    }

    function showState(data) {
      online = data.online;
      canConnect = data.can_connect;
      unavailable.hidden = canConnect;
      announce(device, online ? 'ESP32 disponible' : 'ESP32 sin conexión reciente');
      const job = data.job;
      pending = job?.status === 'pending';
      if (job?.kind === 'scan' && job.id !== scanId) {
        networks.replaceChildren(new Option('Busca redes primero', ''));
        psk.value = '';
        if (job.status === 'scanned') {
          networks.options[0].textContent = job.networks.length ? 'Elige una red' : 'No se detectaron redes';
          for (const network of job.networks) {
            const option = new Option(`${network.ssid} · ${network.rssi} dBm · ${network.secured ? 'Protegida' : 'Abierta'}`, network.ssid);
            option.dataset.secured = String(network.secured);
            networks.add(option);
          }
        }
      }
      if (job?.kind === 'scan' && job.status === 'scanned') scanId = job.id;
      const reading = job?.kind === 'scan';
      const statusText = {
        pending: reading ? 'El ESP32 está buscando redes…' : 'Esperando que el ESP32 guarde la red…',
        scanned: job?.networks?.length ? (canConnect ? 'Búsqueda completada. Elige una red detectada.' : 'Búsqueda completada. Puedes ver las redes detectadas.') : 'No se detectaron redes. Puedes buscar de nuevo.',
        saved: 'El ESP32 guardó la red como respaldo. No se ha comprobado la conexión.',
        failed: reading ? 'La búsqueda falló. Puedes volver a buscar.' : 'El ESP32 no pudo guardar la red.',
        expired: 'La solicitud caducó sin confirmación del ESP32. Puedes volver a intentarlo.',
      };
      announce(status, job ? statusText[job.status] : 'No hay solicitudes de Wi-Fi activas.');
      updateControls();
    }

    async function request(params, signal) {
      const response = await fetch(params ? './' : './?status', {
        method: params ? 'POST' : 'GET',
        body: params,
        cache: 'no-store',
        signal,
      });
      const data = await response.json();
      if (response.status === 401 || response.status === 403) expired = true;
      if (!response.ok || !data.ok) throw new Error(data.error || 'No se pudo consultar el servicio de Wi-Fi.');
      return data;
    }

    function report(message) {
      announce(error, message);
      error.hidden = !message;
    }

    async function refresh() {
      if (busy || expired || poll) return;
      const controller = new AbortController();
      poll = controller;
      try {
        const result = await request(null, controller.signal);
        if (!controller.signal.aborted) showState(result);
      } catch (failure) {
        if (controller.signal.aborted) return;
        online = false;
        announce(device, 'No se pudo consultar el ESP32');
        announce(status, expired ? failure.message : 'Volveremos a consultar el estado en unos segundos.');
        if (expired) psk.value = '';
        updateControls();
      } finally {
        poll = null;
      }
    }

    async function submit(params) {
      if (busy || !online || pending || expired) return;
      poll?.abort();
      busy = true;
      updateControls();
      report('');
      params.set('csrf', csrf);
      try {
        showState(await request(params));
      } catch (failure) {
        if (expired) online = false;
        report(failure.message);
      } finally {
        busy = false;
        updateControls();
      }
    }

    scan.addEventListener('click', () => submit(new URLSearchParams({ action: 'scan' })));
    networks.addEventListener('change', () => { psk.value = ''; updateControls(); });
    document.getElementById('connect-form').addEventListener('submit', (event) => {
      event.preventDefault();
      if (connect.disabled || busy) return;
      const size = new TextEncoder().encode(psk.value).length;
      if (!psk.disabled && (size < 8 || size > 63 || psk.value.includes('\0'))) {
        report('La contraseña de la red debe ocupar entre 8 y 63 bytes.');
        psk.focus();
        return;
      }
      const params = new URLSearchParams({ action: 'connect', ssid: networks.value, psk: psk.disabled ? '' : psk.value });
      psk.value = '';
      submit(params);
    });
    refresh();
    setInterval(refresh, 3000);
  })();
  </script>
  <?php endif; ?>
</body>
</html>
