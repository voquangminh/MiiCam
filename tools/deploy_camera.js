const { Client } = require('ssh2');
const fs = require('fs');
const path = require('path');

const HOST = 'paladin0807.ddns.net';
const PORT = 2235;
const USERNAME = 'root';
const PASSWORD = 'langthan';
const REMOTE_SCRIPTS = '/tmp/sd/firmware/scripts';
const REMOTE_LIB = '/tmp/sd/firmware/lib';

const BINS = [
    'chuangmi_ctrl',
    'codec_ctrl',
    'motor_ctrl',
    'camera_adjust',
    'rtspd',
];

const LIBS = [
    'libchuangmi_codec.so',
    'libchuangmi_sensor.so',
    'libchuangmi_isp328.so',
];

const LOCAL_BIN = path.join(__dirname, 'bin');
const LOCAL_LIB = path.join(__dirname, 'lib');

function runShell(conn, cmd) {
    return new Promise((resolve, reject) => {
        conn.exec(cmd, (err, stream) => {
            if (err) return reject(err);
            let out = '';
            stream.on('close', () => resolve(out));
            stream.stdout.on('data', d => { out += d.toString(); process.stdout.write(d); });
            stream.stderr.on('data', d => process.stderr.write(d));
        });
    });
}

function uploadFile(conn, localPath, remotePath) {
    return new Promise((resolve, reject) => {
        const data = fs.readFileSync(localPath);
        const fname = path.basename(remotePath);
        process.stdout.write(`  ${fname} (${data.length} bytes)... `);
        conn.exec(`dd of=${remotePath} bs=4096 2>/dev/null; echo DD:$?`, (err, stream) => {
            if (err) return reject(err);
            let exited = false;
            stream.on('close', () => { if (!exited) { exited = true; console.log('OK'); resolve(); } });
            stream.stdout.on('data', d => {});
            const CHUNK = 16384;
            let offset = 0;
            function writeChunk() {
                if (offset >= data.length) { stream.stdin.end(); return; }
                const end = Math.min(offset + CHUNK, data.length);
                const ok = stream.stdin.write(data.slice(offset, end));
                offset = end;
                if (ok) writeChunk(); else stream.stdin.once('drain', writeChunk);
            }
            writeChunk();
        });
    });
}

async function main() {
    const conn = new Client();
    await new Promise((resolve, reject) => {
        conn.on('ready', resolve);
        conn.on('error', reject);
        conn.connect({
            host: HOST, port: PORT, username: USERNAME, password: PASSWORD,
            readyTimeout: 15000,
            algorithms: { kex: ['diffie-hellman-group14-sha1', 'diffie-hellman-group14-sha256', 'diffie-hellman-group1-sha1', 'ecdh-sha2-nistp256'] },
        });
    });
    console.log('=== SSH connected ===\n');

    console.log('--- Uploading binaries to', REMOTE_SCRIPTS, '---');
    for (const f of BINS) {
        try { await uploadFile(conn, path.join(LOCAL_BIN, f), `${REMOTE_SCRIPTS}/${f}`); await runShell(conn, `chmod 755 ${REMOTE_SCRIPTS}/${f}`); }
        catch (e) { console.error(`  FAILED ${f}: ${e.message}`); }
    }

    console.log('\n--- Uploading libs to', REMOTE_LIB, '---');
    for (const f of LIBS) {
        try { await uploadFile(conn, path.join(LOCAL_LIB, f), `${REMOTE_LIB}/${f}`); await runShell(conn, `chmod 755 ${REMOTE_LIB}/${f}`); }
        catch (e) { console.error(`  FAILED ${f}: ${e.message}`); }
    }

    console.log('\n--- Verify ---');
    await runShell(conn, `ls -la ${REMOTE_SCRIPTS}/chuangmi_ctrl ${REMOTE_SCRIPTS}/codec_ctrl ${REMOTE_SCRIPTS}/motor_ctrl`);
    await runShell(conn, `ls -la ${REMOTE_LIB}/libchuangmi_codec.so ${REMOTE_LIB}/libchuangmi_sensor.so`);

    console.log('\n--- Test: codec_ctrl status ---');
    await runShell(conn, `LD_LIBRARY_PATH=${REMOTE_LIB} ${REMOTE_SCRIPTS}/codec_ctrl status`);

    console.log('\n--- Test: chuangmi_ctrl GETSTAT ---');
    await runShell(conn, `LD_LIBRARY_PATH=${REMOTE_LIB} ${REMOTE_SCRIPTS}/chuangmi_ctrl GETSTAT`);

    console.log('\n--- Test: motor_ctrl status ---');
    await runShell(conn, `LD_LIBRARY_PATH=${REMOTE_LIB} ${REMOTE_SCRIPTS}/motor_ctrl status`);

    conn.end();
    console.log('\n=== Done ===');
}

main().catch(e => { console.error('Fatal:', e.message); process.exit(1); });
