<?php
/**
 * MiiCam Web API - pure PHP router, no framework/vendor.
 *
 * Served by lighttpd + PHP-CGI (arm-php-cgi). All requests to /api/*
 * are rewritten to this file by lighttpd.conf. Every response is JSON
 * except a few binary/streaming endpoints (snapshot, stream).
 */

declare(strict_types=1);

error_reporting(E_ALL & ~E_DEPRECATED & ~E_STRICT);

define('BIN', '/tmp/sd/firmware/bin');
define('LIBDIR', '/tmp/sd/firmware/lib');
define('CFG', '/tmp/sd/config.cfg');
define('IMAGES', '/tmp/sd/RECORDED_IMAGES');
define('VIDEOS',  '/tmp/sd/RECORDED_VIDEOS');

define('CTRL_SNAPSHOT', '/dev/shm/rtspd_snapshot');
define('LAST_SNAPSHOT', '/dev/shm/rtspd_last_snapshot_path');
/* Coalescing marker: rtspd's take_snapshot() runs synchronously inside its
 * media thread and takes ~1-2s each, so queued triggers starve the RTSP
 * pipeline and trip the hardware watchdog (camera reboot + new rtspd PID).
 * All /api/snapshot callers share this stamp so concurrent requests collapse
 * onto one trigger per SNAP_MIN_INTERVAL seconds instead of piling up. */
define('SNAP_LOCK', '/dev/shm/rtspd_web_snapshot_lock');
define('SNAP_MIN_INTERVAL', 3);

$old_ld = getenv('LD_LIBRARY_PATH');
putenv('LD_LIBRARY_PATH=' . LIBDIR . ($old_ld ? ':' . $old_ld : ''));
define('CTRL_VIDEO',    '/dev/shm/rtspd_video');
define('LAST_VIDEO',    '/dev/shm/rtspd_last_video_path');

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

function json($data, int $code = 200) {
    http_response_code($code);
    header('Content-Type: application/json');
    echo json_encode($data);
    exit;
}

function fail($msg, int $code = 400) {
    json(['status' => 'error', 'message' => $msg], $code);
}

function ok($data = []) {
    json((array)$data + ['status' => 'ok']);
}

function req($key, $default = null) {
    return isset($_REQUEST[$key]) ? $_REQUEST[$key] : $default;
}

function sget($key, $default = null) {
    return isset($_GET[$key]) ? $_GET[$key] : $default;
}

function run_cmd(array $argv, ?int &$code = null): array {
    $cmd = array_map('escapeshellarg', $argv);
    $cmdstr = implode(' ', $cmd) . ' 2>&1';
    $out = [];
    $rc = 0;
    exec($cmdstr, $out, $rc);
    $code = $rc;
    return $out;
}

function need_binary(string $name): string {
    $path = BIN . '/' . $name;
    if (!is_executable($path)) {
        fail('Binary not available: ' . $name, 500);
    }
    return $path;
}

/** Parse "key=value" lines (shell-style config.cfg or command output). */
function parse_kv(array $lines): array {
    $res = [];
    foreach ($lines as $line) {
        if (preg_match('/^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"?([^"\n]*)"?\s*$/', trim($line), $m)) {
            $res[$m[1]] = $m[2];
        }
    }
    return $res;
}

function parse_bool($v): int {
    return ((string)$v === '1' || strtolower((string)$v) === 'on') ? 1 : 0;
}

/** Read latest snapshot file path, falling back to a directory scan. */
function last_snapshot_path(): ?string {
    if (is_file(LAST_SNAPSHOT)) {
        $p = trim((string)file_get_contents(LAST_SNAPSHOT));
        if ($p !== '' && $p !== 'unknown' && is_file($p)) {
            return $p;
        }
    }
    return newest_file(IMAGES);
}

/**
 * rtspd rewrites /dev/shm/rtspd_last_snapshot_path (tmpfs, ns mtime) after
 * each snapshot. This is the robust "a new frame exists" signal - the JPEG
 * itself has second-granular names on vfat and can be overwritten in-place,
 * so we must not compare the image file's own mtime.
 */
function snapshot_signal(): int {
    clearstatcache();
    return (int)@filemtime(LAST_SNAPSHOT);
}

/** Newest file under /tmp/sd/RECORDED_IMAGES(/...), or null. */
function newest_file(string $base): ?string {
    if (!is_dir($base)) {
        return null;
    }
    try {
        $best = null;
        $bestTime = -1;
        $it = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($base));
        foreach ($it as $f) {
            if (!$f->isFile()) {
                continue;
            }
            $mt = @filemtime($f->getPathname());
            if ($mt !== false && $mt > $bestTime) {
                $bestTime = $mt;
                $best = $f->getPathname();
            }
        }
        return $best;
    } catch (Exception $e) {
        return null;
    }
}

/* ------------------------------------------------------------------ */
/* Endpoints                                                           */
/* ------------------------------------------------------------------ */

function ep_status() {
    json([
        'status' => 'ok',
        'camera' => [
            'nightmode' => ep_camera_mode('nightmode'),
            'ir_cut'    => ep_camera_mode('ir_cut'),
            'mirrormode' => ep_camera_mode('mirrormode'),
            'flipmode'  => ep_camera_mode('flipmode'),
        ],
        'led' => [
            'blue'   => ep_camera_mode('blue_led'),
            'yellow' => ep_camera_mode('yellow_led'),
            'ir'     => ep_camera_mode('ir_led'),
        ],
        'codec'  => ep_codec_status(),
        'motion' => ep_motion_status(),
        'image'  => ep_last_media('image'),
        'video'  => ep_last_media('video'),
    ]);
}

/**
 * Read one of the simple on/off style binaries.
 * Returns 1/0/unknown as a value (also used by ep_status).
 */
function ep_camera_mode(string $name) {
    $b = need_binary($name);
    $out = run_cmd([$b, '-s'], $rc);
    $txt = trim(implode(' ', $out));
    if (preg_match('/\b(on|1)\s*$/i', $txt)) {
        return 1;
    }
    if (preg_match('/\b(off|0)\s*$/i', $txt)) {
        return 0;
    }
    return 'unknown';
}

function ep_codec_status() {
    $b = need_binary('codec_ctrl');
    $out = run_cmd([$b, '-j', 'status'], $rc);
    $txt = trim(implode('', $out));
    $dec = json_decode($txt, true);
    return is_array($dec) ? $dec : null;
}

function ep_motion_status() {
    $b = need_binary('chuangmi_ctrl');
    $out = run_cmd([$b, 'GETMOTION'], $rc);
    $txt = trim(implode(' ', $out));
    return [
        'command' => $txt,
        'detected' => (strpos($txt, 'DETECTED') !== false),
    ];
}

function ep_last_media(string $kind): ?array {
    $dir = ($kind === 'image') ? IMAGES : VIDEOS;
    $lastFile = ($kind === 'image') ? LAST_SNAPSHOT : LAST_VIDEO;
    $path = null;
    if (is_file($lastFile)) {
        $p = trim((string)file_get_contents($lastFile));
        if ($p !== '' && $p !== 'unknown' && is_file($p)) {
            $path = $p;
        }
    }
    if ($path === null) {
        $path = newest_file($dir);
    }
    if ($path === null) {
        return null;
    }
    /* Map to a web-accessible URL via lighttpd aliases. */
    $rel = str_replace([IMAGES, VIDEOS], ['/snapshots/', '/videos/'], $path);
    return [
        'path' => $path,
        'url'  => $rel,
        'mtime' => @filemtime($path),
        'size'  => @filesize($path),
        'kind'  => $kind,
    ];
}

function ep_motor_status() {
    $b = need_binary('motor_ctrl');
    $out = run_cmd([$b, 'status', '-j'], $rc);
    $txt = trim(implode('', $out));
    $dec = json_decode($txt, true);
    if (!is_array($dec)) {
        fail('Failed to read motor status', 500);
    }
    json($dec);
}

function ep_config_read() {
    if (!is_file(CFG)) {
        fail('Config file not found', 404);
    }
    $lines = file(CFG, FILE_IGNORE_NEW_LINES);
    json([
        'keys'    => parse_kv($lines),
        'raw'     => implode("\n", $lines),
    ]);
}

function ep_config_write() {
    $raw = req('raw');
    if ($raw === null) {
        $keys = req('keys');
        if (!is_array($keys)) {
            fail('Provide either raw or keys');
        }
        /* If patch=1, merge the given keys into the existing config file
         * instead of replacing it wholesale (used by the Settings page so
         * a per-field save never wipes unrelated keys). */
        if (req('patch') === '1') {
            $cur = is_file(CFG) ? file(CFG, FILE_IGNORE_NEW_LINES) : [];
            $out = [];
            $seen = [];
            foreach ($cur as $line) {
                $t = trim($line);
                if (preg_match('/^([A-Za-z_][A-Za-z0-9_]*)\s*=/', $t, $m)) {
                    $k = $m[1];
                    if (array_key_exists($k, $keys)) {
                        $out[] = $k . '="' . $keys[$k] . '"';
                        $seen[$k] = true;
                        continue;
                    }
                }
                $out[] = $line;
            }
            foreach ($keys as $k => $v) {
                if (!isset($seen[$k])) {
                    $out[] = $k . '="' . $v . '"';
                }
            }
            $raw = implode("\n", $out);
            $raw = (string)$raw;
            goto write;
        }
        $raw = '';
        foreach ($keys as $k => $v) {
            $raw .= $k . '="' . $v . "\"\n";
        }
    }
    $raw = (string)$raw;
write:
    /* config.cfg is shell-sourced (see configupdate: `. /tmp/sd/config.old`),
     * so it must be valid sh: KEY="value" with NO whitespace around `=`.
     * Only comment/blank/KEY="value" lines are allowed; normalize by trimming. */
    $out = [];
    foreach (explode("\n", $raw) as $line) {
        $t = trim($line);
        if ($t === '') {
            $out[] = '';
            continue;
        }
        if (strpos($t, '#') === 0) {
            $out[] = $t;
            continue;
        }
        if (!preg_match('/^[A-Za-z_][A-Za-z0-9_]*=(?:"[^"\n]*"|[^\s"\n]*)$/', $t)) {
            fail('Invalid config line: ' . $line);
        }
        $out[] = $t;
    }
    $raw = implode("\n", $out);
    /* Backup then write. */
    @copy(CFG, CFG . '.bak');
    if (file_put_contents(CFG, $raw, LOCK_EX) === false) {
        fail('Failed to write config', 500);
    }
    ok(['written' => true]);
}

function ep_service(string $name, string $action) {
    $script = '/tmp/sd/firmware/etc/init/S99' . $name;
    if (!is_file($script)) {
        fail('Unknown service: ' . $name, 404);
    }
    if (!in_array($action, ['start', 'stop', 'restart', 'status'], true)) {
        fail('Invalid action: ' . $action);
    }
    $out = run_cmd(['/bin/sh', $script, $action], $rc);
    json(['service' => $name, 'action' => $action, 'rc' => $rc, 'output' => implode("\n", $out)]);
}

function ep_system() {
    $shell = function ($c) {
        return trim((string)shell_exec($c . ' 2>&1'));
    };
    $uptime = $shell('uptime');
    $mem = [];
    foreach (explode("\n", (string)file_get_contents('/proc/meminfo')) as $l) {
        if (preg_match('/^(\w+):\s+(\d+)/', $l, $m)) {
            $mem[$m[1]] = (int)$m[2];
        }
    }
    $ifconfig = $shell('ifconfig 2>/dev/null');
    $ip = '';
    $mac = '';
    foreach (explode("\n", $ifconfig) as $l) {
        if (preg_match('/inet addr:(\S+)/', $l, $m) && $m[1] !== '127.0.0.1') {
            $ip = $m[1];
        }
        if (preg_match('/HWaddr (\S+)/', $l, $m)) {
            $mac = $m[1];
        }
    }
    $net = [
        'ip' => $ip === '' ? null : $ip,
        'mac' => $mac === '' ? null : $mac,
        'gateway' => null,
    ];
    foreach (explode("\n", $shell('route -n 2>/dev/null')) as $l) {
        $f = preg_split('/\s+/', trim($l));
        if (isset($f[0], $f[1], $f[3]) && $f[0] === '0.0.0.0' && $f[3] === 'UG') {
            $net['gateway'] = $f[1];
        }
    }
    $iw = $shell('iwconfig 2>/dev/null');
    if (preg_match('/ESSID:"([^"]*)"/', $iw, $m)) {
        $net['essid'] = $m[1];
    }
    if (preg_match('/Bit Rate:([^ ]+)/', $iw, $m)) {
        $net['bitrate'] = $m[1];
    }
    if (preg_match('/Link Quality=([\d\/]+)/', $iw, $m)) {
        $parts = explode('/', $m[1]);
        $net['link_quality'] = isset($parts[1]) && (int)$parts[1] > 0
            ? round(100 * (int)$parts[0] / (int)$parts[1])
            : null;
        $net['link_quality_raw'] = $m[1];
    }
    if (preg_match('/Signal level=(-?\d+)/', $iw, $m)) {
        $net['signal_dbm'] = (int)$m[1];
    }
    if (preg_match('/Access Point: (\S+)/', $iw, $m)) {
        $net['ap'] = $m[1];
    }
    json([
        'hostname' => $shell('hostname'),
        'uptime'   => $uptime,
        'kernel'   => $shell('uname -r'),
        'arch'     => $shell('uname -m'),
        'ip'       => $net['ip'],
        'loadavg'  => $shell('cat /proc/loadavg'),
        'meminfo'  => $mem,
        'network'  => $net,
    ]);
}

function ep_sdcard() {
    $out = run_cmd(['df', '-h', '/tmp/sd'], $rc);
    json([
        'output' => implode("\n", $out),
        'images_dir' => is_dir(IMAGES),
        'videos_dir' => is_dir(VIDEOS),
        'last_image' => ep_last_media('image'),
        'last_video' => ep_last_media('video'),
    ]);
}

/* ------------------------------------------------------------------ */
/* Binary/streaming endpoints                                          */
/* ------------------------------------------------------------------ */

/**
 * Trigger (touch /dev/shm/rtspd_snapshot) and return a single fresh snapshot
 * JPEG (binary). rtspd's take_snapshot() blocks its media thread ~1-2s, so
 * triggers are coalesced via SNAP_LOCK: only one request per
 * SNAP_MIN_INTERVAL actually touches the trigger; the rest return the newest
 * image already on disk. This bounds rtspd to ~1 snapshot / interval and
 * prevents the watchdog reboot seen when concurrent snapshots queue up.
 */
function ep_snapshot() {
    $now = time();
    $last = (int)@file_get_contents(SNAP_LOCK);
    $triggered = false;
    $before = snapshot_signal();
    if ($now >= $last + SNAP_MIN_INTERVAL) {
        @file_put_contents(SNAP_LOCK, (string)$now, LOCK_EX);
        @touch(CTRL_SNAPSHOT);
        $triggered = true;
    }
    $path = null;
    if ($triggered) {
        for ($i = 0; $i < 25; $i++) { /* up to ~3s, matching rtspd throughput */
            if (snapshot_signal() > $before) {
                $path = last_snapshot_path();
                break;
            }
            usleep(120000); // 120ms
        }
    }
    if ($path === null) {
        $path = last_snapshot_path();
    }
    if ($path === null || !is_file($path)) {
        fail('No snapshot produced (is rtspd running?)', 500);
    }
    while (@ob_get_level()) {
        @ob_end_flush();
    }
    header('Content-Type: image/jpeg');
    header('Content-Length: ' . filesize($path));
    readfile($path);
    exit;
}

/* ------------------------------------------------------------------ */
/* Router                                                              */
/* ------------------------------------------------------------------ */

/* The lighttpd rewrite (-/api/(.*) -> /api/index.php) keeps the original URL
 * in REQUEST_URI but does not necessarily set PATH_INFO. We reconstruct the
 * resource path from the URI after /api/ so both setups work. */
$uri = (string)($_SERVER['REQUEST_URI'] ?? '');
$queryPos = strpos($uri, '?');
if ($queryPos !== false) {
    $uri = substr($uri, 0, $queryPos);
}
$pos = strpos($uri, '/api/');
$resourcePath = $pos !== false ? substr($uri, $pos + 5) : $uri;
$path = ltrim($resourcePath, '/');
if ($path === 'index.php') {
    $path = '';
} elseif (strpos($path, 'index.php/') === 0) {
    $path = substr($path, strlen('index.php/'));
}
$path = rtrim($path, '/');
$segments = $path === '' ? [] : explode('/', $path);
$resource = $segments[0] ?? '';

$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';

try {
    switch ($resource) {
        case '': // /api/ -> status
        case 'status':
            ep_status();
            break;

        case 'snapshot':
            ep_snapshot();
            break;

        case 'codec':
            $b = need_binary('codec_ctrl');
            $sub = $segments[1] ?? 'status';
            if ($sub === 'status') {
                json(['codec' => ep_codec_status()]);
            } elseif (in_array($sub, ['bitrate', 'fps', 'gop', 'mode'], true)) {
                $val = req('value', sget('value'));
                if ($val === null) {
                    fail('Missing value');
                }
                $out = run_cmd([$b, $sub, (string)$val], $rc);
                if ($rc !== 0) {
                    fail('codec_ctrl ' . $sub . ' failed: ' . implode(' ', $out), 500);
                }
                json(['set' => $sub, 'value' => $val, 'output' => implode("\n", $out)]);
            } else {
                fail('Unknown codec action: ' . $sub, 404);
            }
            break;

        case 'keyframe':
            $b = need_binary('codec_ctrl');
            run_cmd([$b, 'keyframe'], $rc);
            json(['keyframe' => true]);
            break;

        case 'motor':
            $b = need_binary('motor_ctrl');
            $cmd = $segments[1] ?? 'status';
            switch ($cmd) {
                case 'status':
                    ep_motor_status();
                    break;
                case 'left':
                case 'right':
                case 'up':
                case 'down':
                case 'home':
                    $steps = req('steps', sget('steps', '1'));
                    run_cmd([$b, $cmd, (string)$steps], $rc);
                    json(['motor' => $cmd, 'steps' => $steps, 'rc' => $rc]);
                case 'goto':
                    $x = req('x', sget('x'));
                    $y = req('y', sget('y'));
                    if ($x === null || $y === null) {
                        fail('Missing x/y');
                    }
                    run_cmd([$b, 'goto', (string)$x, (string)$y], $rc);
                    json(['motor' => 'goto', 'x' => $x, 'y' => $y, 'rc' => $rc]);
                case 'stop':
                    /* motor_ctrl has no stop; use home as safe fallback. */
                    run_cmd([$b, 'home'], $rc);
                    json(['motor' => 'home', 'rc' => $rc]);
                default:
                    fail('Unknown motor command: ' . $cmd, 404);
            }
            break;

        case 'camera':
            $b = need_binary('camera_adjust');
            $sub = $segments[1] ?? 'info';
            if ($sub === 'info') {
                $out = run_cmd([$b, '-j'], $rc);
                $dec = json_decode(trim(implode('', $out)), true);
                json(['camera' => is_array($dec) ? $dec : null]);
            } else {
                $type = $sub; // brightness|contrast|hue|...
                $val = req('value', sget('value'));
                if ($val === null) {
                    fail('Missing value for ' . $type);
                }
                run_cmd([$b, '-s', (string)$val, '-t', $type], $rc);
                json(['camera' => $type, 'value' => $val, 'rc' => $rc]);
            }
            break;

        case 'mode':
            /* Generic on/off toggle: /api/mode/<name>/<action> */
            $name = $segments[1] ?? null;
            $action = $segments[2] ?? req('action', sget('action'));
            if ($name === null || !in_array($name, ['nightmode', 'ir_cut', 'ir_led', 'blue_led', 'yellow_led', 'mirrormode', 'flipmode'], true)) {
                fail('Unknown mode: ' . ($name ?? 'none'), 404);
            }
            $b = need_binary($name);
            if ($action === 'on' || $action === 'enable') {
                run_cmd([$b, '-e'], $rc);
            } elseif ($action === 'off' || $action === 'disable') {
                run_cmd([$b, '-d'], $rc);
            } else {
                fail('Invalid action: ' . $action);
            }
            json(['mode' => $name, 'state' => $action, 'rc' => $rc]);
            break;

        case 'config':
            if ($method === 'POST') {
                ep_config_write();
            }
            ep_config_read();
            break;

        case 'service':
            $name = $segments[1] ?? null;
            $action = $segments[2] ?? req('action', sget('action'));
            if ($name === null || $action === null) {
                fail('Usage: /api/service/<name>/<start|stop|restart|status>');
            }
            ep_service($name, $action);
            break;

        case 'services':
            /* List all managed services + running state. */
            $list = [
                'rtsp', 'lighttpd', 'dropbear', 'telnet', 'ftpd',
                'crond', 'mqtt-control', 'mqtt-interval', 'restartd',
                'auto_night_mode', 'restore_state',
            ];
            $res = [];
            foreach ($list as $s) {
                $script = '/tmp/sd/firmware/etc/init/S99' . $s;
                if (!is_file($script)) {
                    continue;
                }
                $out = run_cmd(['/bin/sh', $script, 'status'], $rc);
                $res[$s] = [
                    'running' => (stripos(implode(' ', $out), 'running') !== false),
                    'output'  => trim(implode(' ', $out)),
                ];
            }
            json(['services' => $res]);
            break;

        case 'system':
            ep_system();
            break;

        case 'sdcard':
            ep_sdcard();
            break;

        case 'last':
            $kind = $segments[1] ?? 'image';
            json(['media' => ep_last_media($kind)]);
            break;

        default:
            fail('Unknown resource: ' . $resource, 404);
    }
} catch (Throwable $e) {
    fail($e->getMessage(), 500);
}
